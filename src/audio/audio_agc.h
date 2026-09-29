#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float peak;             // Current peak envelope value
    float inv_peak;         // Cached target_level / peak, kept in sync by audio_agc_process
    float target_level;     // Target level (default 28000.0)
    float decay_rate;       // Envelope decay multiplier (default 0.9997)
    float max_gain;         // Maximum allowed gain boost (default 30.0)
    float noise_floor;      // Minimum floor to avoid amplifying silence (default 200.0)
    float inv_floor;        // Cached 1.0 / noise_floor
} audio_agc_t;

/**
 * @brief Initialize AGC state with default parameters
 */
void audio_agc_init(audio_agc_t *agc);

/**
 * @brief Reset AGC peak envelope (e.g. at speech start)
 */
void audio_agc_reset(audio_agc_t *agc);

/**
 * @brief Process PCM samples in-place with AGC gain and soft clipping
 * @param agc AGC configuration & state
 * @param samples Array of 16-bit PCM samples
 * @param count Number of samples
 */
void audio_agc_process(audio_agc_t *agc, int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
