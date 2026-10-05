#include "usb_composite.h"
#include "hid_diagnostics.h"
#include "core_diagnostics.h"
#include "uac_microphone.h"
#include "hid_command_queue.h"
#include "usb/hid_guard_registry.h"
#include "app_config.h"
#include "audio/audio_pipeline.h"
#include "ble/ble_remote_client.h"
#include "ble/voice_action_gate.h"
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
#include <string.h>

#if !ARDUINO_USB_CDC_ON_BOOT
USBCDC USBSerial;
#endif

static USBHIDKeyboard        s_keyboard;
static USBHIDConsumerControl s_consumer;
static bool                  s_usb_ready = false;
static bool                  s_usb_initialized = false;
static uint32_t               s_recovery_by_reason[4] = {};
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
static portMUX_TYPE s_hid_command_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_hid_registry_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_voice_state_mux = portMUX_INITIALIZER_UNLOCKED;
static remotemapper::HidCommandQueue s_hid_commands;
static TaskHandle_t s_hid_sender_task = nullptr;

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
using hid_held_entry_t = remotemapper::hid_guard::HeldEntry;
static remotemapper::hid_guard::Registry s_guard_registry = {{}, 1};
static uint32_t         s_last_output_ms          = 0;
static uint32_t         s_last_guard_check_ms     = 0;
static bool             s_guard_inited            = false;
static uint32_t         s_guard_mod_ms            = HID_GUARD_MOD_SOLO_MS;
static uint32_t         s_guard_key_ms            = HID_GUARD_KEY_IDLE_MS;
static uint32_t         s_guard_voice_ms          = HID_GUARD_VOICE_EXTREME_MS;
static uint32_t         s_force_release_count     = 0;
static uint32_t         s_last_force_ms           = 0;
static char             s_last_force_reason[24]   = "";

// The NimBLE producer only touches s_hid_commands under its short portMUX. The
// single sender task owns report submission; hid_lock serializes that task with
// the optional laboratory stress sender and recovery's quiescence barrier.
static hid_keyboard_report_t s_keyboard_report = {};
static_assert(sizeof(hid_keyboard_report_t) == 8, "Keyboard trace requires the standard 8-byte report");
static uint16_t s_consumer_report = 0;
static uint32_t s_hid_last_retry_ms = 0;
static uint32_t s_hid_last_warning_ms = 0;
static uint32_t s_hid_tx_ok = 0;
static uint32_t s_hid_tx_failed = 0;
static bool s_hid_blocked = false;
static uint32_t s_hid_blocked_since_ms = 0;
// Protected by hid_lock(). Recovery first closes this gate, then clears the
// desired reports. It reopens only after a new synchronous TinyUSB mount.
static bool s_hid_recovering = false;
static uint32_t s_reconnect_mount_seq = 0;
static uint32_t s_usb_mount_seq = 0;
static bool guard_has_held(void);
static void guard_force_release_if_current(
        const remotemapper::hid_guard::Snapshot &snapshot, const char *reason);
static void hid_sender_pause_for_detach(void);

// Runs directly in TinyUSB's lifecycle callback, before Arduino posts its
// asynchronous event. Never take hid_lock(): a sender may be waiting for this
// USB task to deliver its completion.
extern "C" void remotemapper_usb_lifecycle(bool mounted) {
    if (mounted) __atomic_add_fetch(&s_usb_mount_seq, 1, __ATOMIC_RELEASE);
}
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
static remotemapper::ble::VoiceActionGate s_voice_action_gate;
static portMUX_TYPE s_voice_action_gate_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint32_t sequence;
    uint32_t press_ms;
    uint32_t release_ms;
    bool drain_pending;
    bool hid_active;
} voice_state_snapshot_t;

static voice_state_snapshot_t voice_state_snapshot(void) {
    voice_state_snapshot_t state;
    portENTER_CRITICAL(&s_voice_state_mux);
    state.sequence = s_voice_diag_seq;
    state.press_ms = s_voice_press_ms;
    state.release_ms = s_voice_release_ms;
    state.drain_pending = s_voice_drain_pending;
    state.hid_active = s_voice_hid_active;
    portEXIT_CRITICAL(&s_voice_state_mux);
    return state;
}

static void hid_command_notify(void) {
    if (s_hid_sender_task) xTaskNotifyGive(s_hid_sender_task);
}

static bool hid_command_pending(uint8_t channel = 0) {
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool pending = channel ? s_hid_commands.has_pending(channel) : s_hid_commands.has_pending();
    portEXIT_CRITICAL(&s_hid_command_mux);
    return pending;
}

static bool hid_command_current(const remotemapper::HidCommandQueue::Command &command) {
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool current = s_hid_commands.is_current(command);
    portEXIT_CRITICAL(&s_hid_command_mux);
    return current;
}

static bool hid_send_command_report(const remotemapper::HidCommandQueue::Command &command,
        bool release_report) {
    if (!s_usb_ready || !hid_command_current(command)) return false;
    const bool keyboard = command.kind == remotemapper::HidCommandQueue::Kind::KeyboardState ||
            command.kind == remotemapper::HidCommandQueue::Kind::KeyboardTap ||
            command.kind == remotemapper::HidCommandQueue::Kind::KeyboardRelease ||
            command.kind == remotemapper::HidCommandQueue::Kind::EmergencyKeyboardRelease;
    if (tud_suspended() || s_hw_sleep_detected || (USB0.dsts & 1) != 0) {
        tud_remote_wakeup();
        vTaskDelay(pdMS_TO_TICKS(keyboard ? 15 : 10));
        if (!s_usb_ready || !hid_command_current(command)) return false;
    }
    if (!tud_ready()) return false;

    hid_lock();
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool can_send = s_hid_commands.begin_send(command);
    portEXIT_CRITICAL(&s_hid_command_mux);
    if (!can_send) {
        hid_unlock();
        return false;
    }

    bool ok = false;
    if (keyboard) {
        hid_keyboard_report_t report = {};
        if (!release_report) {
            report.modifier = command.modifier;
            report.keycode[0] = command.keycode;
        }
        s_keyboard_report = report;
        ok = s_hid_transport.SendReport(HID_REPORT_ID_KEYBOARD, &report, sizeof(report), 20);
        hid_diagnostics_record_keyboard_report(millis(), command.id, command.epoch,
                reinterpret_cast<const uint8_t *>(&report), ok, false);
    } else {
        const uint16_t report = release_report ? 0 : command.consumer;
        s_consumer_report = report;
        ok = s_hid_transport.SendReport(HID_REPORT_ID_CONSUMER_CONTROL, &report, sizeof(report), 20);
    }
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.end_send();
    portEXIT_CRITICAL(&s_hid_command_mux);
    if (ok) s_hid_tx_ok++;
    else s_hid_tx_failed++;
    hid_unlock();
    return ok;
}

static void hid_sender_task(void *) {
    using Queue = remotemapper::HidCommandQueue;
    for (;;) {
        Queue::Command command = {};
        portENTER_CRITICAL(&s_hid_command_mux);
        const bool available = s_hid_commands.peek(command);
        const bool recovering = s_hid_commands.recovering();
        portEXIT_CRITICAL(&s_hid_command_mux);
        if (!available || recovering) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }

        const bool keyboard = command.kind == Queue::Kind::KeyboardState ||
                command.kind == Queue::Kind::KeyboardTap ||
                command.kind == Queue::Kind::KeyboardRelease ||
                command.kind == Queue::Kind::EmergencyKeyboardRelease;
        const bool tap = command.kind == Queue::Kind::KeyboardTap || command.kind == Queue::Kind::ConsumerTap;
        const bool release = command.kind == Queue::Kind::KeyboardRelease ||
                command.kind == Queue::Kind::ConsumerRelease ||
                command.kind == Queue::Kind::EmergencyKeyboardRelease ||
                command.kind == Queue::Kind::EmergencyConsumerRelease;

        bool completed = hid_send_command_report(command, release);
        if (!completed) {
            if (!hid_command_current(command)) continue;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (tap) {
            // Keep the tap indivisible in the FIFO. Cancellation during the
            // dwell invalidates its epoch; the urgent zero mailbox then wins.
            vTaskDelay(pdMS_TO_TICKS(15));
            if (!hid_command_current(command)) continue;
            for (;;) {
                completed = hid_send_command_report(command, true);
                if (completed) break;
                if (!hid_command_current(command)) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (!completed) continue;
        }

        portENTER_CRITICAL(&s_hid_command_mux);
        const bool retired = s_hid_commands.complete(command);
        portEXIT_CRITICAL(&s_hid_command_mux);
        if (!retired) continue;
        if (keyboard && release) app_log("USB_HID", "Keyboard release TX complete");
    }
}

#if defined(REMOTEMAPPER_DWC2_DRIVER)
static usb_hid_stress_stats_t s_hid_stress = {};
static bool s_hid_stress_stop = false;

// Caller holds hid_lock(), including the check and report submission. Never
// replace desired reports or clear release debt to make the test progress.
static bool hid_stress_user_idle_locked(void) {
    const voice_state_snapshot_t voice = voice_state_snapshot();
    if (hid_command_pending() || voice.hid_active || voice.drain_pending ||
            audio_pipeline_is_active(&g_audio_pipeline) || s_consumer_report)
        return false;
    const hid_keyboard_report_t idle = {};
    if (memcmp(&s_keyboard_report, &idle, sizeof(idle)) != 0) return false;
    if (guard_has_held()) return false;
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
        else if (s_hid_recovering || !s_usb_ready || !tud_ready() || tud_suspended()) reason = "usb-unavailable";
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
        if (keyboard) {
            hid_diagnostics_record_keyboard_report(millis(), 0, 0,
                    reinterpret_cast<const uint8_t *>(&keyboard_idle), ok, true);
        }
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
    if (s_hid_stress.active || s_hid_recovering || !s_usb_ready || !tud_ready() || tud_suspended() ||
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
            nullptr, PRIO_TASK_HID_STRESS, nullptr, TASK_CORE_HID_STRESS) == pdPASS;
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

void usb_composite_recovery_reason_counts(uint32_t *audio, uint32_t *hid, uint32_t *wakeup) {
    if (audio) *audio = __atomic_load_n(&s_recovery_by_reason[USB_RECOVERY_AUDIO], __ATOMIC_ACQUIRE);
    if (hid) *hid = __atomic_load_n(&s_recovery_by_reason[USB_RECOVERY_HID], __ATOMIC_ACQUIRE);
    if (wakeup) *wakeup = __atomic_load_n(&s_recovery_by_reason[USB_RECOVERY_WAKE], __ATOMIC_ACQUIRE);
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
            // An old queued Arduino Started event cannot reopen the gate. Only
            // a synchronous mount processed after this attach can do so.
            s_reconnect_mount_seq = __atomic_load_n(&s_usb_mount_seq, __ATOMIC_ACQUIRE);
            tud_connect();
            detached = false;
            s_waiting_reconnect_ms = now;
            app_log("USB", "Recovery: reconnected, waiting for host mount");
        }
        return true;
    }
    hid_lock();
    const bool recovering = s_hid_recovering;
    const bool remounted = recovering && tud_mounted() &&
            __atomic_load_n(&s_usb_mount_seq, __ATOMIC_ACQUIRE) != s_reconnect_mount_seq;
    if (remounted) {
        s_hid_recovering = false;
        portENTER_CRITICAL(&s_hid_command_mux);
        s_hid_commands.end_recovery();
        portEXIT_CRITICAL(&s_hid_command_mux);
    }
    hid_unlock();
    if (remounted) {
        s_waiting_reconnect_ms = 0;
        hid_command_notify();
        app_log("USB", "Recovery: new host mount confirmed; HID retries enabled");
    }
    if (recovering && !remounted) return false; // loop owns the reconnect deadline
    if (!__atomic_load_n(&s_usb_recovery_request, __ATOMIC_ACQUIRE)) return false;
    if (usb_composite_recovery_count() && now - last_recovery_ms < 10000) return false;
    uint32_t reason = __atomic_exchange_n(&s_usb_recovery_request, USB_RECOVERY_NONE, __ATOMIC_ACQ_REL);
    // Serialize with every sender before touching recovery state or the PHY.
    // Force-release below preserves zero-report debt without submitting to an
    // endpoint that is about to be detached.
    hid_lock();
    if (reason == USB_RECOVERY_HID && !hid_command_pending()) {
        hid_unlock();
        return false; // it recovered during the cooldown; no detach is needed
    }
    s_hid_recovering = true;
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.begin_recovery();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_unlock();
    hid_command_notify();
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
            hid_sender_pause_for_detach();
            tud_disconnect();
            app_log("USB", "USB recovery exhausted; detached until power cycle");
            return true;
        }
    }
    const char *cause = reason == USB_RECOVERY_HID ? "hid-tx-stalled" :
            reason == USB_RECOVERY_AUDIO ? "uac-stalled" : "usb-wakeup";
    usb_composite_force_release_all(cause);
    // The recovery gate is closed before taking the sender mutex. This bounded
    // barrier proves no SendReport is still inside the framework before PHY
    // detach; no producer takes this mutex.
    hid_lock();
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.pause_for_detach();
    portEXIT_CRITICAL(&s_hid_command_mux);
    s_hid_blocked = false;
    hid_unlock();
    __atomic_add_fetch(&s_usb_recovery_count, 1, __ATOMIC_RELEASE);
    if (reason >= USB_RECOVERY_AUDIO && reason <= USB_RECOVERY_WAKE)
        __atomic_add_fetch(&s_recovery_by_reason[reason], 1, __ATOMIC_RELEASE);
    last_recovery_ms = now;
    s_hw_sleep_detected = false;
    s_boot_grace_until_ms = now + 5000;
    tud_disconnect();
    detached_ms = now;
    detached = true;
    app_log("USB", "Recovery #%u: %s, detach 350ms",
            (unsigned)usb_composite_recovery_count(), cause);
    return true;
}

static void hid_clear_locked(void) {
    s_keyboard_report = {};
    s_consumer_report = 0;
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.force_release_all();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
}

static void hid_force_cancel_commands(void) {
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.force_release_all();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
}

static void hid_sender_pause_for_detach(void) {
    // Worker owns SendReport under this mutex. Close its report gate before
    // releasing the mutex, then PHY detach cannot race a fresh submission.
    hid_lock();
    portENTER_CRITICAL(&s_hid_command_mux);
    s_hid_commands.pause_for_detach();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_unlock();
}

static void guard_load_config(void) {
    if (s_guard_inited) return;
    s_guard_inited = true;
    uint32_t stored_voice_ms = HID_GUARD_VOICE_EXTREME_MS;
    Preferences p;
    if (p.begin("guard_conf", true)) {
        s_guard_mod_ms   = p.getUInt("mod_ms", HID_GUARD_MOD_SOLO_MS);
        s_guard_key_ms   = p.getUInt("key_ms", HID_GUARD_KEY_IDLE_MS);
        stored_voice_ms = p.getUInt("voice_ms", HID_GUARD_VOICE_EXTREME_MS);
        p.end();
    }
    // The remote ends capture at 60s. Keep a 2s transport/drain allowance and
    // migrate legacy 15-minute or disabled settings to that physical ceiling.
    s_guard_voice_ms = stored_voice_ms == 0 || stored_voice_ms > HID_GUARD_VOICE_EXTREME_MS
            ? HID_GUARD_VOICE_EXTREME_MS : (stored_voice_ms < 1000 ? 1000 : stored_voice_ms);
    if (s_guard_voice_ms != stored_voice_ms) {
        Preferences migrate;
        if (migrate.begin("guard_conf", false)) {
            const bool saved = migrate.putUInt("voice_ms", s_guard_voice_ms) != 0;
            migrate.end();
            if (!saved) app_log("GUARD", "Voice ceiling migration could not be persisted");
        } else {
            app_log("GUARD", "Voice ceiling migration could not open settings");
        }
    }
}

static void guard_add(uint8_t modifier, uint8_t key_code, uint16_t consumer, bool voice) {
    uint32_t now = millis();
    portENTER_CRITICAL(&s_hid_registry_mux);
    const bool added = remotemapper::hid_guard::add(
            s_guard_registry, modifier, key_code, consumer, voice, now);
    portEXIT_CRITICAL(&s_hid_registry_mux);
    if (added) return;
    // Registry full: force everything out rather than risk a stuck key.
    usb_composite_force_release_all("registry-full");
}

static void guard_clear_nonvoice(void) {
    portENTER_CRITICAL(&s_hid_registry_mux);
    remotemapper::hid_guard::clear(s_guard_registry, 0);
    portEXIT_CRITICAL(&s_hid_registry_mux);
}

static void guard_clear_voice(void) {
    portENTER_CRITICAL(&s_hid_registry_mux);
    remotemapper::hid_guard::clear(s_guard_registry, 1);
    portEXIT_CRITICAL(&s_hid_registry_mux);
}

static void guard_clear_all(void) {
    portENTER_CRITICAL(&s_hid_registry_mux);
    remotemapper::hid_guard::clear(s_guard_registry, -1);
    portEXIT_CRITICAL(&s_hid_registry_mux);
}

static bool guard_has_held(void) {
    bool held = false;
    portENTER_CRITICAL(&s_hid_registry_mux);
    for (int i = 0; i < remotemapper::hid_guard::kHeldMax; i++) {
        if (s_guard_registry.entries[i].pressed) { held = true; break; }
    }
    portEXIT_CRITICAL(&s_hid_registry_mux);
    return held;
}

// A timeout decision is based on a short registry snapshot. Revalidate it
// while holding the key engine's recursive mutex, which serializes every
// physical press/release and its guard registry callback through the ensuing
// force-release transaction.
static void guard_force_release_if_current(
        const remotemapper::hid_guard::Snapshot &snapshot, const char *reason) {
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);
    bool current = false;
    portENTER_CRITICAL(&s_hid_registry_mux);
    current = remotemapper::hid_guard::snapshot_is_current(
            snapshot, s_guard_registry.generation);
    portEXIT_CRITICAL(&s_hid_registry_mux);
    if (current) usb_composite_force_release_all(reason);
    key_engine_unlock_state(&g_key_engine);
}

bool usb_composite_guard_force_voice_rx_timeout(uint32_t registry_generation,
        uint32_t voice_since_ms, uint32_t last_frame_ms) {
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);
    remotemapper::hid_guard::Snapshot snapshot;
    portENTER_CRITICAL(&s_hid_registry_mux);
    snapshot = remotemapper::hid_guard::take_snapshot(s_guard_registry);
    portEXIT_CRITICAL(&s_hid_registry_mux);

    bool has_matching_voice = false;
    for (int i = 0; i < remotemapper::hid_guard::kHeldMax; i++) {
        const hid_held_entry_t &entry = snapshot.entries[i];
        if (entry.pressed && entry.voice && entry.since_ms == voice_since_ms) {
            has_matching_voice = true;
            break;
        }
    }
    const uint32_t now = millis();
    const bool current = snapshot.generation == registry_generation && has_matching_voice &&
            ble_remote_last_audio_frame_ms() == last_frame_ms &&
            remotemapper::hid_guard::elapsed_at_least(
                    now, voice_since_ms, HID_GUARD_VOICE_RX_GAP_MS) &&
            remotemapper::hid_guard::elapsed_at_least(
                    now, last_frame_ms, HID_GUARD_VOICE_RX_GAP_MS);
    if (current) usb_composite_force_release_all("voice-rx-timeout");
    key_engine_unlock_state(&g_key_engine);
    return current;
}

// Periodic stuck-key rule evaluation (called from usb_composite_task).
static void guard_tick(uint32_t now) {
    guard_load_config();

    if (!remotemapper::hid_guard::elapsed_at_least(now, s_last_guard_check_ms, 250)) {
        return;
    }
    s_last_guard_check_ms = now;

    bool any_held     = false;
    bool any_voice    = false;
    uint32_t first_voice_ms = 0;
    remotemapper::hid_guard::Snapshot snapshot;
    portENTER_CRITICAL(&s_hid_registry_mux);
    // guard_add timestamps just before taking this mux, so sampling while
    // holding it guarantees every copied press timestamp is no later than the
    // reference clock used for elapsed-time decisions.
    const uint32_t registry_now = millis();
    snapshot = remotemapper::hid_guard::take_snapshot(s_guard_registry);
    portEXIT_CRITICAL(&s_hid_registry_mux);
    uint32_t oldest_voice_age = 0;
    bool got_voice_age = false;
    for (int i = 0; i < remotemapper::hid_guard::kHeldMax; i++) {
        const hid_held_entry_t *e = &snapshot.entries[i];
        if (!e->pressed) continue;
        if (e->voice) {
            any_voice = true;
            const uint32_t age = remotemapper::hid_guard::held_age(registry_now, *e);
            if (!got_voice_age || age > oldest_voice_age) {
                first_voice_ms = e->since_ms;
                oldest_voice_age = age;
                got_voice_age = true;
            }
        } else {
            any_held = true;
        }
    }

    // A new session gets a full startup grace. A timestamp from the previous
    // hold cannot extend or shorten it. Unsigned ages also handle millis wrap.
    // Read the cross-core timestamp before sampling the clock: a new RX frame
    // must not appear "in the future" relative to the earlier loop timestamp.
    const uint32_t last_frame_ms = ble_remote_last_audio_frame_ms();
    const uint32_t voice_now = millis();
    if (any_voice && remotemapper::hid_guard::elapsed_at_least(
                voice_now, first_voice_ms, HID_GUARD_VOICE_RX_GAP_MS) &&
            remotemapper::hid_guard::elapsed_at_least(
                voice_now, last_frame_ms, HID_GUARD_VOICE_RX_GAP_MS)) {
        ble_remote_request_voice_rx_timeout(snapshot.generation, first_voice_ms,
                last_frame_ms);
        return;
    }

    // Rule V: absolute ceiling for a voice recording (exempt from normal rules).
    if (any_voice && s_guard_voice_ms && remotemapper::hid_guard::elapsed_at_least(
                registry_now, first_voice_ms, s_guard_voice_ms)) {
        guard_force_release_if_current(snapshot, "voice-extreme");
        return;
    }

    // Rule M: any modifier held continuously past the ceiling, regardless of
    // whatever else is going on (this is the classic "PC thinks Alt is down").
    for (int i = 0; i < remotemapper::hid_guard::kHeldMax; i++) {
        const hid_held_entry_t *e = &snapshot.entries[i];
        if (!e->pressed || e->voice || e->modifier == 0) continue;
        if (s_guard_mod_ms && remotemapper::hid_guard::elapsed_at_least(
                    registry_now, e->since_ms, s_guard_mod_ms)) {
            guard_force_release_if_current(snapshot, "modifier-hold");
            return;
        }
    }

    // Rule K: a non-voice key held down with zero further output -> release.
    const uint32_t last_output_ms = __atomic_load_n(&s_last_output_ms, __ATOMIC_ACQUIRE);
    if (any_held && s_guard_key_ms && remotemapper::hid_guard::elapsed_at_least(
                registry_now, last_output_ms, s_guard_key_ms)) {
        guard_force_release_if_current(snapshot, "key-idle");
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
        stats->keyboard_pending = hid_command_pending(1);
        stats->consumer_pending = hid_command_pending(2);
        hid_lock();
        stats->desired_modifier = s_keyboard_report.modifier;
        stats->tx_complete = s_hid_tx_ok;
        stats->tx_failed = s_hid_tx_failed;
        stats->pending_ms = s_hid_blocked ? millis() - s_hid_blocked_since_ms : 0;
        stats->usb_recoveries = usb_composite_recovery_count();
        stats->recovery_exhausted = s_usb_recovery_exhausted;
        stats->transport_recovering = s_hid_recovering;
        hid_unlock();
        stats->forced_releases = s_force_release_count;
        stats->last_force_ms   = s_last_force_ms;
        snprintf(stats->last_reason, sizeof(stats->last_reason), "%s", s_last_force_reason);
        stats->any_held  = false;
        stats->held_count = 0;
        portENTER_CRITICAL(&s_hid_registry_mux);
        for (int i = 0; i < remotemapper::hid_guard::kHeldMax; i++) {
            if (s_guard_registry.entries[i].pressed) {
                stats->any_held = true;
                stats->held_count++;
            }
        }
        portEXIT_CRITICAL(&s_hid_registry_mux);
    }
    return true;
}

bool usb_composite_guard_set(const usb_guard_config_t *cfg) {
    if (!cfg) return false;
    guard_load_config();
    s_guard_mod_ms   = cfg->mod_ms;
    s_guard_key_ms   = cfg->key_ms;
    s_guard_voice_ms = cfg->voice_ms == 0 || cfg->voice_ms > HID_GUARD_VOICE_EXTREME_MS
            ? HID_GUARD_VOICE_EXTREME_MS : (cfg->voice_ms < 1000 ? 1000 : cfg->voice_ms);
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
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);

    // Nothing held and nothing recording -> this is a no-op (e.g. the USB bus
    // stops during the very first enumeration at boot). Don't count/flash.
    const bool pending = hid_command_pending();
    if (!guard_has_held() && !audio_pipeline_is_active(&g_audio_pipeline) && !pending) {
        key_engine_unlock_state(&g_key_engine);
        return;
    }

    s_force_release_count++;
    s_last_force_ms = millis();
    snprintf(s_last_force_reason, sizeof(s_last_force_reason), "%s", reason);
    app_log("GUARD", "Forced HID release (reason=%s, count=%u)",
            reason, (unsigned)s_force_release_count);
    // Cancel queued or in-flight-era commands before key_engine_release_all()
    // emits its ordinary releases. Producers never take hid_lock().
    hid_force_cancel_commands();

    // Let the key engine release every slot: it emits the proper RELEASE
    //    actions and clears internal press state consistently.
    key_engine_release_all(&g_key_engine, millis());

    // The engine transaction lock excludes a new physical press until the old
    // voice session and its registry entries are fully cleared.
    portENTER_CRITICAL(&s_voice_state_mux);
    s_voice_drain_pending = false;
    s_voice_hid_active = false;
    portEXIT_CRITICAL(&s_voice_state_mux);
    const bool stopped_audio = audio_pipeline_is_active(&g_audio_pipeline);
    if (stopped_audio) audio_pipeline_stop_session(&g_audio_pipeline);
    if (stopped_audio) ble_remote_request_mic_stop();

    guard_clear_all();

    // 6. Visual: red flash so the user sees the guard fired, then normal status.
    led_indicator_trigger_stuck();
    led_indicator_set(LED_STATE_CONNECTED);
    key_engine_unlock_state(&g_key_engine);
}

void usb_composite_cancel_hid_epoch(void) {
    hid_force_cancel_commands();
}

extern "C" {

static bool finish_voice_drain(uint32_t expected_sequence, bool timeout, const char* reason) {
    uint32_t sequence = 0;
    uint32_t release_ms = 0;
    size_t remaining = 0;
    portENTER_CRITICAL(&s_voice_state_mux);
    const uint32_t now = millis();
    if (!s_voice_drain_pending || s_voice_diag_seq != expected_sequence) {
        portEXIT_CRITICAL(&s_voice_state_mux);
        return false;
    }
    const size_t queued = audio_ring_buffer_peek_available(&g_audio_pipeline.ring_buf);
    const bool due = timeout ? (now - s_voice_release_ms >= 1800) :
            (queued <= 32 && now - s_voice_press_ms >= 1400);
    if (!due) {
        portEXIT_CRITICAL(&s_voice_state_mux);
        return false;
    }
    sequence = s_voice_diag_seq;
    release_ms = s_voice_release_ms;
    remaining = queued;
    s_voice_drain_pending = false;
    audio_pipeline_stop_session(&g_audio_pipeline);
    portEXIT_CRITICAL(&s_voice_state_mux);
    app_log("VOICE_DRAIN", "seq=%u reason=%s delay=%ums remaining=%u",
            (unsigned)sequence, reason, (unsigned)(millis() - release_ms), (unsigned)remaining);
    return true;
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
    if (!s_hid_mutex) {
        app_log("USB", "Initialization failed: HID mutex allocation");
        return;
    }
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

    const bool audio_ready = uac_microphone_init();

    s_keyboard.begin();
    s_consumer.begin();
    s_hid_transport.begin();
    const bool usb_started = USB.begin();
    TaskHandle_t sender = nullptr;
    const bool sender_created = xTaskCreatePinnedToCore(hid_sender_task, "hid_sender", 4096,
            nullptr, PRIO_TASK_USB, &sender, TASK_CORE_USB) == pdPASS;
    if (sender_created) {
        s_hid_sender_task = sender;
        core_diagnostics_register("hid_sender", sender, TASK_CORE_USB);
    } else {
        app_log("USB_HID", "Failed to spawn dedicated report sender");
    }
    s_usb_initialized = audio_ready && usb_started && sender_created;
    s_usb_ready = s_usb_initialized;
    if (!s_usb_initialized)
        app_log("USB", "Initialization failed: audio=%u stack=%u sender=%u", audio_ready, usb_started, sender_created);
    hid_command_notify();
}

bool usb_composite_is_initialized(void) { return s_usb_initialized; }

void usb_composite_task(void) {
    uac_microphone_task();

    uint32_t now = millis();
    if (usb_recovery_tick(now)) return;
    guard_tick(now);
    const voice_state_snapshot_t voice = voice_state_snapshot();
    if (voice.drain_pending) {
        if (now - voice.release_ms >= 1800) {
            finish_voice_drain(voice.sequence, true, "timeout");
        } else if (now - voice.press_ms >= 1400) {
            finish_voice_drain(voice.sequence, false, "empty");
        }
    }

    // The sender task owns retries. The loop only observes durable queue/release
    // debt and requests recovery if reports keep failing while USB is mounted.
    hid_lock();
    if (now - s_hid_last_retry_ms >= 50) {
        s_hid_last_retry_ms = now;
        const bool pending_keyboard = hid_command_pending(1);
        const bool pending_consumer = hid_command_pending(2);
        // An idle local registry does NOT imply Windows received the release.
        // Keep this deadline independent of new presses and audio activity.
        if ((pending_keyboard || pending_consumer) && tud_ready()) {
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
        if ((pending_keyboard || pending_consumer) &&
                now - s_hid_last_warning_ms >= 5000) {
            s_hid_last_warning_ms = now;
            app_log("USB_HID", "TX pending: keyboard=%d consumer=%d mod=0x%02X ready=%d failures=%u",
                    pending_keyboard, pending_consumer,
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
            app_log("USB_HW", "PC Wakeup Detected! Requesting coordinated USB re-enumeration");
            s_hw_sleep_detected = false;
            usb_composite_request_recovery(USB_RECOVERY_WAKE);
        } else {
            s_hw_sleep_detected = false;
        }
    }
}

bool usb_hid_keyboard_press(uint8_t modifier, uint8_t keycode) {
    if (!s_usb_ready) return false;
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_keyboard(modifier, keycode, false);
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

bool usb_hid_keyboard_release(void) {
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_keyboard_release();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode) {
    if (!s_usb_ready) return false;
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_keyboard(modifier, keycode, true);
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

bool usb_hid_consumer_press(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_consumer(usage_code, false);
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

bool usb_hid_consumer_release(void) {
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_consumer_release();
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

bool usb_hid_consumer_tap(uint16_t usage_code) {
    if (!s_usb_ready) return false;
    portENTER_CRITICAL(&s_hid_command_mux);
    const bool queued = s_hid_commands.enqueue_consumer(usage_code, true);
    portEXIT_CRITICAL(&s_hid_command_mux);
    hid_command_notify();
    return queued;
}

void usb_hid_dispatch_action(const key_action_t *action) {
    if (!action) return;
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);

    if (action->type == ACTION_VOICE_HOLD) {
        portENTER_CRITICAL(&s_voice_action_gate_mux);
        const bool accepted = s_voice_action_gate.press();
        portEXIT_CRITICAL(&s_voice_action_gate_mux);
        if (!accepted) {
            app_log("VOICE", "Voice hold refused: BLE voice link not ready");
            key_engine_unlock_state(&g_key_engine);
            return;
        }
    } else if (action->type == ACTION_VOICE_RELEASE) {
        portENTER_CRITICAL(&s_voice_action_gate_mux);
        const bool release_active = s_voice_action_gate.release();
        portEXIT_CRITICAL(&s_voice_action_gate_mux);
        if (!release_active) {
            key_engine_unlock_state(&g_key_engine);
            return;
        }
    }

    // Every emitted action counts as output activity for the stuck-key rules.
    __atomic_store_n(&s_last_output_ms, millis(), __ATOMIC_RELEASE);
    // Invalidate a timeout snapshot if activity arrives before it can commit.
    portENTER_CRITICAL(&s_hid_registry_mux);
    s_guard_registry.generation = remotemapper::hid_guard::next_generation(
            s_guard_registry.generation);
    portEXIT_CRITICAL(&s_hid_registry_mux);

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
        case ACTION_VOICE_HOLD: {
            // Serialize a new session against loop-side drain completion. The
            // loop rechecks this sequence under the same short lock before stop.
            uac_microphone_get_pcm_stats(&s_voice_pcm_samples0,
                    &s_voice_pcm_nonzero0, &s_voice_pcm_abs0, nullptr);
            uint32_t old_sequence = 0;
            uint32_t old_release_ms = 0;
            size_t old_remaining = 0;
            bool ended_previous_drain = false;
            {
                uac_tx_stats_t tx = {};
                uac_microphone_get_stats(&tx);
                s_voice_usb_completed0 = tx.completed;
                s_voice_usb_claim_skips0 = tx.claim_skips;
            }
            portENTER_CRITICAL(&s_voice_state_mux);
            if (s_voice_drain_pending) {
                ended_previous_drain = true;
                old_sequence = s_voice_diag_seq;
                old_release_ms = s_voice_release_ms;
                old_remaining = audio_ring_buffer_peek_available(&g_audio_pipeline.ring_buf);
                s_voice_drain_pending = false;
                audio_pipeline_stop_session(&g_audio_pipeline);
            }
            ++s_voice_diag_seq;
            s_voice_press_ms = millis();
            const bool hid_active = action->modifier != 0 || action->key_code != 0;
            s_voice_hid_active = hid_active;
            audio_pipeline_start_session(&g_audio_pipeline, 0);
            portEXIT_CRITICAL(&s_voice_state_mux);
            if (ended_previous_drain) {
                app_log("VOICE_DRAIN", "seq=%u reason=new-press delay=%ums remaining=%u",
                        (unsigned)old_sequence, (unsigned)(millis() - old_release_ms),
                        (unsigned)old_remaining);
            }
            if (hid_active) {
                guard_add(action->modifier, action->key_code, 0, true);
                usb_hid_keyboard_press(action->modifier, action->key_code);
            }
            break;
        }
        case ACTION_VOICE_RELEASE: {
            // Release Right Alt with the physical button. Some input methods
            // distinguish a tap from a hold, and keeping Alt down for audio
            // drain makes the keyboard appear stuck after a short press.
            // Keep only the audio pipeline alive for a bounded drain period;
            // apps that continue capture can consume it without holding HID.
            uint32_t sequence = 0;
            uint32_t press_ms = 0;
            uint32_t release_ms = 0;
            bool hid_was_active = false;
            portENTER_CRITICAL(&s_voice_state_mux);
            release_ms = millis();
            sequence = s_voice_diag_seq;
            press_ms = s_voice_press_ms;
            hid_was_active = s_voice_hid_active;
            s_voice_hid_active = false;
            s_voice_release_ms = release_ms;
            s_voice_drain_pending = hid_was_active && audio_pipeline_is_active(&g_audio_pipeline);
            if (!hid_was_active) audio_pipeline_stop_session(&g_audio_pipeline);
            portEXIT_CRITICAL(&s_voice_state_mux);
            guard_clear_voice();
            if (hid_was_active) {
                usb_hid_keyboard_release();
            }
            {
                uint32_t samples = 0, nonzero = 0;
                uint64_t absolute_sum = 0;
                uac_tx_stats_t tx = {};
                uac_microphone_get_pcm_stats(&samples, &nonzero, &absolute_sum, nullptr);
                uac_microphone_get_stats(&tx);
                const uint32_t arm = uac_microphone_get_stream_start_ms();
                const int32_t arm_delay = arm >= press_ms && arm <= millis()
                        ? (int32_t)(arm - press_ms) : -1;
                const uint32_t pcm_n = samples - s_voice_pcm_samples0;
                app_log("VOICE_AUDIO", "seq=%u hold=%u arm=%d ble=%u push=%u usb=%u skip=%u pcm=%u nz=%u avg=%u",
                        (unsigned)sequence, (unsigned)(release_ms - press_ms),
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
    key_engine_unlock_state(&g_key_engine);
}

void usb_voice_action_set_ready(bool ready) {
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);
    portENTER_CRITICAL(&s_voice_action_gate_mux);
    s_voice_action_gate.set_ready(ready);
    portEXIT_CRITICAL(&s_voice_action_gate_mux);
    if (!ready) usb_voice_action_invalidate();
    key_engine_unlock_state(&g_key_engine);
}

void usb_voice_action_invalidate(void) {
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);
    portENTER_CRITICAL(&s_voice_action_gate_mux);
    const bool release_active = s_voice_action_gate.invalidate();
    portEXIT_CRITICAL(&s_voice_action_gate_mux);
    if (!release_active) {
        key_engine_unlock_state(&g_key_engine);
        return;
    }
    // Clear only the voice action's local audio/HID state. This also handles
    // a MIC_OPEN failure if the keymap changes before the physical UP report.
    portENTER_CRITICAL(&s_voice_state_mux);
    const bool hid_was_active = s_voice_hid_active;
    s_voice_hid_active = false;
    s_voice_drain_pending = false;
    audio_pipeline_stop_session(&g_audio_pipeline);
    portEXIT_CRITICAL(&s_voice_state_mux);
    guard_clear_voice();
    if (hid_was_active) usb_hid_keyboard_release();
    led_indicator_set(ble_remote_get_state() >= BLE_STATE_CONNECTED
            ? LED_STATE_CONNECTED : LED_STATE_WAIT_CONNECTION);
    app_log("VOICE", "Voice hold released after BLE readiness loss");
    key_engine_unlock_state(&g_key_engine);
}

bool usb_voice_action_is_active(void) {
    extern key_mapper_engine_t g_key_engine;
    key_engine_lock_state(&g_key_engine);
    portENTER_CRITICAL(&s_voice_action_gate_mux);
    const bool active = s_voice_action_gate.active();
    portEXIT_CRITICAL(&s_voice_action_gate_mux);
    key_engine_unlock_state(&g_key_engine);
    return active;
}

void usb_audio_task(void) {
    uac_microphone_task();
}

} // extern "C"
