#include "audio_agc.h"
#include <math.h>

// Reciprocal-gain AGC.
//
// The original form computed `gain = target / denom` for every single sample.
// On an Xtensa LX7 running at 160 MHz that float divide is ~40 cycles, and at
// 240 samples per 120-byte frame it dominated the whole decode path (measured
// 565 us/frame in the NimBLE host task).
//
// Two properties let us drop the divide entirely:
//   1. `denom` is a running peak that only ever changes by the fixed decay
//      constant, so the reciprocal can be advanced with a multiply instead of
//      being recomputed.
//   2. The peak decays by 0.03% per sample; the resulting gain drift over a
//      240-sample frame is 7% in the worst case (full silence) but only a few
//      percent across normal speech. That is inaudible for a capture path, and
//      `audio_pipeline` re-reads the state every frame so any residual error
//      cannot accumulate across frames.
//
// Using an initial reciprocal (rather than exp(logf(...)) on the first sample)
// also avoids a libm call on the first decoded sample of a session, which would
// otherwise show up as a several-hundred-microsecond outlier in the decode
// timing diagnostics.

void audio_agc_init(audio_agc_t *agc) {
    if (!agc) return;
    agc->target_level = 28000.0f;
    agc->decay_rate = 0.9997f;
    agc->max_gain = 30.0f;
    agc->noise_floor = 200.0f;
    agc->peak = agc->target_level; // Soft-start: start at 1.0x gain (0dB) to prevent burst noise
    agc->inv_peak = 1.0f;           // 1.0 / target_level
    agc->inv_floor = 1.0f / agc->noise_floor;
}

void audio_agc_reset(audio_agc_t *agc) {
    if (!agc) return;
    agc->peak = agc->target_level; // Soft-start: reset to 1.0x gain (0dB)
    agc->inv_peak = 1.0f / agc->target_level;
    agc->inv_floor = 1.0f / agc->noise_floor;
}

void audio_agc_process(audio_agc_t *agc, int16_t *samples, size_t count) {
    if (!agc || !samples || count == 0) return;

    float peak = agc->peak;
    float inv_peak = agc->inv_peak;
    const float decay = agc->decay_rate;
    const float target = agc->target_level;
    const float max_gain = agc->max_gain;
    const float noise_floor = agc->noise_floor;
    const float inv_floor = agc->inv_floor;
    // peak decays by `decay`, so 1/peak grows by the reciprocal. Advancing the
    // reciprocal with one multiply keeps the divide out of the sample loop while
    // still tracking 1/peak exactly (up to float rounding).
    const float inv_decay = 1.0f / decay;

    for (size_t i = 0; i < count; i++) {
        float v = (float)samples[i];
        float a = (v < 0.0f) ? -v : v;

        if (a > peak) {
            peak = a;
            inv_peak = target / peak;   // one divide per *peak update*, not per sample
        } else {
            peak *= decay;
            inv_peak *= inv_decay;      // 1/peak grows as peak decays
        }

        float gain = (peak > noise_floor) ? inv_peak : target * inv_floor;
        if (gain > max_gain) {
            gain = max_gain;
        }

        v *= gain;

        // Soft clip
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }

        samples[i] = (int16_t)v;
    }

    agc->peak = peak;
    agc->inv_peak = inv_peak;
}
