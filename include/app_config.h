#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Audio & DSP Parameters
// ==========================================
#define AUDIO_SAMPLE_RATE         16000     // 16kHz sample rate
#define AUDIO_BITS_PER_SAMPLE     16        // 16-bit PCM
#define AUDIO_CHANNELS            1         // Mono
#define AUDIO_DEFAULT_FRAME_BYTES 120       // Default 120 bytes ADPCM per BLE frame
#define AUDIO_DEFAULT_FRAME_SAMPS 240       // 120 * 2 = 240 PCM samples per frame

// The RC003's true capture rate is 16 kHz, proved by the Goertzel spectral probe
// in audio_pipeline.c: the 6.5 kHz bin measures 0.0 dB relative to the strongest
// bin, i.e. as loud as the peak. Under a genuine 12 kHz source 6.5 kHz is above
// the 6 kHz Nyquist and could not appear at all; Goertzel leakage alone would
// land 20-40 dB down.
//
// What we actually receive is 240 samples per 120-byte frame at ~50 fps, i.e.
// 12000 samples/s: 75% of what a 16 kHz stream carries. So the earlier
// "12080 samples/s == 12 kHz content" reading was an arithmetic coincidence, not
// a sample rate. The audio arriving here is genuine 16 kHz content with about a
// quarter of the samples missing, which is why:
//
//   - 1:1 is the correct ratio. Playback pitch is already right, and the audible
//     defect is gaps, not pitch. Upsampling cannot restore audio that was never
//     transmitted, so stretching by 4/3 only buries the gaps while shifting
//     everything down by ~4 semitones.
//   - recovering the missing samples is a link problem, not a DSP one. A full
//     16 kHz stream needs 240 samples every 15 ms = 66.7 notifications/s; we
//     sustain ~50/s. See AUDIO_CONN_* in ble_remote_client.cpp, which asks for a
//     7.5-15 ms connection interval to lift that ceiling.
#define AUDIO_REMOTE_SAMPLE_RATE  16000     // spectral-probe confirmed

// Default output/input ratio as an exact fraction, applied by
// audio_pipeline_init(). 1/1 is correct per the note above; the runtime
// /api/audio/resample endpoint can override it for A/B comparison.
// AUDIO_RS_STEP_Q is retained only for the disabled-resampler sentinel.
#define AUDIO_RS_STEP_Q           1
// Accepted resampler ratio window, as a percentage of 1:1. Below 100% the UAC
// consumer starves; above ~200% the work buffer could overflow. Both bounds are
// enforced by audio_pipeline_set_resample().
#define AUDIO_RS_MIN_RATIO_PCT    100
#define AUDIO_RS_MAX_RATIO_PCT    200
// Worst case for the resampler is ceil(interval/step)+1 outputs per input
// sample. At the 2:1 ceiling that is 3x the input frame, so size for that.
#define AUDIO_WORK_SAMPLES        (AUDIO_DEFAULT_FRAME_SAMPS * 3)

#define AUDIO_RING_BUFFER_SIZE    8192      // Ring buffer capacity (in samples, ~512ms buffer)

// AGC & Filter parameters
#define AGC_TARGET_LEVEL          28000.0f
#define AGC_DECAY_RATE            0.9997f
#define AGC_MAX_GAIN              30.0f
#define AGC_NOISE_FLOOR           200.0f
#define AGC_INITIAL_PEAK          28000.0f  // Soft-start: initial gain = 1.0x (0dB) to avoid 28x burst
#define AUDIO_LEAD_MUTE_SAMPLES   480       // 30ms silence zone (480 samples @ 16kHz) to eliminate button click & pops.
#define AUDIO_FADE_IN_SAMPLES     160       // 10ms micro fade-in (160 samples @ 16kHz) - only for transient smoothing at boundary
#define DECLIP_THRESHOLD          1000

// ==========================================
// 2. BLE Central & ATVV Parameters
// ==========================================
#define BLE_REMOTE_NAME_PREFIX    "MI RC"
#define BLE_REMOTE_MAC_PREFIX     "c0:5d:39"
#define BLE_SCAN_INTERVAL_MS      800
#define BLE_SCAN_WINDOW_MS        200
#define BLE_KEEP_ALIVE_INTERVAL   2000      // MIC_EXTEND interval during active voice (ms)

// ATVV GATT UUIDs (128-bit)
#define ATVV_SVC_UUID             "ab5e0001-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CMD_UUID        "ab5e0002-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_AUD_UUID        "ab5e0003-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CTL_UUID        "ab5e0004-5a21-4f05-bc7d-af01f617b664"

// HOGP UUIDs (Standard Bluetooth SIG 16-bit)
#define HOGP_SVC_UUID             0x1812
#define HOGP_REPORT_CHAR_UUID     0x2A4D

// ==========================================
// 3. FreeRTOS Task & Multi-Core Pinning
// ==========================================
#define TASK_CORE_BLE             0         // Core 0: BLE & Audio decoding
#define TASK_CORE_USB             1         // Core 1: TinyUSB & HID/Audio push

#define PRIO_TASK_USB             6         // Real-time USB Isochronous & HID
#define PRIO_TASK_AUDIO_DSP       5         // High-priority audio stream decoding
#define PRIO_TASK_BLE             4         // NimBLE client processing
#define PRIO_TASK_KEYMAP_CLI      3         // Key mapping state machine & CLI

// ==========================================
// 4. Default Keymap & Hotkey Codes
// ==========================================
// Default Voice Input Hotkey: Right Alt + Comma (WeChat Voice IME)
// HID Keyboard Modifiers: 0x01=LCTRL, 0x02=LSHIFT, 0x04=LALT, 0x08=LGUI, 0x10=RCTRL, 0x20=RSHIFT, 0x40=RALT, 0x80=RGUI
#define DEFAULT_VOICE_MODIFIER    0x40      // KEY_MOD_RALT
#define DEFAULT_VOICE_KEY         0x36      // HID Usage for Comma ','

// ==========================================
// 5. Wi-Fi Power Management
// ==========================================
// Policy values are defined by wifi_policy_t in src/wifi/wifi_manager.h
// (WIFI_POLICY_ALWAYS_ON=0, WIFI_POLICY_ON_DEMAND=1, WIFI_POLICY_DISABLED=2).
// The defaults below stay plain integers so this C-safe header stays decoupled.
#define WIFI_DEFAULT_POLICY       1         // default = WIFI_POLICY_ON_DEMAND
#define WIFI_DEFAULT_TIMEOUT_MIN  5         // Default ON_DEMAND idle timeout (minutes)
#define WIFI_TIMEOUT_NEVER        0         // 0 = keep radio on until manual off

// 5-press wake gesture: press any remote key N times within the window to wake Wi-Fi
// after the ON_DEMAND idle power-down.
#define WIFI_WAKE_PRESS_THRESHOLD 5
#define WIFI_WAKE_PRESS_WINDOW_MS 5000

// ==========================================
// 6. Stuck-Key Guard (Anti-Stuck Safety Net)
// ==========================================
// Automatic forced-release rules that keep the PC-side HID state clean even when
// a release never arrives (dropped BLE packet, remote battery death, etc.).
// 0 disables a rule. Values are milliseconds.
#define HID_GUARD_MOD_SOLO_MS      20000    // Any modifier held > 20s (independent of other keys)
#define HID_GUARD_KEY_IDLE_MS      60000    // Non-voice key held with zero output for > 60s
#define HID_GUARD_VOICE_EXTREME_MS 900000   // Absolute ceiling for a voice recording (15 min)

#ifdef __cplusplus
}
#endif
