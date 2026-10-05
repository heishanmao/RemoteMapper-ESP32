#include "audio_pipeline.h"
#include <string.h>
#include <math.h>
#include <Arduino.h>
#include <esp_heap_caps.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern void app_log(const char* tag, const char* format, ...);

audio_pipeline_t g_audio_pipeline;
static portMUX_TYPE s_audio_session_mux = portMUX_INITIALIZER_UNLOCKED;

bool audio_pipeline_set_resample(audio_pipeline_t *pipeline, uint16_t num, uint16_t den);
static void audio_spectrum_probe(audio_pipeline_t *pipeline, const int16_t *x, size_t n);

static uint32_t next_session_epoch(uint32_t epoch) {
    epoch++;
    return epoch ? epoch : 1u;
}

static void audio_pipeline_reset_producer_state(audio_pipeline_t *pipeline, uint8_t session_id) {
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_reset(&pipeline->agc);
    pipeline->rs_prev = 0;
    pipeline->rs_phase = 0;
    pipeline->rs_primed = false;
    memset(pipeline->spec_mag, 0, sizeof(pipeline->spec_mag));
    memset(pipeline->spec_peak, 0, sizeof(pipeline->spec_peak));
    pipeline->spec_frames = 0;
    __atomic_store_n(&pipeline->session_id, session_id, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->total_frames_decoded, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->total_samples_pushed, 0u, __ATOMIC_RELEASE);
    pipeline->lead_mute_remaining = AUDIO_LEAD_MUTE_SAMPLES;
    pipeline->fade_in_remaining = AUDIO_FADE_IN_SAMPLES;
}

static bool audio_pipeline_take_session_reset(audio_pipeline_t *pipeline,
                                              uint32_t *epoch,
                                              uint8_t *session_id) {
    bool claimed = false;
    portENTER_CRITICAL(&s_audio_session_mux);
    if (pipeline->reset_pending && pipeline->reset_epoch == pipeline->session_epoch) {
        *epoch = pipeline->session_epoch;
        *session_id = pipeline->reset_session_id;
        pipeline->reset_pending = false;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_audio_session_mux);
    return claimed;
}

static void audio_pipeline_apply_requested_ratio(audio_pipeline_t *pipeline);

static bool audio_pipeline_apply_session_reset(audio_pipeline_t *pipeline,
                                               uint32_t *session_epoch) {
    uint32_t epoch = 0;
    uint8_t session_id = 0;
    if (!audio_pipeline_take_session_reset(pipeline, &epoch, &session_id)) return false;
    // This function is called only from the BLE producer (feed/sync/session
    // preparation), so decoder/filter/resampler state has a single writer.
    audio_pipeline_apply_requested_ratio(pipeline);
    audio_pipeline_reset_producer_state(pipeline, session_id);
    if (session_epoch) *session_epoch = epoch;
    return true;
}

static bool audio_pipeline_begin_produce(audio_pipeline_t *pipeline, uint32_t *epoch_out) {
    bool apply_reset = false;
    uint32_t epoch = 0;
    uint8_t session_id = 0;
    portENTER_CRITICAL(&s_audio_session_mux);
    if (__atomic_load_n(&pipeline->active, __ATOMIC_ACQUIRE)) {
        epoch = pipeline->session_epoch;
        if (pipeline->reset_pending && pipeline->reset_epoch == epoch) {
            session_id = pipeline->reset_session_id;
            pipeline->reset_pending = false;
            apply_reset = true;
        }
    }
    portEXIT_CRITICAL(&s_audio_session_mux);
    if (!epoch) return false;
    if (apply_reset) {
        audio_pipeline_apply_requested_ratio(pipeline);
        audio_pipeline_reset_producer_state(pipeline, session_id);
    }
    portENTER_CRITICAL(&s_audio_session_mux);
    const bool valid = __atomic_load_n(&pipeline->active, __ATOMIC_ACQUIRE) &&
                       pipeline->session_epoch == epoch;
    portEXIT_CRITICAL(&s_audio_session_mux);
    if (valid && epoch_out) *epoch_out = epoch;
    return valid;
}

static size_t audio_pipeline_commit_prepared_write(audio_pipeline_t *pipeline,
                                                   uint32_t epoch,
                                                   uint32_t expected_head,
                                                   size_t prepared,
                                                   bool decoded_frame) {
    size_t written = 0;
    portENTER_CRITICAL(&s_audio_session_mux);
    if (__atomic_load_n(&pipeline->active, __ATOMIC_ACQUIRE) &&
            pipeline->session_epoch == epoch) {
        if (decoded_frame) {
            __atomic_fetch_add(&pipeline->total_frames_decoded, 1u, __ATOMIC_RELAXED);
        }
        if (prepared != 0 && audio_ring_buffer_commit_write(
                    &pipeline->ring_buf, expected_head, prepared)) {
            written = prepared;
            __atomic_fetch_add(&pipeline->total_samples_pushed, (uint32_t)written, __ATOMIC_RELAXED);
            if (__atomic_load_n(&pipeline->buffering, __ATOMIC_RELAXED) &&
                    audio_ring_buffer_peek_available(&pipeline->ring_buf) >= AUDIO_JITTER_PREFILL_SAMPLES) {
                __atomic_store_n(&pipeline->buffering, false, __ATOMIC_RELEASE);
            }
        }
    }
    portEXIT_CRITICAL(&s_audio_session_mux);
    return written;
}

void audio_pipeline_init(audio_pipeline_t *pipeline) {
    if (!pipeline) return;
    memset(pipeline, 0, sizeof(audio_pipeline_t));
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_init(&pipeline->agc);
    int16_t *ring_storage = (int16_t *)heap_caps_malloc(
            AUDIO_RING_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t ring_capacity = ring_storage ? AUDIO_RING_BUFFER_SIZE : AUDIO_RING_FALLBACK_SIZE;
    if (!ring_storage) {
        ring_storage = pipeline->ring_fallback;
        app_log("AUDIO", "PSRAM pre-roll allocation failed; using %u-sample fallback",
                (unsigned)ring_capacity);
    }
    audio_ring_buffer_init(&pipeline->ring_buf, ring_storage, ring_capacity);
    __atomic_store_n(&pipeline->active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->buffering, false, __ATOMIC_RELEASE);
    pipeline->session_id = 0;
    pipeline->total_frames_decoded = 0;
    pipeline->total_samples_pushed = 0;
    // Passthrough is the applied/requested startup default. Later API settings
    // are accepted atomically and applied only by the BLE producer at a session
    // boundary.
    pipeline->rs_interval = 1;
    pipeline->rs_step = 1;
    pipeline->rs_requested_packed = audio_rs_pack(1, 1);
    pipeline->rs_applied_packed = audio_rs_pack(1, 1);
}

void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id) {
    if (!pipeline) return;
    portENTER_CRITICAL(&s_audio_session_mux);
    if (pipeline->prepared_start) {
        // BLE already reset the producer state at AUDIO_START. Retain any
        // AUDIO_SYNC that arrived before this USB-side activation.
        pipeline->prepared_start = false;
    } else {
        pipeline->session_epoch = next_session_epoch(pipeline->session_epoch);
        pipeline->reset_epoch = pipeline->session_epoch;
        pipeline->reset_session_id = session_id;
        pipeline->reset_pending = true;
    }
    audio_ring_buffer_clear(&pipeline->ring_buf);
    __atomic_store_n(&pipeline->buffering, true, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->active, true, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_audio_session_mux);
}

void audio_pipeline_prepare_session(audio_pipeline_t *pipeline, uint8_t session_id) {
    if (!pipeline) return;
    portENTER_CRITICAL(&s_audio_session_mux);
    pipeline->session_epoch = next_session_epoch(pipeline->session_epoch);
    pipeline->reset_epoch = pipeline->session_epoch;
    pipeline->reset_session_id = session_id;
    pipeline->reset_pending = true;
    pipeline->prepared_start = true;
    // Invalidate any frame already decoding in the previous session. The BLE
    // callback and decoder share an owner task, and the epoch check also rejects
    // a stale frame at its eventual ring-head commit.
    __atomic_store_n(&pipeline->active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->buffering, true, __ATOMIC_RELEASE);
    audio_ring_buffer_clear(&pipeline->ring_buf);
    portEXIT_CRITICAL(&s_audio_session_mux);
    audio_pipeline_apply_session_reset(pipeline, NULL);
}

void audio_pipeline_sync(audio_pipeline_t *pipeline, int16_t predictor, int8_t step_index) {
    if (!pipeline) return;
    audio_pipeline_apply_session_reset(pipeline, NULL);
    adpcm_sync_state(&pipeline->adpcm, predictor, step_index);
    // The predictor reset makes the next decoded sample independent of the last
    // one, so the upsampler must re-prime rather than interpolate across that
    // discontinuity (which would emit a large spurious step, i.e. a click).
    pipeline->rs_primed = false;
}

// Linear-interpolating rational resampler, ratio = rs_interval / rs_step.
//
// The current field observation is 240 decoded samples per frame at roughly
// 66.7 frames/s, which matches AUDIO_SAMPLE_RATE=16000 and the default 1:1 path.
// The ratio remains runtime-tunable for measurement, but the spectral probe is
// diagnostic evidence rather than proof of a fixed missing-sample fraction.
//
//   rs_interval  units per input-sample interval (the interpolation denominator)
//   rs_step      read-position advance per output sample
//   ratio        outputs per input = rs_interval / rs_step
//
// Traced with interval 4, step 3 (ratio 4/3), phase 0, prev = in[0]:
//   emit in[0]                          (phase 0)
//   emit in[0] + (in[1]-in[0])*3/4      (phase 3, then 6 crosses the interval)
//   emit in[1] + (in[2]-in[1])*2/4      (phase 2, then 5 crosses)
//   emit in[2] + (in[3]-in[2])*1/4      (phase 1, then 4 crosses)
//   emit in[3]                          (phase 0 again)
// which is 4 outputs per 3 inputs, exactly, with no accumulated drift.
//
// Phase lives in integer units, so a ratio request takes effect at the next
// session boundary; no fractional error builds up over frames or sessions.
static size_t audio_resample_up(audio_pipeline_t *pipeline,
                                const int16_t *in, size_t n, int16_t *out) {
    if (pipeline->rs_step == 0 || pipeline->rs_interval == 0) {
        // Ratio disabled: pass through unchanged.
        if (n > AUDIO_WORK_SAMPLES) n = AUDIO_WORK_SAMPLES;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }

    if (!pipeline->rs_primed) {
        if (n == 0) return 0;
        pipeline->rs_prev   = in[0];
        pipeline->rs_phase  = 0;
        pipeline->rs_primed = true;
        in++;
        n--;
    }

    const uint32_t interval = pipeline->rs_interval;
    const uint32_t step     = pipeline->rs_step;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t cur = in[i];
        while (pipeline->rs_phase < interval) {
            // Worst case per input sample is ceil(interval/step)+1 outputs, and
            // callers cap the ratio so that this stays inside AUDIO_WORK_SAMPLES.
            if (o >= AUDIO_WORK_SAMPLES) { pipeline->rs_primed = false; return o; }
            // The phase is derived from uint16 ratios and can approach 65535;
            // full-scale PCM deltas times phase exceed signed 32-bit range.
            const int64_t d = (int64_t)(cur - pipeline->rs_prev) * pipeline->rs_phase;
            out[o++] = (int16_t)(pipeline->rs_prev + d / (int64_t)interval);
            pipeline->rs_phase += step;
        }
        pipeline->rs_phase -= interval;
        pipeline->rs_prev = (int16_t)cur;
    }
    return o;
}

// Ratio is (outputs per input) = num / den. Values outside [AUDIO_RS_MIN_RATIO_PCT,
// AUDIO_RS_MAX_RATIO_PCT] are rejected: too small starves the UAC consumer, and
// too large can exceed the work buffer.
bool audio_pipeline_set_resample(audio_pipeline_t *pipeline, uint16_t num, uint16_t den) {
    if (!pipeline) return false;
    if (!audio_rs_valid(num, den, AUDIO_RS_MIN_RATIO_PCT, AUDIO_RS_MAX_RATIO_PCT)) return false;
    // Reduce to lowest terms so the phase stays small and exact.
    const uint32_t gcd = audio_rs_gcd(num, den);
    const uint16_t reduced_num = (uint16_t)(num / gcd);
    const uint16_t reduced_den = (uint16_t)(den / gcd);
    // Success means the request passed validation and was atomically queued.
    // The active ratio and phase are untouched until the BLE owner applies it
    // at a session boundary.
    __atomic_store_n(&pipeline->rs_requested_packed,
                     audio_rs_pack(reduced_num, reduced_den), __ATOMIC_RELEASE);
    return true;
}

void audio_pipeline_get_resample(const audio_pipeline_t *pipeline, uint16_t *num, uint16_t *den) {
    if (!pipeline || !num || !den) return;
    const uint32_t requested = __atomic_load_n(&pipeline->rs_requested_packed, __ATOMIC_ACQUIRE);
    *num = audio_rs_num(requested);
    *den = audio_rs_den(requested);
}

void audio_pipeline_get_resample_status(const audio_pipeline_t *pipeline, audio_resample_status_t *out) {
    if (!pipeline || !out) return;
    const uint32_t requested = __atomic_load_n(&pipeline->rs_requested_packed, __ATOMIC_ACQUIRE);
    const uint32_t applied = __atomic_load_n(&pipeline->rs_applied_packed, __ATOMIC_ACQUIRE);
    out->requested_num = audio_rs_num(requested);
    out->requested_den = audio_rs_den(requested);
    out->applied_num = audio_rs_num(applied);
    out->applied_den = audio_rs_den(applied);
    out->pending = audio_rs_is_pending(requested, applied) != 0;
}

static void audio_pipeline_apply_requested_ratio(audio_pipeline_t *pipeline) {
    const uint32_t requested = __atomic_load_n(&pipeline->rs_requested_packed, __ATOMIC_ACQUIRE);
    const uint32_t applied = __atomic_load_n(&pipeline->rs_applied_packed, __ATOMIC_RELAXED);
    if (requested == applied) return;
    const uint16_t num = audio_rs_num(requested);
    const uint16_t den = audio_rs_den(requested);
    // Setter validation guarantees these are non-zero and in range.
    pipeline->rs_interval = num;
    pipeline->rs_step = den;
    pipeline->rs_primed = false;
    __atomic_store_n(&pipeline->rs_applied_packed, requested, __ATOMIC_RELEASE);
}

bool audio_pipeline_is_active(const audio_pipeline_t *pipeline) {
    return pipeline && __atomic_load_n(&pipeline->active, __ATOMIC_ACQUIRE);
}

bool audio_pipeline_is_buffering(const audio_pipeline_t *pipeline) {
    return pipeline && __atomic_load_n(&pipeline->buffering, __ATOMIC_ACQUIRE);
}

void audio_pipeline_consume_pending_clear(audio_pipeline_t *pipeline) {
    if (!pipeline) return;
    portENTER_CRITICAL(&s_audio_session_mux);
    audio_ring_buffer_consume_pending_clear(&pipeline->ring_buf);
    portEXIT_CRITICAL(&s_audio_session_mux);
}

size_t audio_pipeline_feed_adpcm(audio_pipeline_t *pipeline, const uint8_t *adpcm_bytes, size_t len) {
    if (!pipeline || !adpcm_bytes || len == 0) return 0;
    uint32_t session_epoch = 0;
    if (!audio_pipeline_begin_produce(pipeline, &session_epoch)) return 0;

    // 0. ADPCM decode at the remote's native rate, then rate-convert to
    //    AUDIO_SAMPLE_RATE so everything below (rate-dependent filters,
    //    time-based mute/fade, the UAC consumer) runs at its design rate.
    const size_t decoded = adpcm_decode_frame(&pipeline->adpcm, adpcm_bytes, len, pipeline->decode_buf);
    if (decoded == 0) return 0;
    // The spectral probe is diagnostic-only, and at one Goertzel pass per frame it
    // added ~3 ms to a 15 ms budget inside the NimBLE host task, which throttles
    // notification handling. 8 bins over a whole utterance is plenty, so sample
    // one frame in AUDIO_SPEC_STRIDE; at about 66.7 fps that is roughly 2.7 probes/s.
    if ((pipeline->spec_frames++ % AUDIO_SPEC_STRIDE) == 0) {
        audio_spectrum_probe(pipeline, pipeline->decode_buf, decoded);
    }
    const size_t samples_decoded = audio_resample_up(pipeline, pipeline->decode_buf, decoded, pipeline->temp_pcm);
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
    // The zeroing is a memset-equivalent rather than a per-sample branch.
    if (pipeline->lead_mute_remaining >= samples_decoded) {
        memset(pipeline->temp_pcm, 0, samples_decoded * sizeof(int16_t));
        pipeline->lead_mute_remaining -= samples_decoded;
    } else if (pipeline->lead_mute_remaining > 0) {
        memset(pipeline->temp_pcm, 0, pipeline->lead_mute_remaining * sizeof(int16_t));
        pipeline->lead_mute_remaining = 0;
    }
    // fade_in samples pass through AGC normally (they're real audio, just new)
    audio_agc_process(&pipeline->agc, pipeline->temp_pcm, samples_decoded);
    // 5. Post-Processing: Micro Fade-In (10ms) applied AFTER AGC
    // The mute already zeroed the first 30ms. This 10ms fade-in smooths the
    // hard boundary transition. It is too short (160 samples) to create audible
    // noise artifacts, but long enough to eliminate any hard step discontinuity.
    //
    // The ramp is a Q15 accumulator rather than `* step / AUDIO_FADE_IN_SAMPLES`
    // so there is no integer divide in the sample loop: `q15` walks 0 -> 32767
    // in fixed increments and the multiply is a shift. The previous form did
    // `sample * step >> 15` with step <= 160, which peaked at 160/32768 = 0.5%
    // gain and so attenuated the opening samples instead of fading them in.
    if (pipeline->fade_in_remaining > 0) {
        const uint32_t fade = pipeline->fade_in_remaining;
        const size_t n = (fade < samples_decoded) ? fade : samples_decoded;
        // Per-sample Q15 increment, rounded so the ramp reaches unity exactly.
        const int32_t fade_inc = (int32_t)((32768 + (AUDIO_FADE_IN_SAMPLES / 2)) /
                                           AUDIO_FADE_IN_SAMPLES);
        int32_t q15 = 0;
        for (size_t i = 0; i < n; i++) {
            q15 += fade_inc;
            pipeline->temp_pcm[i] = (int16_t)(((int32_t)pipeline->temp_pcm[i] * q15) >> 15);
        }
        pipeline->fade_in_remaining = fade - n;
    }

    // Copy into unpublished ring slots without a long critical section. The
    // final short session lock decides whether these samples belong to the
    // current epoch and publishes head atomically; stop/restart invalidates an
    // in-flight frame before it can become visible to USB.
    uint32_t expected_head = 0;
    const size_t prepared = audio_ring_buffer_prepare_write(
            &pipeline->ring_buf, pipeline->temp_pcm, samples_decoded, &expected_head);
    return audio_pipeline_commit_prepared_write(
            pipeline, session_epoch, expected_head, prepared, true);
}

size_t audio_pipeline_read_for_usb(audio_pipeline_t *pipeline, int16_t *out_pcm, size_t sample_count) {
    if (!pipeline || !out_pcm || sample_count == 0) return 0;

    // Bound every session critical section to a small USB-sized chunk, even if
    // another caller requests a much larger read than the usual 32 samples.
    static const size_t READ_LOCK_CHUNK_SAMPLES = 64;
    size_t offset = 0;
    size_t short_avail = 0;
    bool short_read = false;
    bool became_silent = false;
    while (offset < sample_count) {
        const size_t chunk = (sample_count - offset < READ_LOCK_CHUNK_SAMPLES)
            ? sample_count - offset : READ_LOCK_CHUNK_SAMPLES;
        size_t n = 0;
        portENTER_CRITICAL(&s_audio_session_mux);
        audio_ring_buffer_consume_pending_clear(&pipeline->ring_buf);
        const bool silent = !__atomic_load_n(&pipeline->active, __ATOMIC_ACQUIRE) ||
                            __atomic_load_n(&pipeline->buffering, __ATOMIC_ACQUIRE);
        if (!silent) {
            short_avail = audio_ring_buffer_available_read(&pipeline->ring_buf);
            const size_t wanted = short_avail < chunk ? short_avail : chunk;
            n = audio_ring_buffer_read(&pipeline->ring_buf, out_pcm + offset, wanted);
            __atomic_fetch_add(&pipeline->wire_samples, (uint32_t)n, __ATOMIC_RELAXED);
            if (n < chunk) {
                if (!short_read) {
                    if (short_avail == 0) __atomic_fetch_add(&pipeline->starve_count, 1u, __ATOMIC_RELAXED);
                    else __atomic_fetch_add(&pipeline->partial_count, 1u, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&pipeline->underrun_count, 1u, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&pipeline->padded_samples,
                                       (uint32_t)(sample_count - offset - n), __ATOMIC_RELAXED);
                }
                short_read = true;
            }
        } else {
            became_silent = true;
        }
        portEXIT_CRITICAL(&s_audio_session_mux);

        offset += n;
        if (became_silent) {
            memset(out_pcm + offset, 0, (sample_count - offset) * sizeof(int16_t));
            return sample_count;
        }
        if (n < chunk) break;
    }

    if (short_read) {
        // Surface underruns (starved ring) at most once every 5s so the web log
        // stays readable during long voice sessions instead of a 2s periodic beat.
        // Report the split so a real producer shortfall is distinguishable from
        // ordinary ring jitter: only starve_count means audio was actually lost.
        static uint32_t s_last_underrun_log_ms = 0;
        static uint32_t s_log_starve = 0, s_log_partial = 0, s_log_padded = 0, s_log_wire = 0;
        uint32_t now = millis();
        if (now - s_last_underrun_log_ms >= 5000) {
            s_last_underrun_log_ms = now;
            const uint32_t starve = __atomic_load_n(&pipeline->starve_count, __ATOMIC_RELAXED);
            const uint32_t partial = __atomic_load_n(&pipeline->partial_count, __ATOMIC_RELAXED);
            const uint32_t padded = __atomic_load_n(&pipeline->padded_samples, __ATOMIC_RELAXED);
            const uint32_t wire = __atomic_load_n(&pipeline->wire_samples, __ATOMIC_RELAXED);
            uint32_t d_starve  = starve - s_log_starve;
            uint32_t d_partial = partial - s_log_partial;
            uint32_t d_padded  = padded - s_log_padded;
            uint32_t d_wire    = wire - s_log_wire;
            s_log_starve = starve;
            s_log_partial = partial;
            s_log_padded = padded;
            s_log_wire = wire;
            uint32_t d_total = d_padded + d_wire;
            uint32_t loss_pct = d_total ? (d_padded * 100 / d_total) : 0;
            app_log("AUDIO_DBG",
                "ring short: avail=%d req=%d | 5s window: starve=%u partial=%u "
                "wire=%u padded=%u (%u%% silence) | frames=%u",
                (int)short_avail, (int)sample_count,
                (unsigned)d_starve, (unsigned)d_partial,
                (unsigned)d_wire, (unsigned)d_padded, (unsigned)loss_pct,
                (unsigned)__atomic_load_n(&pipeline->total_frames_decoded, __ATOMIC_RELAXED));
        }
        // Smooth hold / fade to 0 to prevent sharp sawtooth click
        int16_t last_val = (offset > 0) ? out_pcm[offset - 1] : 0;
        for (size_t i = offset; i < sample_count; i++) {
            last_val = (int16_t)((last_val * 7) / 8); // Smooth exponential fade to 0
            out_pcm[i] = last_val;
        }
    }

    return sample_count;
}

void audio_pipeline_stop_session(audio_pipeline_t *pipeline) {
    if (!pipeline) return;
    portENTER_CRITICAL(&s_audio_session_mux);
    __atomic_store_n(&pipeline->active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline->buffering, false, __ATOMIC_RELEASE);
    pipeline->session_epoch = next_session_epoch(pipeline->session_epoch);
    pipeline->reset_epoch = pipeline->session_epoch;
    pipeline->reset_pending = false;
    pipeline->prepared_start = false;
    audio_ring_buffer_clear(&pipeline->ring_buf);
    portEXIT_CRITICAL(&s_audio_session_mux);
}

// ---------------------------------------------------------------------------
// Spectral probe
// ---------------------------------------------------------------------------

// Goertzel energy estimate for one bin over `n` samples. Cheaper than an FFT and
// we only need a handful of fixed frequencies.
static uint32_t audio_goertzel(const int16_t *x, size_t n, uint32_t bin_hz, uint32_t rate) {
    const double k = 2.0 * M_PI * (double)bin_hz / (double)rate;
    const double c = 2.0 * cos(k);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return p > 0.0 ? (uint32_t)(p / (double)n) : 0u;
}

// Accumulate energy on the decoded sequence, before rate conversion. The current
// working interpretation is 240 samples/frame at about 66.7 fps (~16 kHz), but
// the probe is diagnostic and cannot by itself prove the source sampling rate.
static void audio_spectrum_probe(audio_pipeline_t *pipeline,
                                 const int16_t *x, size_t n) {
    for (int b = 0; b < AUDIO_SPEC_BINS; b++) {
        const uint32_t hz = AUDIO_SPEC_BIN0_HZ + (uint32_t)b * AUDIO_SPEC_BIN_HZ_STEP;
        const uint32_t m = audio_goertzel(x, n, hz, AUDIO_REMOTE_SAMPLE_RATE);
        pipeline->spec_mag[b] += m;
        if (m > pipeline->spec_peak[b]) pipeline->spec_peak[b] = m;
    }
}

void audio_pipeline_get_spectrum(const audio_pipeline_t *pipeline, uint16_t out[AUDIO_SPEC_BINS]) {
    if (!pipeline || !out) return;
    uint64_t mx = 1;
    for (int b = 0; b < AUDIO_SPEC_BINS; b++) {
        if ((uint64_t)pipeline->spec_peak[b] > mx) mx = pipeline->spec_peak[b];
    }
    // 64-bit intermediate: spec_peak is an accumulated energy and overflows a
    // 32-bit product with 1000 easily, which silently corrupted the whole plot.
    for (int b = 0; b < AUDIO_SPEC_BINS; b++) {
        out[b] = (uint16_t)(((uint64_t)pipeline->spec_peak[b] * 1000ull) / mx);
    }
}

void audio_pipeline_get_spectrum_raw(const audio_pipeline_t *pipeline, uint32_t out[AUDIO_SPEC_BINS]) {
    if (!pipeline || !out) return;
    for (int b = 0; b < AUDIO_SPEC_BINS; b++) out[b] = pipeline->spec_peak[b];
}
