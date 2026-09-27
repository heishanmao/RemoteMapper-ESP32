#include "audio_pipeline.h"
#include <string.h>

extern void app_log(const char* tag, const char* format, ...);

audio_pipeline_t g_audio_pipeline;

void audio_pipeline_init(audio_pipeline_t *pipeline) {
    if (!pipeline) return;
    memset(pipeline, 0, sizeof(audio_pipeline_t));
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_init(&pipeline->agc);
    audio_ring_buffer_init(&pipeline->ring_buf, pipeline->ring_storage, AUDIO_RING_BUFFER_SIZE);
    pipeline->active = false;
    pipeline->buffering = false;
    pipeline->session_id = 0;
    pipeline->total_frames_decoded = 0;
    pipeline->total_samples_pushed = 0;
}

void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id) {
    if (!pipeline) return;
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_reset(&pipeline->agc);
    audio_ring_buffer_clear(&pipeline->ring_buf);
    pipeline->session_id = session_id;
    pipeline->active = true;
    pipeline->buffering = true; // Wait for initial prefill cushion to prevent jitter underruns
    pipeline->lead_mute_remaining = AUDIO_LEAD_MUTE_SAMPLES;
    pipeline->fade_in_remaining = AUDIO_FADE_IN_SAMPLES;
    pipeline->total_frames_decoded = 0;
    pipeline->total_samples_pushed = 0;
    pipeline->underrun_count = 0;
}

void audio_pipeline_sync(audio_pipeline_t *pipeline, int16_t predictor, int8_t step_index) {
    if (!pipeline) return;
    adpcm_sync_state(&pipeline->adpcm, predictor, step_index);
}

size_t audio_pipeline_feed_adpcm(audio_pipeline_t *pipeline, const uint8_t *adpcm_bytes, size_t len) {
    if (!pipeline || !adpcm_bytes || len == 0 || !pipeline->active) return 0;

    size_t samples_decoded = adpcm_decode_frame(&pipeline->adpcm, adpcm_bytes, len, pipeline->temp_pcm);
    if (samples_decoded == 0) return 0;

    // 1. Declip (single-sample spike eliminator, threshold 1000)
    audio_filter_declip(&pipeline->filter, pipeline->temp_pcm, samples_decoded, DECLIP_THRESHOLD);

    // 2. 3-Tap Triangle FIR Lowpass [0.25, 0.5, 0.25]
    audio_filter_lowpass(&pipeline->filter, pipeline->temp_pcm, samples_decoded);

    // 3. DC-Blocker (strips ~0-80Hz sub-bass and AC hum)
    audio_filter_dc_block(&pipeline->filter, pipeline->temp_pcm, samples_decoded);

    // 4. Dynamic AGC + Soft Clip
    // KEY: During the lead mute period, zero the samples BEFORE AGC so the peak envelope
    // sees silence and does NOT decay. This keeps gain locked at 1.0x (soft-start).
    // When mute ends, the AGC is still at 1.0x and can only rise from real audio -> no burst.
    for (size_t i = 0; i < samples_decoded; i++) {
        if (pipeline->lead_mute_remaining > 0) {
            // Feed zeros to AGC so it doesn't decay the peak during silence
            pipeline->temp_pcm[i] = 0;
            pipeline->lead_mute_remaining--;
        }
        // fade_in samples pass through AGC normally (they're real audio, just new)
    }
    audio_agc_process(&pipeline->agc, pipeline->temp_pcm, samples_decoded);

    // 5. Post-Processing: Micro Fade-In (10ms) applied AFTER AGC
    // The mute already zeroed the first 150ms. This 10ms fade-in smooths the
    // hard boundary transition. It is too short (160 samples) to create audible
    // noise artifacts, but long enough to eliminate any hard step discontinuity.
    for (size_t i = 0; i < samples_decoded; i++) {
        if (pipeline->fade_in_remaining > 0) {
            uint32_t step = AUDIO_FADE_IN_SAMPLES - pipeline->fade_in_remaining;
            pipeline->temp_pcm[i] = (int16_t)(((int32_t)pipeline->temp_pcm[i] * (int32_t)step) / AUDIO_FADE_IN_SAMPLES);
            pipeline->fade_in_remaining--;
        }
    }

    // 6. Enqueue into ring buffer
    size_t written = audio_ring_buffer_write(&pipeline->ring_buf, pipeline->temp_pcm, samples_decoded);

    pipeline->total_frames_decoded++;
    pipeline->total_samples_pushed += written;

    // Check if prefill threshold reached
    if (pipeline->buffering) {
        if (audio_ring_buffer_available_read(&pipeline->ring_buf) >= AUDIO_JITTER_PREFILL_SAMPLES) {
            pipeline->buffering = false;
        }
    }

    return written;
}

size_t audio_pipeline_read_for_usb(audio_pipeline_t *pipeline, int16_t *out_pcm, size_t sample_count) {
    if (!pipeline || !out_pcm || sample_count == 0) return 0;

    if (!pipeline->active || pipeline->buffering) {
// Feed pure silence while idle or building the initial prefill cushion
// (AUDIO_JITTER_PREFILL_SAMPLES = 50ms).
        memset(out_pcm, 0, sample_count * sizeof(int16_t));
        return sample_count;
    }

    size_t avail = audio_ring_buffer_available_read(&pipeline->ring_buf);

    if (avail < sample_count) {
        // Surface underruns (starved ring) at most once every 5s so the web log
        // stays readable during long voice sessions instead of a 2s periodic beat.
        static uint32_t s_last_underrun_log_ms = 0;
        uint32_t now = millis();
        if (now - s_last_underrun_log_ms >= 5000) {
            s_last_underrun_log_ms = now;
            app_log("AUDIO_DBG", "Ring underrun: avail %d < %d req (underruns=%d)",
                (int)avail, (int)sample_count, (int)pipeline->underrun_count);
        }
        pipeline->underrun_count++;
        size_t n = audio_ring_buffer_read(&pipeline->ring_buf, out_pcm, avail);
        // Smooth hold / fade to 0 to prevent sharp sawtooth click
        int16_t last_val = (n > 0) ? out_pcm[n - 1] : 0;
        for (size_t i = n; i < sample_count; i++) {
            last_val = (int16_t)((last_val * 7) / 8); // Smooth exponential fade to 0
            out_pcm[i] = last_val;
        }
        return sample_count;
    }

    audio_ring_buffer_read(&pipeline->ring_buf, out_pcm, sample_count);
    return sample_count;
}

void audio_pipeline_stop_session(audio_pipeline_t *pipeline) {
    if (!pipeline) return;
    pipeline->active = false;
    pipeline->buffering = false;
    pipeline->lead_mute_remaining = 0;
    pipeline->fade_in_remaining = 0;
    audio_ring_buffer_clear(&pipeline->ring_buf);
}
