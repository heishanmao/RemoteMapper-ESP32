#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "app_config.h"
#include "adpcm_decoder.h"
#include "audio_filter.h"
#include "audio_agc.h"
#include "audio_ring_buffer.h"
#include "audio_resample_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_JITTER_PREFILL_SAMPLES  800   // 50ms prefill cushion to absorb BLE jitter

// Spectral probe on the decoded (pre-resample) sample sequence.
//
// The current working interpretation is 240 decoded samples per frame at about
// 66.7 frames/s (roughly 16 kHz), so the default output path is 1:1. Spectral
// energy in the measured bins above 6 kHz would challenge a 12 kHz source-rate
// hypothesis (whose Nyquist is 6 kHz), but this diagnostic alone does not prove
// a fixed sample-loss fraction or the exact capture rate.
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
    int16_t              ring_fallback[AUDIO_RING_FALLBACK_SIZE];
    int16_t              decode_buf[AUDIO_DEFAULT_FRAME_SAMPS];  // native rate, as decoded
    int16_t              temp_pcm[AUDIO_WORK_SAMPLES];           // AUDIO_SAMPLE_RATE, post-resample
    // Rational resampler state, carried across frame boundaries.
    //   rs_phase    read position inside the current [rs_prev, next) input interval,
    //               measured in 1/rs_interval units, so it is in [0, rs_interval)
    //   rs_step     how far the read position advances per output sample
    // Output/input ratio is therefore rs_interval / rs_step, held exactly.
    // Runtime-tunable for diagnostics, but changes are queued and applied only
    // at a producer-owned session boundary.
    int32_t              rs_prev;
    uint32_t             rs_phase;
    uint32_t             rs_interval;
    uint32_t             rs_step;
    bool                 rs_primed;
    uint32_t             rs_requested_packed; // atomic, can be written from web/core 1
    uint32_t             rs_applied_packed;   // atomic, applied by BLE/core 0 only
    // Running energy per spectral bin, accumulated over decoded frames.
    uint32_t             spec_mag[AUDIO_SPEC_BINS];
    uint32_t             spec_peak[AUDIO_SPEC_BINS];
    uint32_t             spec_frames;   // frame counter driving AUDIO_SPEC_STRIDE
    uint8_t              session_id;
    volatile bool        active;    // accessed through atomic helpers across cores
    volatile bool        buffering; // accessed through atomic helpers across cores
    // Session control is protected by the short audio-session mux in .c.
    uint32_t             session_epoch;
    uint32_t             reset_epoch;
    uint8_t              reset_session_id;
    bool                 reset_pending;
    bool                 prepared_start;
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
 * @brief Publish a new speech session; BLE producer applies pending reset safely
 */
void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id);

/**
 * @brief Apply ATVV AUDIO_SYNC (0x0A) state reset
 */
void audio_pipeline_sync(audio_pipeline_t *pipeline, int16_t predictor, int8_t step_index);

/** @brief Queue a validated output/input ratio for the next audio session. */
bool audio_pipeline_set_resample(audio_pipeline_t *pipeline, uint16_t num, uint16_t den);
typedef struct {
    uint16_t requested_num;
    uint16_t requested_den;
    uint16_t applied_num;
    uint16_t applied_den;
    bool pending;
} audio_resample_status_t;
// Compatibility getter reports the accepted/requested ratio, which may still
// be pending. Use the status getter to distinguish applied from requested.
void audio_pipeline_get_resample(const audio_pipeline_t *pipeline, uint16_t *num, uint16_t *den);
void audio_pipeline_get_resample_status(const audio_pipeline_t *pipeline, audio_resample_status_t *out);
// Called by the BLE producer at a session boundary. USB start_session only
// publishes a bounded reset request; this function is producer-owned.
void audio_pipeline_prepare_session(audio_pipeline_t *pipeline, uint8_t session_id);
bool audio_pipeline_is_active(const audio_pipeline_t *pipeline);
bool audio_pipeline_is_buffering(const audio_pipeline_t *pipeline);
// USB consumer wrapper keeps clear acknowledgement ordered with start/stop and
// producer head commits. Call only from the single USB audio consumer task.
void audio_pipeline_consume_pending_clear(audio_pipeline_t *pipeline);
// Scaled magnitudes, normalised so the strongest bin reads 1000.
void audio_pipeline_get_spectrum(const audio_pipeline_t *pipeline, uint16_t out[AUDIO_SPEC_BINS]);
// Raw accumulated energy per bin, for offline analysis without the scaling.
void audio_pipeline_get_spectrum_raw(const audio_pipeline_t *pipeline, uint32_t out[AUDIO_SPEC_BINS]);

/**
 * @brief Feed raw BLE ADPCM, decode/filter/resample it, and enqueue PCM.
 * @param pipeline Pipeline handle
 * @param adpcm_bytes Raw BLE ADPCM payload
 * @param len Byte count
 * @return Number of PCM samples produced and enqueued
 */
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
