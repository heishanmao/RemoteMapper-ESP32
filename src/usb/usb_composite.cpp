#include "usb_composite.h"
#include "hid_diagnostics.h"
#include "uac_microphone.h"
#include "audio/audio_pipeline.h"
#include "ble/ble_remote_client.h"
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
#include "esp_attr.h"
#include <Preferences.h>

#if !ARDUINO_USB_CDC_ON_BOOT
USBCDC USBSerial;
#endif

static USBHIDKeyboard        s_keyboard;
static USBHIDConsumerControl s_consumer;
static bool                  s_usb_ready = false;
// Uses the same framework HID instance/semaphore as keyboard and consumer.
static USBHID                s_hid_transport;


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

// Desired reports and retry state are serialized with hid_lock(). A newer
// press/release replaces the previous desired state, including pending retries.
// SendReport waits for TinyUSB's report-complete callback; never infer delivery
// from an endpoint number (UAC and HID endpoints are allocated dynamically).
static hid_keyboard_report_t s_keyboard_report = {};
static uint16_t s_consumer_report = 0;
static bool s_keyboard_pending = false;
static bool s_consumer_pending = false;
static uint32_t s_hid_last_retry_ms = 0;
static uint32_t s_hid_last_warning_ms = 0;
static uint32_t s_hid_tx_ok = 0;
static uint32_t s_hid_tx_failed = 0;
static bool s_hid_blocked = false;
static uint32_t s_hid_blocked_since_ms = 0;
static uint32_t s_usb_recovery_request = USB_RECOVERY_NONE;
static uint32_t s_usb_recovery_count = 0;
static bool s_usb_recovery_exhausted = false;
static uint32_t s_voice_diag_seq = 0;
static uint32_t s_voice_press_ms = 0;
static uint32_t s_voice_pcm_samples0 = 0;
static uint32_t s_voice_pcm_nonzero0 = 0;
static uint64_t s_voice_pcm_abs0 = 0;
static uint32_t s_voice_usb_completed0 = 0;
static uint32_t s_voice_usb_claim_skips0 = 0;
static bool s_voice_drain_pending = false;
static uint32_t s_voice_release_ms = 0;
static bool s_voice_hid_active = false;

#if defined(REMOTEMAPPER_EXPERIMENTAL_DWC2)
static usb_hid_stress_stats_t s_hid_stress = {};
static bool s_hid_stress_stop = false;

// Caller holds hid_lock(), including the check and report submission. Never
// replace desired reports or clear release debt to make the test progress.
static bool hid_stress_user_idle_locked(void) {
    if (s_keyboard_pending || s_consumer_pending || s_voice_hid_active ||
            s_voice_drain_pending || g_audio_pipeline.active || s_consumer_report)
        return false;
    const hid_keyboard_report_t idle = {};
    if (memcmp(&s_keyboard_report, &idle, sizeof(idle)) != 0) return false;
    for (const auto &entry : s_held) if (entry.pressed) return false;
    return true;
}

static void hid_stress_task(void *) {
    const hid_keyboard_report_t keyboard_idle = {};
    const uint16_t consumer_idle = 0;
    uint32_t iteration = 0;
    for (;;) {
        hid_lock();
        const uint32_t elapsed = millis() - s_hid_stress.started_ms;
        const char *reason = nullptr;
        if (s_hid_stress_stop) reason = "stop";
        else if (!hid_stress_user_idle_locked()) reason = "user-input";
        else if (elapsed >= s_hid_stress.duration_ms) reason = "duration";
        else if (!s_usb_ready || !tud_ready() || tud_suspended()) reason = "usb-unavailable";
        if (reason) {
            s_hid_stress.user_aborted = strcmp(reason, "user-input") == 0;
            snprintf(s_hid_stress.stop_reason, sizeof(s_hid_stress.stop_reason), "%s", reason);
            hid_unlock();
            break;
        }
        const bool keyboard = (iteration & 1) == 0;
        s_hid_stress.attempted++;
        const bool ok = s_hid_transport.SendReport(
                keyboard ? HID_REPORT_ID_KEYBOARD : HID_REPORT_ID_CONSUMER_CONTROL,
                keyboard ? (const void *)&keyboard_idle : (const void *)&consumer_idle,
                keyboard ? sizeof(keyboard_idle) : sizeof(consumer_idle), 20);
        if (ok) s_hid_stress.completed++;
        else {
            s_hid_stress.failed++;
            snprintf(s_hid_stress.stop_reason, sizeof(s_hid_stress.stop_reason), "tx-failed");
        }
        hid_unlock();
        if (!ok) break; // Save the first failure; do not flood retries or recover.
        vTaskDelay(pdMS_TO_TICKS(3 + iteration % 5));
        iteration++;
    }
    hid_lock();
    s_hid_stress.ended_ms = millis();
    s_hid_stress.active = false;
    const usb_hid_stress_stats_t done = s_hid_stress;
    hid_unlock();
    app_log("USB_STRESS", "id=%u complete=%u attempted=%u failed=%u stop=%s",
            (unsigned)done.test_id, (unsigned)done.completed, (unsigned)done.attempted,
            (unsigned)done.failed, done.stop_reason);
    vTaskDelete(nullptr);
}

bool usb_hid_stress_start(uint32_t seconds) {
    if (seconds < 1 || seconds > 300) return false;
    hid_lock();
    if (s_hid_stress.active || !s_usb_ready || !tud_ready() || tud_suspended() ||
            !hid_stress_user_idle_locked()) {
        hid_unlock();
        return false;
    }
    const uint32_t id = s_hid_stress.test_id + 1;
    s_hid_stress = {};
    s_hid_stress.active = true;
    s_hid_stress.test_id = id;
    s_hid_stress.duration_ms = seconds * 1000;
    s_hid_stress.started_ms = millis();
    s_hid_stress_stop = false;
    const bool created = xTaskCreatePinnedToCore(hid_stress_task, "hid_stress", 3072,
            nullptr, 1, nullptr, 0) == pdPASS;
    if (!created) {
        s_hid_stress.active = false;
        s_hid_stress.ended_ms = millis();
        snprintf(s_hid_stress.stop_reason, sizeof(s_hid_stress.stop_reason), "create-failed");
    }
    hid_unlock();
    return created;
}

void usb_hid_stress_stop(void) {
    hid_lock();
    s_hid_stress_stop = true;
    hid_unlock();
}

void usb_hid_stress_get(usb_hid_stress_stats_t *stats) {
    if (!stats) return;
    hid_lock();
    *stats = s_hid_stress;
    hid_unlock();
}
#endif

// One isolated 2 ms completion after re-enumeration is not recovery. The
// incident showed roughly one completion per failed 10 s cycle.
static constexpr uint32_t USB_RECOVERY_HEALTHY_PACKETS = 100;
static constexpr uint32_t USB_RECOVERY_RTC_MAGIC = 0x524D5532;
struct usb_recovery_rtc_t { uint32_t magic, restart_attempted, restart_marker; };
RTC_NOINIT_ATTR static usb_recovery_rtc_t s_usb_recovery_rtc;
static uint32_t s_boot_reset_reason = 0;
static bool s_boot_usb_recovery_restart = false;

static void usb_recovery_rtc_init(void) {
    const esp_reset_reason_t reset = esp_reset_reason();
    if (s_usb_recovery_rtc.magic != USB_RECOVERY_RTC_MAGIC ||
            reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT) {
        s_usb_recovery_rtc.magic = USB_RECOVERY_RTC_MAGIC;
        s_usb_recovery_rtc.restart_attempted = 0;
        s_usb_recovery_rtc.restart_marker = 0;
    }
}

uint32_t usb_composite_boot_reset_reason(void) { return s_boot_reset_reason; }
bool usb_composite_boot_usb_recovery_restart(void) { return s_boot_usb_recovery_restart; }

void usb_composite_request_recovery(usb_recovery_reason_t reason) {
    uint32_t empty = USB_RECOVERY_NONE;
    __atomic_compare_exchange_n(&s_usb_recovery_request, &empty, (uint32_t)reason,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

uint32_t usb_composite_recovery_count(void) {
    return __atomic_load_n(&s_usb_recovery_count, __ATOMIC_ACQUIRE);
}

// Loop task owns the physical recovery for both audio and HID. A healthy
// microphone must not conceal a blocked keyboard release (or vice versa).
static bool usb_recovery_tick(uint32_t now) {
    static bool detached = false;
    static uint32_t detached_ms = 0;
    static uint32_t last_recovery_ms = 0;
    static uint32_t last_audio_completions = 0;
    static uint8_t audio_no_progress_recoveries = 0;
    if (s_usb_recovery_exhausted) return true;
    if (s_usb_recovery_rtc.restart_attempted && now >= 120000 &&
            usb_composite_recovery_count() == 0 && tud_mounted()) {
        s_usb_recovery_rtc.restart_attempted = 0;
        app_log("USB", "USB mounted for 2 min; full-restart allowance restored");
    }
    if (detached) {
        if (now - detached_ms >= 350) {
            tud_connect();
            detached = false;
            s_waiting_reconnect_ms = now;
            app_log("USB", "Recovery: reconnected, waiting for host mount");
        }
        return true;
    }
    if (!__atomic_load_n(&s_usb_recovery_request, __ATOMIC_ACQUIRE)) return false;
    if (usb_composite_recovery_count() && now - last_recovery_ms < 10000) return false;
    uint32_t reason = __atomic_exchange_n(&s_usb_recovery_request, USB_RECOVERY_NONE, __ATOMIC_ACQ_REL);
    if (reason == USB_RECOVERY_HID) {
        hid_lock();
        const bool still_pending = s_keyboard_pending || s_consumer_pending;
        hid_unlock();
        if (!still_pending) return false; // it recovered during the cooldown
    }
    hid_diagnostics_capture(reason);
    if (reason == USB_RECOVERY_AUDIO) {
        uac_tx_stats_t tx = {};
        uac_microphone_get_stats(&tx);
        audio_no_progress_recoveries =
                (uint32_t)(tx.completed - last_audio_completions) >= USB_RECOVERY_HEALTHY_PACKETS
                ? 1 : (uint8_t)(audio_no_progress_recoveries + 1);
        last_audio_completions = tx.completed;
        if (audio_no_progress_recoveries >= 3) {
            app_log("USB", "Audio still stalled after %u recovery attempts (completed=%u)",
                    (unsigned)audio_no_progress_recoveries, (unsigned)tx.completed);
            usb_composite_force_release_all("uac-stalled");
            if (!s_usb_recovery_rtc.restart_attempted) {
                s_usb_recovery_rtc.restart_attempted = 1;
                hid_diagnostics_persist_first_audio_fault();
                s_usb_recovery_rtc.restart_marker = 1;
                app_log("USB", "Soft USB recovery failed; restarting MCU once");
                delay(100);
                esp_restart();
            }
            // If a restart also failed, stop cycling the USB device every 10 s.
            // Leave it detached so Windows drops the unusable capture device.
            s_usb_recovery_exhausted = true;
            tud_disconnect();
            app_log("USB", "USB recovery exhausted; detached until power cycle");
            return true;
        }
    }
    usb_composite_force_release_all(reason == USB_RECOVERY_HID ? "hid-tx-stalled" : "uac-stalled");
    hid_lock();
    s_hid_blocked = false;
    hid_unlock();
    __atomic_add_fetch(&s_usb_recovery_count, 1, __ATOMIC_RELEASE);
    last_recovery_ms = now;
    s_hw_sleep_detected = false;
    s_boot_grace_until_ms = now + 5000;
    tud_disconnect();
    detached_ms = now;
    detached = true;
    app_log("USB", "Recovery #%u: %s stalled, detach 350ms",
            (unsigned)usb_composite_recovery_count(), reason == USB_RECOVERY_HID ? "HID" : "audio");
    return true;
}

// Caller holds hid_lock(). A failure retains the desired state indefinitely.
static bool hid_flush_keyboard_locked(void) {
    if (!s_keyboard_pending) return true;
    if (!s_usb_ready || !tud_ready()) return false;
    bool ok = s_hid_transport.SendReport(HID_REPORT_ID_KEYBOARD,
            &s_keyboard_report, sizeof(s_keyboard_report), 20);
    if (ok) {
        s_keyboard_pending = false;
        s_hid_tx_ok++;
        if (s_keyboard_report.modifier == 0 && s_keyboard_report.keycode[0] == 0)
            app_log("USB_HID", "Keyboard release TX complete");
    } else {
        s_hid_tx_failed++;
    }
    return ok;
}

static bool hid_flush_consumer_locked(void) {
    if (!s_consumer_pending) return true;
    if (!s_usb_ready || !tud_ready()) return false;
    bool ok = s_hid_transport.SendReport(HID_REPORT_ID_CONSUMER_CONTROL,
            &s_consumer_report, sizeof(s_consumer_report), 20);
    if (ok) { s_consumer_pending = false; s_hid_tx_ok++; }
    else { s_hid_tx_failed++; }
    return ok;
}

static void hid_clear_locked(void) {
    s_keyboard_report = {};
    s_consumer_report = 0;
    s_keyboard_pending = true;
    s_consumer_pending = true;
    hid_flush_keyboard_locked();
    hid_flush_consumer_locked();
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

    // A new session gets a full startup grace. A timestamp from the previous
    // hold cannot extend or shorten it. Unsigned ages also handle millis wrap.
    // Read the cross-core timestamp before sampling the clock: a new RX frame
    // must not appear "in the future" relative to the earlier loop timestamp.
    const uint32_t last_frame_ms = ble_remote_last_audio_frame_ms();
    const uint32_t voice_now = millis();
    if (any_voice && voice_now - first_voice_ms >= HID_GUARD_VOICE_RX_GAP_MS &&
            voice_now - last_frame_ms >= HID_GUARD_VOICE_RX_GAP_MS) {
        usb_composite_force_release_all("voice-rx-timeout");
        return;
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
        hid_lock();
        stats->keyboard_pending = s_keyboard_pending;
        stats->consumer_pending = s_consumer_pending;
        stats->desired_modifier = s_keyboard_report.modifier;
        stats->tx_complete = s_hid_tx_ok;
        stats->tx_failed = s_hid_tx_failed;
        stats->pending_ms = s_hid_blocked ? millis() - s_hid_blocked_since_ms : 0;
        stats->usb_recoveries = usb_composite_recovery_count();
        stats->recovery_exhausted = s_usb_recovery_exhausted;
        hid_unlock();
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
    hid_lock();
    const bool pending = s_keyboard_pending || s_consumer_pending;
    hid_unlock();
    if (!guard_has_held() && !g_audio_pipeline.active && !pending) return;

    s_force_release_count++;
    s_last_force_ms = millis();
    snprintf(s_last_force_reason, sizeof(s_last_force_reason), "%s", reason);
    app_log("GUARD", "Forced HID release (reason=%s, count=%u)",
            reason, (unsigned)s_force_release_count);
    s_voice_drain_pending = false;
    s_voice_hid_active = false;

    // 1. Stop any running voice/audio session first (engine release will emit
    //    a VOICE_RELEASE but the session must not outlive the forced release).
    if (g_audio_pipeline.active) {
        audio_pipeline_stop_session(&g_audio_pipeline);
        ble_remote_request_mic_stop();
    }

    // 2. Let the key engine release every slot: it emits the proper RELEASE
    //    actions and clears internal press state consistently.
    extern key_mapper_engine_t g_key_engine;
    key_engine_release_all(&g_key_engine, millis());

    // Keep the release pending independently of the local held registry.
    hid_lock();
    hid_clear_locked();
    hid_unlock();
    guard_clear_all();

    // 6. Visual: red flash so the user sees the guard fired, then normal status.
    led_indicator_trigger_stuck();
    led_indicator_set(LED_STATE_CONNECTED);
}

extern "C" {

static void finish_voice_drain(const char* reason) {
    if (!s_voice_drain_pending) return;
    s_voice_drain_pending = false;
    const size_t remaining = audio_ring_buffer_peek_available(&g_audio_pipeline.ring_buf);
    audio_pipeline_stop_session(&g_audio_pipeline);
    app_log("VOICE_DRAIN", "seq=%u reason=%s delay=%ums remaining=%u",
            (unsigned)s_voice_diag_seq, reason,
            (unsigned)(millis() - s_voice_release_ms), (unsigned)remaining);
}

void usb_composite_init(void) {
    usb_recovery_rtc_init();
    s_boot_reset_reason = (uint32_t)esp_reset_reason();
    s_boot_usb_recovery_restart = s_usb_recovery_rtc.restart_marker != 0;
    s_usb_recovery_rtc.restart_marker = 0;
    app_log("BOOT", "reset_reason=%u usb_recovery_restart=%u",
            (unsigned)s_boot_reset_reason, s_boot_usb_recovery_restart ? 1u : 0u);
    // Create the mutex before USB/BLE callbacks can race its initialization.
    hid_lock();
    hid_unlock();
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
            usb_composite_force_release_all("usb-resume");
            hid_lock();
            hid_clear_locked();
            hid_unlock();
        } else if (id == ARDUINO_USB_STARTED_EVENT) {
            s_waiting_reconnect_ms = 0;
            app_log("USB", "USB Started / Mounted");
            usb_composite_force_release_all("usb-mounted");
            hid_lock();
            hid_clear_locked();
            hid_unlock();
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
    s_hid_transport.begin();
    s_usb_ready = true;
    USB.begin();
}

void usb_composite_task(void) {
    uac_microphone_task();

    uint32_t now = millis();
    if (usb_recovery_tick(now)) return;
    guard_tick(now);
    if (s_voice_drain_pending) {
        const size_t queued = audio_ring_buffer_peek_available(&g_audio_pipeline.ring_buf);
        if (now - s_voice_release_ms >= 1800) {
            finish_voice_drain("timeout");
        } else if (queued <= 32 && now - s_voice_press_ms >= 1400) {
            finish_voice_drain("empty");
        }
    }

    // Checking pending state and sending are one critical section: no stale
    // all-zero retry can race a newly pressed key. Retry at most every 50 ms;
    // disconnect/suspend retains the debt without blocking or flooding logs.
    hid_lock();
    if (now - s_hid_last_retry_ms >= 50) {
        s_hid_last_retry_ms = now;
        hid_flush_keyboard_locked();
        hid_flush_consumer_locked();
        // An idle local registry does NOT imply Windows received the release.
        // Keep this deadline independent of new presses and audio activity.
        if ((s_keyboard_pending || s_consumer_pending) && tud_ready()) {
            const uint32_t checked_ms = millis();
            if (!s_hid_blocked) {
                s_hid_blocked = true;
                s_hid_blocked_since_ms = checked_ms;
            } else if (checked_ms - s_hid_blocked_since_ms >= 1000) {
                usb_composite_request_recovery(USB_RECOVERY_HID);
            }
        } else {
            s_hid_blocked = false;
        }
        if ((s_keyboard_pending || s_consumer_pending) &&
                now - s_hid_last_warning_ms >= 5000) {
            s_hid_last_warning_ms = now;
            app_log("USB_HID", "TX pending: keyboard=%d consumer=%d mod=0x%02X ready=%d failures=%u",
                    s_keyboard_pending, s_consumer_pending,
                    s_keyboard_report.modifier, tud_ready(), (unsigned)s_hid_tx_failed);
        }
    }
    hid_unlock();

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

    s_keyboard_report = {};
    s_keyboard_report.modifier = modifier;
    s_keyboard_report.keycode[0] = keycode;
    s_keyboard_pending = true;
    bool ok = hid_flush_keyboard_locked();
    app_log("USB_HID", "Keyboard DOWN mod=0x%02X key=0x%02X tx=%s",
            modifier, keycode, ok ? "complete" : "pending");
    hid_unlock();
    return ok;
}

bool usb_hid_keyboard_release(void) {
    hid_lock();
    s_keyboard_report = {};
    s_keyboard_pending = true;
    bool ok = hid_flush_keyboard_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode) {
    if (!s_usb_ready) return false;
    bool down = usb_hid_keyboard_press(modifier, keycode);
    delay(15);
    bool up = usb_hid_keyboard_release();
    return down && up;
}

bool usb_hid_consumer_press(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    hid_lock();

    if (tud_suspended()) {
        app_log("USB", "PC Suspended -> tud_remote_wakeup");
        tud_remote_wakeup();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_consumer_report = usage_code;
    s_consumer_pending = true;
    bool ok = hid_flush_consumer_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_consumer_release(void) {
    if (!s_usb_ready) return false;
    hid_lock();

    s_consumer_report = 0;
    s_consumer_pending = true;
    bool ok = hid_flush_consumer_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_consumer_tap(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    bool down = usb_hid_consumer_press(usage_code);
    delay(15);
    bool up = usb_hid_consumer_release();
    return down && up;
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
            finish_voice_drain("new-press");
            ++s_voice_diag_seq;
            s_voice_press_ms = millis();
            uac_microphone_get_pcm_stats(&s_voice_pcm_samples0,
                    &s_voice_pcm_nonzero0, &s_voice_pcm_abs0, nullptr);
            {
                uac_tx_stats_t tx = {};
                uac_microphone_get_stats(&tx);
                s_voice_usb_completed0 = tx.completed;
                s_voice_usb_claim_skips0 = tx.claim_skips;
            }
            audio_pipeline_start_session(&g_audio_pipeline, 0);
            s_voice_hid_active = action->modifier != 0 || action->key_code != 0;
            if (s_voice_hid_active) {
                guard_add(action->modifier, action->key_code, 0, true);
                usb_hid_keyboard_press(action->modifier, action->key_code);
            }
            break;
        case ACTION_VOICE_RELEASE: {
            // Release Right Alt with the physical button. Some input methods
            // distinguish a tap from a hold, and keeping Alt down for audio
            // drain makes the keyboard appear stuck after a short press.
            // Keep only the audio pipeline alive for a bounded drain period;
            // apps that continue capture can consume it without holding HID.
            s_voice_release_ms = millis();
            const bool hid_was_active = s_voice_hid_active;
            s_voice_hid_active = false;
            s_voice_drain_pending = hid_was_active && g_audio_pipeline.active;
            guard_clear_voice();
            if (hid_was_active) {
                usb_hid_keyboard_release();
            } else {
                audio_pipeline_stop_session(&g_audio_pipeline);
            }
            {
                uint32_t samples = 0, nonzero = 0;
                uint64_t absolute_sum = 0;
                uac_tx_stats_t tx = {};
                uac_microphone_get_pcm_stats(&samples, &nonzero, &absolute_sum, nullptr);
                uac_microphone_get_stats(&tx);
                const uint32_t arm = uac_microphone_get_stream_start_ms();
                const int32_t arm_delay = arm >= s_voice_press_ms && arm <= millis()
                        ? (int32_t)(arm - s_voice_press_ms) : -1;
                const uint32_t pcm_n = samples - s_voice_pcm_samples0;
                app_log("VOICE_AUDIO", "seq=%u hold=%u arm=%d ble=%u push=%u usb=%u skip=%u pcm=%u nz=%u avg=%u",
                        (unsigned)s_voice_diag_seq, (unsigned)(millis() - s_voice_press_ms),
                        (int)arm_delay, (unsigned)g_audio_pipeline.total_frames_decoded,
                        (unsigned)g_audio_pipeline.total_samples_pushed,
                        (unsigned)(tx.completed - s_voice_usb_completed0),
                        (unsigned)(tx.claim_skips - s_voice_usb_claim_skips0),
                        (unsigned)pcm_n, (unsigned)(nonzero - s_voice_pcm_nonzero0),
                        (unsigned)(pcm_n ? (absolute_sum - s_voice_pcm_abs0) / pcm_n : 0));
            }
            break;
        }
        default:
            break;
    }
}

void usb_audio_task(void) {
    uac_microphone_task();
}

} // extern "C"
