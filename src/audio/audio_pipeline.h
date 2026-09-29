#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "app_config.h"
#include "adpcm_decoder.h"
#include "audio_filter.h"
#include "audio_agc.h"
#include "audio_ring_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_JITTER_PREFILL_SAMPLES  800   // 50ms prefill cushion to absorb BLE jitter

// Spectral probe on the decoded (pre-resample) sample sequence.
//
// Decoded frames are 240 samples arriving every 20 ms, so consecutive samples
// are 1/12000 s apart. Under that assumption 6 kHz is Nyquist: content above it
// is impossible for a genuine 12 kHz source, so real energy at 6.5-8 kHz proves
// the remote captured at a higher rate (16 kHz) and ships only 75% of the
// samples. That one measurement decides the resample ratio without guessing.
#define AUDIO_SPEC_BINS       8
#define AUDIO_SPEC_BIN0_HZ    3000
#define AUDIO_SPEC_BIN_HZ_STEP 500
#define AUDIO_SPEC_MAX_HZ     (AUDIO_SPEC_BIN0_HZ + (AUDIO_SPEC_BINS - 1) * AUDIO_SPEC_BIN_HZ_STEP)
// Probe one frame in N. The probe is purely diagnostic and its cost lands in the
// NimBLE host task, so keep the duty cycle low.
#define AUDIO_SPEC_STRIDE     25

typedef struct {
    adpcm_state_t        adpcm;
    audio_filter_state_t filter;
    audio_agc_t          agc;
    audio_ring_buffer_t  ring_buf;
    int16_t              ring_storage[AUDIO_RING_BUFFER_SIZE];
    int16_t              decode_buf[AUDIO_DEFAULT_FRAME_SAMPS];  // native rate, as decoded
    int16_t              temp_pcm[AUDIO_WORK_SAMPLES];           // AUDIO_SAMPLE_RATE, post-resample
    // Rational resampler state, carried across frame boundaries.
    //   rs_phase    read position inside the current [rs_prev, next) input interval,
    //               measured in 1/rs_interval units, so it is in [0, rs_interval)
    //   rs_step     how far the read position advances per output sample
    // Output/input ratio is therefore rs_interval / rs_step, held exactly.
    // Runtime-tunable because the remote's true rate was measured two different
    // ways that disagreed (arrival arithmetic said 12 kHz, pitch said otherwise).
    int32_t              rs_prev;
    uint32_t             rs_phase;
    uint32_t             rs_interval;
    uint32_t             rs_step;
    bool                 rs_primed;
    // Running energy per spectral bin, accumulated over decoded frames.
    uint32_t             spec_mag[AUDIO_SPEC_BINS];
    uint32_t             spec_peak[AUDIO_SPEC_BINS];
    uint32_t             spec_frames;   // frame counter driving AUDIO_SPEC_STRIDE
    uint8_t              session_id;
    bool                 active;
    bool                 buffering;
    uint32_t             total_frames_decoded;
    uint32_t             total_samples_pushed;
    uint32_t             underrun_count;
    // Diagnostics: a short read is not the same as a starved ring. A partial
    // chunk is normal jitter (a few samples of fade-out are inserted); avail==0
    // means the producer fell behind real time and audio was genuinely lost.
    uint32_t             partial_count;      // 0 < avail < requested
    uint32_t             starve_count;       // avail == 0
    uint32_t             padded_samples;     // silence samples substituted
    uint32_t             wire_samples;       // real samples delivered
    uint32_t             lead_mute_remaining;
    uint32_t             fade_in_remaining;
} audio_pipeline_t;

/**
 * @brief Global audio pipeline instance
 */
extern audio_pipeline_t g_audio_pipeline;

/**
 * @brief Initialize audio pipeline
 */
void audio_pipeline_init(audio_pipeline_t *pipeline);

/**
 * @brief Start a new speech session (resets filter, AGC, syncs state)
 */
void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id);

/**
 * @brief Apply ATVV AUDIO_SYNC (0x0A) state reset
 */
void audio_pipeline_sync(audio_pipeline_t *pipeline, int16_t predictor, int8_t step_index);

/**
 * @brief Feed raw ADPCM frame from BLE, decode, filter, AGC, and push to ring buffer
 * @param pipeline Pipeline handle
 * @param adpcm_bytes Raw BLE ADPCM payload
 * @param len Byte count
 * @return Number of PCM samples produced and enqueued
 */
bool audio_pipeline_set_resample(audio_pipeline_t *pipeline, uint16_t num, uint16_t den);
void audio_pipeline_get_resample(const audio_pipeline_t *pipeline, uint16_t *num, uint16_t *den);
// Scaled magnitudes, normalised so the strongest bin reads 1000.
void audio_pipeline_get_spectrum(const audio_pipeline_t *pipeline, uint16_t out[AUDIO_SPEC_BINS]);
// Raw accumulated energy per bin, for offline analysis without the scaling.
void audio_pipeline_get_spectrum_raw(const audio_pipeline_t *pipeline, uint32_t out[AUDIO_SPEC_BINS]);

size_t audio_pipeline_feed_adpcm(audio_pipeline_t *pipeline, const uint8_t *adpcm_bytes, size_t len);

/**
 * @brief Read PCM samples for USB UAC Isochronous IN endpoint
 * @param pipeline Pipeline handle
 * @param out_pcm Destination buffer
 * @param sample_count Number of samples requested
 * @return Number of samples read
 */
size_t audio_pipeline_read_for_usb(audio_pipeline_t *pipeline, int16_t *out_pcm, size_t sample_count);

/**
 * @brief Stop active speech session
 */
void audio_pipeline_stop_session(audio_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif
