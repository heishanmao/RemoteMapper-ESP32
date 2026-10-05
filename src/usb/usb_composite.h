#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"
#include "keymap/key_state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize TinyUSB stack with UAC 1.0 Microphone, HID Keyboard & Consumer, and CDC Serial
 */
void usb_composite_init(void);
bool usb_composite_is_initialized(void);

/**
 * @brief Process periodic TinyUSB device tasks
 */
void usb_composite_task(void);

typedef enum { USB_RECOVERY_NONE, USB_RECOVERY_AUDIO, USB_RECOVERY_HID, USB_RECOVERY_WAKE } usb_recovery_reason_t;
void usb_composite_request_recovery(usb_recovery_reason_t reason);
uint32_t usb_composite_recovery_count(void);
void usb_composite_recovery_reason_counts(uint32_t *audio, uint32_t *hid, uint32_t *wakeup);
uint32_t usb_composite_boot_reset_reason(void);
bool usb_composite_boot_usb_recovery_restart(void);

#if defined(REMOTEMAPPER_DWC2_DRIVER)
// Bounded laboratory test: sends only idle reports and yields to real input.
typedef struct {
    bool active;
    bool user_aborted;
    uint32_t test_id;
    uint32_t duration_ms;
    uint32_t started_ms;
    uint32_t ended_ms;
    uint32_t attempted;
    uint32_t completed;
    uint32_t failed;
    char stop_reason[20];
} usb_hid_stress_stats_t;
bool usb_hid_stress_start(uint32_t seconds);
void usb_hid_stress_stop(void);
void usb_hid_stress_get(usb_hid_stress_stats_t *stats);
#endif

/**
 * @brief Enqueue USB HID Keyboard Key Down (with modifier).
 * @return true when accepted by the bounded sender queue; this does not mean
 *         the host has received the report.
 */
bool usb_hid_keyboard_press(uint8_t modifier, uint8_t keycode);

/**
 * @brief Enqueue USB HID Keyboard Key Up (release all keys).
 * @return true when accepted as queued work or durable release debt; this does
 *         not mean the host has received the report.
 */
bool usb_hid_keyboard_release(void);

/**
 * @brief Enqueue an atomic USB HID Keyboard Tap (Press then Release).
 * @return true when accepted by the bounded sender queue; this does not mean
 *         the host has received either report.
 */
bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode);

/**
 * @brief Enqueue USB HID Consumer Control Code (e.g. Volume Up/Down, AC Back).
 * @return true when accepted by the bounded sender queue; this does not mean
 *         the host has received the report.
 */
bool usb_hid_consumer_press(uint16_t usage_code);

/**
 * @brief Enqueue USB HID Consumer Control release.
 * @return true when accepted as queued work or durable release debt; this does
 *         not mean the host has received the report.
 */
bool usb_hid_consumer_release(void);

/**
 * @brief Enqueue an atomic USB HID Consumer Tap.
 * @return true when accepted by the bounded sender queue; this does not mean
 *         the host has received either report.
 */
bool usb_hid_consumer_tap(uint16_t usage_code);

/**
 * @brief Dispatch high-level key action to USB HID
 */
void usb_hid_dispatch_action(const key_action_t *action);

/**
 * @brief Push 1ms frame of PCM audio to USB UAC Isochronous IN endpoint (32 bytes = 16 samples @ 16kHz)
 */
void usb_audio_task(void);

/**
 * @brief Stuck-key guard configuration (units in ms).
 */
typedef struct {
    uint32_t mod_ms;   // Modifier held longer than this -> force release (0 = off)
    uint32_t key_ms;   // Non-voice key down with zero output -> force release (0 = off)
    uint32_t voice_ms; // Voice ceiling, clamped to 1000..62000 ms (0 selects 62000 ms)
} usb_guard_config_t;

/**
 * @brief Stuck-key guard live statistics.
 */
typedef struct {
    uint32_t forced_releases;   // Total times the guard forced a release
    uint32_t last_force_ms;     // ms tick of the last forced release (0 = never)
    char     last_reason[24];   // Reason string of the last forced release
    bool     any_held;          // true while any HID hold is currently registered
    bool keyboard_pending;
    bool consumer_pending;
    uint8_t desired_modifier;
    uint32_t tx_complete;
    uint32_t tx_failed;
    uint32_t pending_ms;
    uint32_t usb_recoveries;
    bool recovery_exhausted;
    bool transport_recovering; // submissions gated until a fresh host mount
    uint32_t held_count;        // Number of currently registered held entries
} usb_guard_stats_t;

/**
 * @brief Get current guard config + live stats.
 */
bool usb_composite_guard_get(usb_guard_config_t *cfg, usb_guard_stats_t *stats);

/**
 * @brief Set guard config (persisted to NVS).
 */
bool usb_composite_guard_set(const usb_guard_config_t *cfg);

/**
 * @brief Immediately force-release all held HID keys/consumer/voice session,
 *        in a consistent way (engine + USB report + audio + LED) and log the reason.
 */
void usb_composite_force_release_all(const char* reason);
// Cancels queued HID work on a remote disconnect and leaves durable zero reports.
void usb_composite_cancel_hid_epoch(void);

#ifdef __cplusplus
}
#endif
