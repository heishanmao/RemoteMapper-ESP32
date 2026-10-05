// Compile-only assertions exercise the same constexpr primitives used by the
// production callback's first-sample baseline and fps calculation.
#include "ble_audio_diagnostics.h"

// The first notification after a long idle establishes the baseline after its
// frames are counted; no idle interval or first-frame time is inferred.
static_assert(ble_audio_diag_first_sample_frames(0u, 1u) == 1u,
              "First sample frames must be excluded from observed-span rate");
static_assert(ble_audio_diag_window_rate_x10(1u, 1u, 0u, 1u) == 0u,
              "A one-frame window has no measured interval and reports zero fps");
static_assert(ble_audio_diag_window_rate_x10(1u, 1u, 0u, 0u) == 0u,
              "No received sample means no rate");
static_assert(ble_audio_diag_window_rate_x10(2u, 1u, 15u, 1u) == 666u,
              "Two frames over a 15 ms observed span must not double count the first frame");

// millis() wraps at 32 bits: this pair is exactly 15 ms apart.
static_assert(ble_audio_diag_elapsed_between(0xfffffff8u, 0x00000007u) == 15u,
              "Elapsed time must remain correct across millis wrap");
static_assert(ble_audio_diag_window_rate_x10(
                  2u, 1u, ble_audio_diag_elapsed_between(0xfffffff8u, 0x00000007u), 1u) == 666u,
              "The measured rate across millis wrap must remain correct");
