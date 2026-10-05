// Runtime tests include the production implementation directly. RTOS critical
// sections are single-thread no-ops in this deterministic state/ordering test;
// actual cross-core exclusion remains a target-only integration property.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/audio/audio_pipeline.c"
#include "../../src/audio/audio_ring_buffer.c"
#include "../../src/audio/adpcm_decoder.c"
#include "../../src/audio/audio_filter.c"
#include "../../src/audio/audio_agc.c"

static uint32_t s_test_now_ms;
uint32_t millis(void) { return s_test_now_ms; }
void app_log(const char*, const char*, ...) {}

int main() {
    // The clear watermark is fixed at request time. New samples published before
    // the consumer acknowledges that request remain readable.
    audio_ring_buffer_t ring = {};
    int16_t storage[8] = {};
    assert(audio_ring_buffer_init(&ring, storage, 8));
    const int16_t old_samples[] = {11, 12};
    const int16_t new_samples[] = {21, 22, 23};
    assert(audio_ring_buffer_write(&ring, old_samples, 2) == 2);
    audio_ring_buffer_clear(&ring);
    assert(audio_ring_buffer_write(&ring, new_samples, 3) == 3);
    audio_ring_buffer_consume_pending_clear(&ring);
    int16_t out[3] = {};
    assert(audio_ring_buffer_read(&ring, out, 3) == 3);
    assert(out[0] == 21 && out[1] == 22 && out[2] == 23);

    audio_pipeline_t pipeline = {};
    audio_pipeline_init(&pipeline);
    audio_resample_status_t status = {};
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.requested_num == 1 && status.requested_den == 1);
    assert(status.applied_num == 1 && status.applied_den == 1 && !status.pending);

    // Setter success means accepted/requested. Applied remains unchanged until
    // BLE prepares the next session; equivalent fractions are normalized.
    assert(audio_pipeline_set_resample(&pipeline, 8, 6));
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.requested_num == 4 && status.requested_den == 3);
    assert(status.applied_num == 1 && status.applied_den == 1 && status.pending);
    assert(!audio_pipeline_set_resample(&pipeline, 1, 3));
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.requested_num == 4 && status.requested_den == 3 && status.pending);

    audio_pipeline_prepare_session(&pipeline, 7);
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.applied_num == 4 && status.applied_den == 3 && !status.pending);
    assert(pipeline.rs_interval == 4 && pipeline.rs_step == 3);
    audio_pipeline_start_session(&pipeline, 7);
    assert(audio_pipeline_is_active(&pipeline));
    assert(audio_pipeline_is_buffering(&pipeline));

    // A request during a session does not reset phase or the live ratio.
    pipeline.rs_phase = 2;
    pipeline.rs_primed = true;
    assert(audio_pipeline_set_resample(&pipeline, 3, 2));
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.applied_num == 4 && status.applied_den == 3 && status.pending);
    assert(pipeline.rs_interval == 4 && pipeline.rs_step == 3);
    assert(pipeline.rs_phase == 2 && pipeline.rs_primed);

    // A new BLE session boundary applies the queued ratio, resets resampler
    // phase, and invalidates any old in-flight frame by advancing the epoch.
    const uint32_t old_epoch = pipeline.session_epoch;
    audio_pipeline_prepare_session(&pipeline, 8);
    assert(pipeline.session_epoch != old_epoch);
    assert(!audio_pipeline_is_active(&pipeline));
    assert(!pipeline.rs_primed && pipeline.rs_phase == 0);
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.applied_num == 3 && status.applied_den == 2 && !status.pending);

    // Prepare old-frame bytes without publishing head, then stop/restart. The
    // old epoch's commit is rejected; the new epoch can publish and the clear
    // acknowledgement does not discard its first samples.
    audio_pipeline_start_session(&pipeline, 8); // consumes prepared_start
    int16_t stale_samples[] = {31, 32};
    uint32_t stale_head = 0;
    size_t stale_count = audio_ring_buffer_prepare_write(
            &pipeline.ring_buf, stale_samples, 2, &stale_head);
    assert(stale_count == 2);
    const uint32_t stale_epoch = pipeline.session_epoch;
    audio_pipeline_stop_session(&pipeline);
    assert(audio_pipeline_commit_prepared_write(
                &pipeline, stale_epoch, stale_head, stale_count, true) == 0);

    audio_pipeline_start_session(&pipeline, 9);
    const uint32_t current_epoch = pipeline.session_epoch;
    int16_t fresh_samples[] = {41, 42};
    uint32_t fresh_head = 0;
    size_t fresh_count = audio_ring_buffer_prepare_write(
            &pipeline.ring_buf, fresh_samples, 2, &fresh_head);
    assert(fresh_count == 2);
    assert(audio_pipeline_commit_prepared_write(
                &pipeline, current_epoch, fresh_head, fresh_count, true) == 2);
    audio_pipeline_consume_pending_clear(&pipeline);
    int16_t fresh_out[2] = {};
    assert(audio_ring_buffer_read(&pipeline.ring_buf, fresh_out, 2) == 2);
    assert(fresh_out[0] == 41 && fresh_out[1] == 42);

    audio_pipeline_stop_session(&pipeline);
    assert(!audio_pipeline_is_active(&pipeline));

    // The largest valid uint16 denominator is accepted. Full-scale alternating
    // input makes (delta * phase) exceed int32; interpolation must still stay
    // within the mathematically expected full-scale interval.
    assert(audio_pipeline_set_resample(&pipeline, 65535, 65534));
    audio_pipeline_prepare_session(&pipeline, 10);
    audio_pipeline_get_resample_status(&pipeline, &status);
    assert(status.applied_num == 65535 && status.applied_den == 65534);
    const int16_t alternating[] = {-32768, 32767, -32768, 32767};
    int16_t resampled[12] = {};
    pipeline.rs_primed = false;
    const size_t resampled_count = audio_resample_up(
            &pipeline, alternating, 4, resampled);
    assert(resampled_count >= 4);
    assert(resampled[1] == 32766);
    for (size_t i = 0; i < resampled_count; ++i) {
        assert(resampled[i] >= -32768 && resampled[i] <= 32767);
    }
    return 0;
}
