#pragma once

#include <stdint.h>

// Primitive-only per-session diagnostics state. Callers provide synchronization
// when this state is shared between the NimBLE callback and the loop task.
typedef struct {
    uint32_t frames;
    uint32_t len_other;
    uint32_t interval_count;
    uint32_t interval_sum_ms;
    uint32_t interval_max_ms;
    uint32_t long_intervals;
    uint32_t decode_us_sum;
    uint32_t decode_us_max;
    uint32_t decode_count;
    uint32_t partial;
    uint32_t decode_drop;
    uint32_t first_rx_ms;
    uint32_t last_rx_ms;
    uint32_t first_rx_frames;
    uint32_t has_rx;
} ble_audio_diag_window_t;

// Pure timing primitives are constexpr-compatible with the firmware's GNU++11
// mode so host-independent static assertions can exercise the production math.
static constexpr uint32_t ble_audio_diag_elapsed_between(uint32_t first_ms, uint32_t last_ms) {
    return (uint32_t)(last_ms - first_ms);
}

static constexpr uint32_t ble_audio_diag_rate_x10(uint32_t frame_count, uint32_t elapsed_ms) {
    return elapsed_ms ? (frame_count * 10000u / elapsed_ms) : 0u;
}

static constexpr uint32_t ble_audio_diag_window_rate_x10(uint32_t frames,
                                                          uint32_t first_rx_frames,
                                                          uint32_t elapsed_ms,
                                                          uint32_t has_rx) {
    return (has_rx && frames >= first_rx_frames)
        ? ble_audio_diag_rate_x10(frames - first_rx_frames, elapsed_ms) : 0u;
}

static constexpr uint32_t ble_audio_diag_first_sample_frames(uint32_t prior_frames,
                                                              uint32_t decoded_frames) {
    return prior_frames + decoded_frames;
}

static inline void ble_audio_diag_reset(ble_audio_diag_window_t* w) {
    *w = {};
}

static inline void ble_audio_diag_record(ble_audio_diag_window_t* w,
                                         uint32_t now_ms,
                                         uint32_t interval_ms,
                                         uint32_t decoded_frames,
                                         uint32_t decode_us,
                                         uint32_t len_other,
                                         uint32_t partial,
                                         uint32_t decode_drop,
                                         uint32_t long_interval_ms) {
    if (!w->has_rx) {
        w->first_rx_ms = now_ms;
        w->first_rx_frames = ble_audio_diag_first_sample_frames(w->frames, decoded_frames);
        w->has_rx = 1;
    }
    w->last_rx_ms = now_ms;
    w->frames += decoded_frames;
    w->len_other += len_other;
    w->partial += partial;
    w->decode_drop += decode_drop;
    if (interval_ms != 0) {
        w->interval_count++;
        w->interval_sum_ms += interval_ms;
        if (interval_ms > w->interval_max_ms) w->interval_max_ms = interval_ms;
        if (interval_ms >= long_interval_ms) w->long_intervals++;
    }
    if (decoded_frames != 0) {
        w->decode_count++;
        w->decode_us_sum += decode_us;
        if (decode_us > w->decode_us_max) w->decode_us_max = decode_us;
    }
}

static inline uint32_t ble_audio_diag_elapsed_ms(const ble_audio_diag_window_t* w) {
    // Unsigned subtraction is wrap-safe for the short diagnostic windows.
    return w->has_rx ? ble_audio_diag_elapsed_between(w->first_rx_ms, w->last_rx_ms) : 0;
}

static inline uint32_t ble_audio_diag_fps_x10(const ble_audio_diag_window_t* w) {
    // Measure only frames received after the initial sample over the observed
    // first-to-last span. This excludes the first frame's prior unknown time.
    const uint32_t elapsed = ble_audio_diag_elapsed_ms(w);
    return ble_audio_diag_window_rate_x10(w->frames, w->first_rx_frames,
                                          elapsed, w->has_rx);
}
