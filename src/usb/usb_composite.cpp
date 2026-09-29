#include "usb_composite.h"
#include "uac_microphone.h"
#include "audio/audio_pipeline.h"
#include "log/app_log.h"
#include "led_indicator.h"
#include "wifi/wifi_manager.h"
#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "tusb.h"
#include "esp32-hal-tinyusb.h"
#include "esp_system.h"
#include <Preferences.h>

#if !ARDUINO_USB_CDC_ON_BOOT
USBCDC USBSerial;
#endif

static USBHIDKeyboard        s_keyboard;
static USBHIDConsumerControl s_consumer;
static bool                  s_usb_ready = false;

extern "C" void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr);
extern "C" bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr);

#include "soc/usb_struct.h"

static bool                  s_hw_sleep_detected    = false;
static uint32_t              s_hw_sleep_start_ms    = 0;
static uint16_t              s_last_soffn           = 0;
static uint32_t              s_last_soffn_change_ms = 0;
static uint32_t              s_boot_grace_until_ms  = 0;
static uint32_t              s_waiting_reconnect_ms = 0; // non-zero while we await soft re-enumeration

static SemaphoreHandle_t    s_hid_mutex = NULL;

// Non-recursive mutex guarding the USB HID reports + registry. Defined here in
// plain C++ (file-local helpers): the guard section below lives outside the
// extern "C" block, so keeping the same linkage avoids a C/C++ conflict.
static void hid_lock(void) {
    if (!s_hid_mutex) {
        s_hid_mutex = xSemaphoreCreateMutex();
    }
    if (s_hid_mutex) {
        xSemaphoreTake(s_hid_mutex, portMAX_DELAY);
    }
}

static void hid_unlock(void) {
    if (s_hid_mutex) {
        xSemaphoreGive(s_hid_mutex);
    }
}

// ===========================================================================
// Stuck-Key Guard (Anti-Stuck Safety Net)
// ---------------------------------------------------------------------------
// Holds are registered when a hold-style action is emitted and cleared on
// release / force-release. Rules run from usb_composite_task() and, when one
// trips, force a fully consistent release (engine + USB report + audio + LED).
// ===========================================================================
#define HID_HELD_MAX 8
typedef struct {
    uint8_t  modifier;   // 0 = none
    uint8_t  key_code;   // 0 = none
    uint16_t consumer;   // 0 = none
    bool     voice;      // true = part of a voice hold (exempt from normal rules)
    bool     pressed;
    uint32_t since_ms;
} hid_held_entry_t;

static hid_held_entry_t s_held[HID_HELD_MAX];
static uint32_t         s_last_output_ms          = 0;
static uint32_t         s_last_guard_check_ms     = 0;
static bool             s_guard_inited            = false;
static uint32_t         s_guard_mod_ms            = HID_GUARD_MOD_SOLO_MS;
static uint32_t         s_guard_key_ms            = HID_GUARD_KEY_IDLE_MS;
static uint32_t         s_guard_voice_ms          = HID_GUARD_VOICE_EXTREME_MS;
static uint32_t         s_force_release_count     = 0;
static uint32_t         s_last_force_ms           = 0;
static char             s_last_force_reason[24]   = "";

// Bounded report re-assertion.
//
// A crash or watchdog reset can leave the host holding a modifier-down state it
// never sees released, and Windows applies that modifier to *every* keyboard, so
// the user's own physical keyboard types garbage. The guard above cannot cover
// this: it only runs while the firmware is alive, and dying is exactly when it
// stops helping.
//
// Re-enumeration normally repairs the host on its own (the mount path sends an
// all-zero report), so a permanent heartbeat would be both wasteful and a smell:
// it would also mask any future unbalanced-report bug. Instead we re-assert
// only inside a short window right after an event that could plausibly have
// desynchronised the host, then stop for good.
#define USB_REASSERT_WINDOW_MS 10000
#define USB_REASSERT_PERIOD_MS 500
static uint32_t         s_reassert_until_ms        = 0;  // 0 = window closed
static uint32_t         s_reassert_last_ms         = 0;

// Arm the window. Called from the paths where the host may have missed (or never
// received) a release report.
static void reassert_arm(void) {
    const uint32_t now = millis();
    s_reassert_until_ms = now + USB_REASSERT_WINDOW_MS;
    s_reassert_last_ms  = now;
}

// Per-release re-assertion.
//
// Observed failure: `mod=0x08` (LeftAlt) stayed held on the host after the remote
// key came up. The Arduino USBHIDKeyboard::sendReport() helper is `void` and
// forwards to HID().SendReport(), whose bool it discards, and `hid` is a *private*
// member, so we cannot observe whether the host actually accepted a report. That
// report can legitimately fail: SendReport() returns false when the endpoint is
// still busy, or when the PC has stopped polling (NAK) - exactly what happens
// while the screen blanks or the remote is released near a suspend.
//
// A tap presses and releases ~15 ms apart, so the press is usually still in
// flight when the release is submitted, and a busy endpoint is the common case
// rather than the exotic one. When that release is dropped nothing in the design
// notices: the registry is cleared locally, the host keeps the modifier, and the
// only thing that eventually repairs it is the modifier guard after 20 s - by
// which time Windows has applied Alt to every keystroke on the machine.
//
// We cannot see the bool, but we do have one piece of positive evidence: the
// host draining the interrupt IN endpoint means the report was actually taken.
// So we re-assert until the endpoint is observed idle after a send, and treat
// that - not a fixed number of retries - as delivery. Repeats stop as soon as
// anything is genuinely held again, so this can never truncate a real keypress.
#define USB_HID_EP_IN 0x81
#define USB_RELEASE_REASSERT_WINDOW_MS 1000
#define USB_RELEASE_REASSERT_PERIOD_MS 15
static uint32_t         s_release_owed_until_ms    = 0;  // 0 = not owed
static uint32_t         s_release_owed_last_ms     = 0;
static uint32_t         s_release_owed_modifier    = 0;  // for the forensics log
static uint32_t         s_release_owed_sent        = 0;
static bool             s_release_owed_drained     = false;
static uint32_t         s_release_slow_count       = 0;  // windows needing many repeats
static uint8_t          s_last_modifier_down       = 0;

// Caller must hold hid_lock().
static void hid_send_clear_locked(void) {
    KeyReport clear_report = {0};
    s_keyboard.sendReport(&clear_report);
}

// Arm the per-release re-assertion window. Caller must hold hid_lock().
static void release_owed_arm_locked(uint8_t modifier) {
    const uint32_t now = millis();
    s_release_owed_until_ms = now + USB_RELEASE_REASSERT_WINDOW_MS;
    s_release_owed_last_ms  = now;
    s_release_owed_modifier = modifier;
    s_release_owed_sent     = 0;
    s_release_owed_drained  = false;
}

static void guard_load_config(void) {
    if (s_guard_inited) return;
    s_guard_inited = true;
    Preferences p;
    if (p.begin("guard_conf", true)) {
        s_guard_mod_ms   = p.getUInt("mod_ms", HID_GUARD_MOD_SOLO_MS);
        s_guard_key_ms   = p.getUInt("key_ms", HID_GUARD_KEY_IDLE_MS);
        s_guard_voice_ms = p.getUInt("voice_ms", HID_GUARD_VOICE_EXTREME_MS);
        p.end();
    }
}

static void guard_add(uint8_t modifier, uint8_t key_code, uint16_t consumer, bool voice) {
    uint32_t now = millis();
    for (int i = 0; i < HID_HELD_MAX; i++) {
        hid_held_entry_t *e = &s_held[i];
        if (!e->pressed) {
            e->modifier = modifier;
            e->key_code = key_code;
            e->consumer = consumer;
            e->voice    = voice;
            e->pressed  = true;
            e->since_ms = now;
            return;
        }
    }
    // Registry full: force everything out rather than risk a stuck key.
    usb_composite_force_release_all("registry-full");
}

static void guard_clear_nonvoice(void) {
    for (int i = 0; i < HID_HELD_MAX; i++) {
        hid_held_entry_t *e = &s_held[i];
        if (e->pressed && !e->voice) {
            e->pressed  = false;
            e->modifier = 0;
            e->key_code = 0;
            e->consumer = 0;
        }
    }
}

static void guard_clear_voice(void) {
    for (int i = 0; i < HID_HELD_MAX; i++) {
        hid_held_entry_t *e = &s_held[i];
        if (e->pressed && e->voice) {
            e->pressed  = false;
            e->modifier = 0;
            e->key_code = 0;
            e->consumer = 0;
        }
    }
}

static void guard_clear_all(void) {
    for (int i = 0; i < HID_HELD_MAX; i++) {
        s_held[i].pressed  = false;
        s_held[i].modifier = 0;
        s_held[i].key_code = 0;
        s_held[i].consumer = 0;
        s_held[i].voice    = false;
    }
}

static bool guard_has_held(void) {
    for (int i = 0; i < HID_HELD_MAX; i++) {
        if (s_held[i].pressed) return true;
    }
    return false;
}

// Periodic stuck-key rule evaluation (called from usb_composite_task).
static void guard_tick(uint32_t now) {
    guard_load_config();

    if ((now - s_last_guard_check_ms) < 250) {
        return;
    }
    s_last_guard_check_ms = now;

    bool any_held     = false;
    bool any_voice    = false;
    uint32_t first_ms      = 0; // earliest press among registered holds
    uint32_t first_voice_ms = 0;
    for (int i = 0; i < HID_HELD_MAX; i++) {
        const hid_held_entry_t *e = &s_held[i];
        if (!e->pressed) continue;
        if (e->voice) {
            any_voice = true;
            if (first_voice_ms == 0 || e->since_ms < first_voice_ms) first_voice_ms = e->since_ms;
        } else {
            any_held = true;
            if (first_ms == 0 || e->since_ms < first_ms) first_ms = e->since_ms;
        }
    }

    // Rule V: absolute ceiling for a voice recording (exempt from normal rules).
    if (any_voice && s_guard_voice_ms && (now - first_voice_ms >= s_guard_voice_ms)) {
        usb_composite_force_release_all("voice-extreme");
        return;
    }

    // Rule M: any modifier held continuously past the ceiling, regardless of
    // whatever else is going on (this is the classic "PC thinks Alt is down").
    for (int i = 0; i < HID_HELD_MAX; i++) {
        const hid_held_entry_t *e = &s_held[i];
        if (!e->pressed || e->voice || e->modifier == 0) continue;
        if (s_guard_mod_ms && (now - e->since_ms >= s_guard_mod_ms)) {
            usb_composite_force_release_all("modifier-hold");
            return;
        }
    }

    // Rule K: a non-voice key held down with zero further output -> release.
    if (any_held && s_guard_key_ms && (now - s_last_output_ms >= s_guard_key_ms)) {
        usb_composite_force_release_all("key-idle");
    }
}

bool usb_composite_guard_get(usb_guard_config_t *cfg, usb_guard_stats_t *stats) {
    guard_load_config();
    if (cfg) {
        cfg->mod_ms   = s_guard_mod_ms;
        cfg->key_ms   = s_guard_key_ms;
        cfg->voice_ms = s_guard_voice_ms;
    }
    if (stats) {
        stats->forced_releases = s_force_release_count;
        stats->last_force_ms   = s_last_force_ms;
        snprintf(stats->last_reason, sizeof(stats->last_reason), "%s", s_last_force_reason);
        stats->any_held  = false;
        stats->held_count = 0;
        for (int i = 0; i < HID_HELD_MAX; i++) {
            if (s_held[i].pressed) {
                stats->any_held = true;
                stats->held_count++;
            }
        }
    }
    return true;
}

bool usb_composite_guard_set(const usb_guard_config_t *cfg) {
    if (!cfg) return false;
    guard_load_config();
    s_guard_mod_ms   = cfg->mod_ms;
    s_guard_key_ms   = cfg->key_ms;
    s_guard_voice_ms = cfg->voice_ms;
    Preferences p;
    if (!p.begin("guard_conf", false)) {
        return false;
    }
    bool ok = p.putUInt("mod_ms", s_guard_mod_ms)   != 0;
    ok      = p.putUInt("key_ms", s_guard_key_ms)   != 0 && ok;
    ok      = p.putUInt("voice_ms", s_guard_voice_ms) != 0 && ok;
    p.end();
    app_log("GUARD", "Config updated: mod=%ums key=%ums voice=%ums",
            (unsigned)s_guard_mod_ms, (unsigned)s_guard_key_ms, (unsigned)s_guard_voice_ms);
    return ok;
}

void usb_composite_force_release_all(const char* reason) {
    // Nothing held and nothing recording -> this is a no-op (e.g. the USB bus
    // stops during the very first enumeration at boot). Don't count/flash.
    if (!guard_has_held() && !g_audio_pipeline.active) return;

    s_force_release_count++;
    s_last_force_ms = millis();
    snprintf(s_last_force_reason, sizeof(s_last_force_reason), "%s", reason);
    app_log("GUARD", "Forced HID release (reason=%s, count=%u)",
            reason, (unsigned)s_force_release_count);

    // 1. Stop any running voice/audio session first (engine release will emit
    //    a VOICE_RELEASE but the session must not outlive the forced release).
    if (g_audio_pipeline.active) {
        audio_pipeline_stop_session(&g_audio_pipeline);
    }

    // 2. Let the key engine release every slot: it emits the proper RELEASE
    //    actions and clears internal press state consistently.
    extern key_mapper_engine_t g_key_engine;
    key_engine_release_all(&g_key_engine, millis());

    // 3. Hard-clear the USB reports so the PC can never see a ghost key/modifier.
    hid_lock();
    if (s_usb_ready) {
        hid_send_clear_locked();
        s_keyboard.releaseAll();
        s_consumer.release();
    }
    release_owed_arm_locked(s_last_modifier_down);
    s_last_modifier_down = 0;
    hid_unlock();

    // 4. Registry is now empty regardless.
    guard_clear_all();

    // 5. The host is now receiving a clean report; keep re-asserting briefly in
    //    case a release was lost in flight, then stop.
    reassert_arm();

    // 6. Visual: red flash so the user sees the guard fired, then normal status.
    led_indicator_trigger_stuck();
    led_indicator_set(LED_STATE_CONNECTED);
}

extern "C" {

void usb_composite_init(void) {
    USB.VID(0x303A);
    USB.PID(0x8089);
    USB.productName("RemoteMapper Audio & Remote Bridge");
    USB.manufacturerName("RemoteMapper");
    USB.serialNumber("RM-ESP32S3-MIC02");
    USB.usbClass(0xEF);
    USB.usbSubClass(0x02);
    USB.usbProtocol(0x01); // MISC_PROTOCOL_IAD
    USB.usbAttributes(TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);

    USB.onEvent([](void* arg, esp_event_base_t base, int32_t id, void* data) {
        if (id == ARDUINO_USB_SUSPEND_EVENT) {
            app_log("USB", "USB Suspend Event (PC Sleep/Standby)");
            // PC cannot see our state while asleep: drop everything we hold so a
            // ghost key never survives the sleep/wake cycle.
            usb_composite_force_release_all("usb-suspend");
        } else if (id == ARDUINO_USB_RESUME_EVENT) {
            app_log("USB", "USB Resume Event (PC Woke up)");
            for (uint8_t ep = 1; ep <= 4; ep++) {
                usbd_edpt_clear_stall(0, (uint8_t)(ep | 0x80));
            }
            s_keyboard.releaseAll();
            s_consumer.release();
            release_owed_arm_locked(s_last_modifier_down);
            s_last_modifier_down = 0;
            guard_clear_all();
            reassert_arm();   // host may have missed a release across suspend
        } else if (id == ARDUINO_USB_STARTED_EVENT) {
            s_waiting_reconnect_ms = 0; // soft re-enumeration succeeded
            app_log("USB", "USB Started / Mounted");
            for (uint8_t ep = 1; ep <= 4; ep++) {
                usbd_edpt_clear_stall(0, (uint8_t)(ep | 0x80));
            }
            // Push an all-zero report to the host as the very first thing after
            // (re-)enumeration. A crash or watchdog reset can leave the PC
            // holding a modifier-down state that it never sees released, and
            // Windows applies a stuck modifier from this HID keyboard to *every*
            // keyboard on the system, which reads to the user as their own
            // physical keyboard typing garbage. Clearing our internal registry
            // is not enough: the host has to be told explicitly.
            hid_lock();
            if (s_usb_ready) {
                hid_send_clear_locked();
                s_keyboard.releaseAll();
                s_consumer.release();
            }
            release_owed_arm_locked(s_last_modifier_down);
            s_last_modifier_down = 0;
            hid_unlock();
            guard_clear_all();
            reassert_arm();
            // Host (re-)enumerated us: boot, PC reboot, or Device Manager
            // re-enable. The user is at the PC, so make the web UI reachable
            // in case the on-demand idle timeout had powered the radio down.
            // Deferred wake: processed in the main-loop context, race-free.
            wifi_manager_notify_usb_mounted();
        } else if (id == ARDUINO_USB_STOPPED_EVENT) {
            app_log("USB", "USB Stopped / Bus Reset");
            usb_composite_force_release_all("usb-stopped");
        }
    });

#if !ARDUINO_USB_CDC_ON_BOOT
    USBSerial.begin();
#endif

    uac_microphone_init();

    s_keyboard.begin();
    s_consumer.begin();
    USB.begin();
    s_usb_ready = true;
}

void usb_composite_task(void) {
    uac_microphone_task();

    uint32_t now = millis();
    guard_tick(now);

    // Per-release re-assertion. Runs after every release and keeps re-sending the
    // all-zero report until the host is observed to have drained the interrupt
    // endpoint, so a release the host never accepted is repaired in milliseconds
    // rather than waiting out the 20 s modifier guard. Gated on nothing being
    // held, so a fast follow-up keypress always wins.
    if (s_release_owed_until_ms == 0) {
        // nothing owed
    } else if (guard_has_held()) {
        // real key down: the new press supersedes the owed clear
        s_release_owed_until_ms = 0;
    } else if (now - s_release_owed_last_ms >= USB_RELEASE_REASSERT_PERIOD_MS) {
        if (now >= s_release_owed_until_ms) {
            // Window closed before we saw a drain. That is the interesting case:
            // the host was not taking reports, so the release may well be lost.
            s_release_owed_until_ms = 0;
            if (s_release_owed_sent > 0) {
                s_release_slow_count++;
                app_log("USB_HID", "!! release NOT confirmed after %u sends (mod=0x%02X) slow=%u",
                        (unsigned)s_release_owed_sent,
                        (unsigned)s_release_owed_modifier,
                        (unsigned)s_release_slow_count);
            }
        } else {
            s_release_owed_last_ms = now;
            hid_lock();
            if (s_usb_ready) {
                hid_send_clear_locked();
                s_release_owed_sent++;
                s_release_owed_drained = !usbd_edpt_busy(0, USB_HID_EP_IN);
            }
            hid_unlock();
            // Drain observed after at least one round: the host has the report.
            if (s_release_owed_drained && s_release_owed_sent >= 2) {
                s_release_owed_until_ms = 0;
            }
        }
    }

    // Re-assert a clean keyboard report, but only inside the bounded window
    // armed by mount/resume/force-release. Gated on nothing being held so we can
    // never truncate a real keypress, and it shuts itself off once the window
    // closes rather than running forever.
    if (s_reassert_until_ms == 0) {
        // window closed: nothing to do
    } else if (now >= s_reassert_until_ms) {
        s_reassert_until_ms = 0;
    } else if (guard_has_held()) {
        s_reassert_last_ms = now;  // real key down: pause, don't interfere
    } else if (now - s_reassert_last_ms >= USB_REASSERT_PERIOD_MS) {
        s_reassert_last_ms = now;
        hid_lock();
        if (s_usb_ready) {
            hid_send_clear_locked();
        }
        hid_unlock();
    }

    // Soft re-enumeration safety net: if the host did not re-mount us within
    // 4s of the D+/D- detach, fall back to a full restart (today's behavior).
    if (s_waiting_reconnect_ms != 0 && (now - s_waiting_reconnect_ms >= 4000)) {
        s_waiting_reconnect_ms = 0;
        app_log("USB_HW", "Soft re-enumeration timed out -> falling back to full restart");
        esp_restart();
    }
    if (s_boot_grace_until_ms == 0) {
        s_boot_grace_until_ms = now + 5000; // 5s boot grace period
        s_last_soffn_change_ms = now;
    }
    if (now < s_boot_grace_until_ms) {
        return; // Don't check during initial bootup
    }

    // Read hardware DWC2 register on ESP32-S3
    uint32_t dsts = USB0.dsts;
    bool is_hw_suspended = (dsts & 1) != 0; // Bit 0: SuspSts (1 = Suspended)
    uint16_t current_soffn = (dsts >> 8) & 0x3FFF; // Bits 8-21: Frame Number (increments every 1ms when PC is awake)

    if (current_soffn != s_last_soffn) {
        s_last_soffn = current_soffn;
        s_last_soffn_change_ms = now;
    }

    // If SOF packets stopped for > 800ms, Windows is asleep/disconnected
    bool sof_active = (now - s_last_soffn_change_ms < 800);

    // 1. Detect PC entering Sleep
    if ((is_hw_suspended || !sof_active) && !s_hw_sleep_detected) {
        s_hw_sleep_detected = true;
        s_hw_sleep_start_ms = now;
        app_log("USB_HW", "PC Sleep Detected! (SuspSts=%d, SOF stopped)", is_hw_suspended ? 1 : 0);
    }
    // 2. Detect PC waking up (SOF resumed after sleeping for > 2s)
    else if (s_hw_sleep_detected && !is_hw_suspended && sof_active) {
        if (now - s_hw_sleep_start_ms >= 2000) {
            app_log("USB_HW", "PC Wakeup Detected! SOF resumed -> soft USB re-enumeration (no reboot)...");
            vTaskDelay(pdMS_TO_TICKS(500));
            // Signal a disconnect to the Windows kernel by pulling D+(20)/D-(19).
            // The 350ms SE0 forces Windows to drop and re-enumerate the device.
            pinMode(20, OUTPUT);
            pinMode(19, OUTPUT);
            digitalWrite(20, LOW);
            digitalWrite(19, LOW);
            vTaskDelay(pdMS_TO_TICKS(350));
            // Release the pads back to the internal USB PHY (hi-Z GPIO no longer
            // overrides them); the D+ pull-up reappears and the host sees a fresh attach.
            pinMode(20, INPUT);
            pinMode(19, INPUT);
            s_hw_sleep_detected = false;
            s_waiting_reconnect_ms = millis();
            app_log("USB_HW", "D+/D- released -> waiting for host re-enumeration (4s fallback before restart)");
        } else {
            s_hw_sleep_detected = false;
        }
    }
}

bool usb_hid_keyboard_press(uint8_t modifier, uint8_t keycode) {
    if (!s_usb_ready) return false;
    hid_lock();

    if (s_hw_sleep_detected || (USB0.dsts & 1) != 0 || tud_suspended()) {
        app_log("USB_HW", "Key pressed while PC asleep -> Sending Remote Wakeup!");
        tud_remote_wakeup();
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    KeyReport report = {0};
    report.modifiers = modifier;
    report.keys[0] = keycode;
    s_keyboard.sendReport(&report);

    // A new key down supersedes any owed clear: the re-assert window must never
    // be able to truncate a press. A tap is not in the guard registry, so the
    // task loop cannot infer this on its own - disarm it here explicitly.
    s_release_owed_until_ms = 0;

    // Forensics for the "PC thinks Alt is down" failure mode. Any report with a
    // non-zero modifier byte is the only way this device can make an unrelated
    // physical keyboard misbehave, so log those loudly and leave an
    // unmistakable trail in the ring buffer for the next occurrence.
    if (modifier != 0) {
        // Remember which modifier went down: if the matching release is dropped,
        // the per-release re-assert window can name the culprit in the log.
        s_last_modifier_down = modifier;
        app_log("USB_HID", "!! MODIFIER DOWN mod=0x%02X key=0x%02X (guard=%ums)",
                (unsigned)modifier, (unsigned)keycode, (unsigned)s_guard_mod_ms);
    } else {
        app_log("USB_HID", "press key=0x%02X", (unsigned)keycode);
    }

    hid_unlock();
    return true;
}

bool usb_hid_keyboard_release(void) {
    if (!s_usb_ready) return false;
    hid_lock();

    // Arm the re-assert window first so that even if this report is dropped the
    // task loop keeps pushing clears until the host has certainly seen one.
    release_owed_arm_locked(s_last_modifier_down);
    s_last_modifier_down = 0;

    // One all-zero report is enough. The previous code additionally called
    // releaseAll(), which sent a second identical report through the same
    // fire-and-forget path - it doubled the odds of a NAK without adding any
    // guarantee, since a dropped release stays dropped either way.
    hid_send_clear_locked();

    hid_unlock();
    return true;
}

bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode) {
    if (!s_usb_ready) return false;
    usb_hid_keyboard_press(modifier, keycode);
    delay(15);
    usb_hid_keyboard_release();
    return true;
}

bool usb_hid_consumer_press(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    hid_lock();

    if (tud_suspended()) {
        app_log("USB", "PC Suspended -> tud_remote_wakeup");
        tud_remote_wakeup();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_consumer.press(usage_code);

    hid_unlock();
    return true;
}

bool usb_hid_consumer_release(void) {
    if (!s_usb_ready) return false;
    hid_lock();

    s_consumer.release();

    hid_unlock();
    return true;
}

bool usb_hid_consumer_tap(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    usb_hid_consumer_press(usage_code);
    delay(15);
    usb_hid_consumer_release();
    return true;
}

void usb_hid_dispatch_action(const key_action_t *action) {
    if (!action) return;

    // Every emitted action counts as output activity for the stuck-key rules.
    s_last_output_ms = millis();

    // User input feeds ON_DEMAND power management: keeps the radio alive, or
    // triggers the 5-press-of-same-key wake gesture after an idle power-down.
    uint16_t gesture_key = (uint16_t)(((uint16_t)action->type << 8) |
                                      (action->consumer_code != 0 ? action->consumer_code : action->key_code));
    wifi_manager_notify_key_press(gesture_key);

    app_log("USB_HID", "Emit Action: type=%d, mod=0x%02X, key=0x%02X, cons=0x%04X", 
            action->type, action->modifier, action->key_code, action->consumer_code);

    if (action->type == ACTION_VOICE_HOLD) {
        led_indicator_set(LED_STATE_MIC_STREAMING); // Solid Blue while voice recording
    } else if (action->type == ACTION_VOICE_RELEASE) {
        led_indicator_set(LED_STATE_CONNECTED);     // Solid Green when voice recording ends
    } else {
        led_indicator_trigger_key(false);           // Yellow flash on ordinary HID key actions
    }

    switch (action->type) {
        case ACTION_KEYBOARD_TAP:
            usb_hid_keyboard_tap(action->modifier, action->key_code);
            break;
        case ACTION_KEYBOARD_HOLD:
            guard_add(action->modifier, action->key_code, 0, false);
            usb_hid_keyboard_press(action->modifier, action->key_code);
            break;
        case ACTION_KEYBOARD_RELEASE:
            guard_clear_nonvoice();
            usb_hid_keyboard_release();
            break;
        case ACTION_CONSUMER_TAP:
            usb_hid_consumer_tap(action->consumer_code);
            break;
        case ACTION_CONSUMER_HOLD:
            guard_add(0, 0, action->consumer_code, false);
            usb_hid_consumer_press(action->consumer_code);
            break;
        case ACTION_CONSUMER_RELEASE:
            guard_clear_nonvoice();
            usb_hid_consumer_release();
            break;
        case ACTION_VOICE_HOLD:
            // Hold Voice Hotkey and start audio session
            guard_add(action->modifier, action->key_code, 0, true);
            audio_pipeline_start_session(&g_audio_pipeline, 0);
            if (action->modifier != 0 || action->key_code != 0) {
                usb_hid_keyboard_press(action->modifier, action->key_code);
            }
            break;
        case ACTION_VOICE_RELEASE:
            // Release Voice Hotkey and stop audio session
            guard_clear_voice();
            usb_hid_keyboard_release();
            audio_pipeline_stop_session(&g_audio_pipeline);
            break;
        default:
            break;
    }
}

void usb_audio_task(void) {
    uac_microphone_task();
}

} // extern "C"
