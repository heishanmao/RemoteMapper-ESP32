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

typedef struct {
    adpcm_state_t        adpcm;
    audio_filter_state_t filter;
    audio_agc_t          agc;
    audio_ring_buffer_t  ring_buf;
    int16_t              ring_storage[AUDIO_RING_BUFFER_SIZE];
    int16_t              temp_pcm[AUDIO_DEFAULT_FRAME_SAMPS * 2];
    uint8_t              session_id;
    bool                 active;
    bool                 buffering;
    uint32_t             total_frames_decoded;
    uint32_t             total_samples_pushed;
    uint32_t             underrun_count;
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
