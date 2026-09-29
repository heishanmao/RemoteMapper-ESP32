#include "audio_pipeline.h"
#include <string.h>
#include <math.h>
#include <Arduino.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern void app_log(const char* tag, const char* format, ...);

audio_pipeline_t g_audio_pipeline;

bool audio_pipeline_set_resample(audio_pipeline_t *pipeline, uint16_t num, uint16_t den);
static void audio_spectrum_probe(audio_pipeline_t *pipeline, const int16_t *x, size_t n);

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
    // Passthrough by default: the spectral probe shows 1:1 is the true capture
    // rate, so pitch is already correct and the defect is dropped samples rather
    // than a rate mismatch. Applied here rather than in start_session so a runtime
    // /api/audio/resample setting survives across sessions.
    audio_pipeline_set_resample(pipeline, 1, 1);
}

void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id) {
    if (!pipeline) return;
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_reset(&pipeline->agc);
    audio_ring_buffer_clear(&pipeline->ring_buf);
    // Re-prime the upsampler so the first frame does not interpolate against a
    // sample from the previous session (which would be an audible click).
    // The ratio itself is deliberately left alone: it is a runtime setting.
    pipeline->rs_prev   = 0;
    pipeline->rs_phase  = 0;
    pipeline->rs_primed = false;
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
    // The predictor reset makes the next decoded sample independent of the last
    // one, so the upsampler must re-prime rather than interpolate across that
    // discontinuity (which would emit a large spurious step, i.e. a click).
    pipeline->rs_primed = false;
}

// Linear-interpolating rational resampler, ratio = rs_interval / rs_step.
//
// The remote delivers 120-byte ADPCM frames (240 samples) at a steady ~50 fps.
// AUDIO_SAMPLE_RATE is 16000, so at 1:1 the UAC consumer pulls 16000 samples/s
// while only 12000 arrive, and the pipeline covers the 25% shortfall with
// silence. Some amount of rate conversion is therefore mandatory; the only open
// question is the exact ratio, which is why it is runtime-tunable.
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
// Phase lives in integer units, so a ratio change only affects samples decoded
// after the change; no fractional error builds up over frames or sessions.
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
            const int32_t d = (cur - pipeline->rs_prev) * (int32_t)pipeline->rs_phase;
            out[o++] = (int16_t)(pipeline->rs_prev + d / (int32_t)interval);
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
    if (den == 0 || num == 0) return false;                 // 0/0 = passthrough
    if ((uint32_t)num * 100u > (uint32_t)den * AUDIO_RS_MAX_RATIO_PCT) return false;
    if ((uint32_t)num * 100u < (uint32_t)den * AUDIO_RS_MIN_RATIO_PCT) return false;
    // Reduce to lowest terms so the phase stays small and exact.
    uint32_t a = num, b = den;
    while (b) { uint32_t t = a % b; a = b; b = t; }
    // Ratio is outputs-per-input = rs_interval / rs_step, so the interpolation
    // denominator comes from `num` and the per-output advance from `den`.
    pipeline->rs_interval = num / a;
    pipeline->rs_step     = den / a;
    pipeline->rs_primed   = false;                          // re-prime on next frame
    return true;
}

void audio_pipeline_get_resample(const audio_pipeline_t *pipeline, uint16_t *num, uint16_t *den) {
    if (!pipeline || !num || !den) return;
    if (pipeline->rs_interval == 0 || pipeline->rs_step == 0) { *num = 1; *den = 1; return; }
    *num = (uint16_t)pipeline->rs_interval;                // outputs per input
    *den = (uint16_t)pipeline->rs_step;
}

size_t audio_pipeline_feed_adpcm(audio_pipeline_t *pipeline, const uint8_t *adpcm_bytes, size_t len) {
    if (!pipeline || !adpcm_bytes || len == 0 || !pipeline->active) return 0;

    // 0. ADPCM decode at the remote's native rate, then rate-convert to
    //    AUDIO_SAMPLE_RATE so everything below (rate-dependent filters,
    //    time-based mute/fade, the UAC consumer) runs at its design rate.
    const size_t decoded = adpcm_decode_frame(&pipeline->adpcm, adpcm_bytes, len, pipeline->decode_buf);
    if (decoded == 0) return 0;
    // The spectral probe is diagnostic-only, and at one Goertzel pass per frame it
    // added ~3 ms to a 15 ms budget inside the NimBLE host task, which throttles
    // notification handling. 8 bins over a whole utterance is plenty, so sample
    // one frame in AUDIO_SPEC_STRIDE; at 50-66 fps that is roughly 2-3 probes/s.
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

    // 6. Enqueue into ring buffer
    size_t written = audio_ring_buffer_write(&pipeline->ring_buf, pipeline->temp_pcm, samples_decoded);

    pipeline->total_frames_decoded++;
    pipeline->total_samples_pushed += written;

    // Check if prefill threshold reached. Producer side, so use the pure peek:
    // the applying accessor moves the consumer's tail, and doing that from the
    // producer would discard the prefill this very loop is building.
    if (pipeline->buffering) {
        if (audio_ring_buffer_peek_available(&pipeline->ring_buf) >= AUDIO_JITTER_PREFILL_SAMPLES) {
            pipeline->buffering = false;
        }
    }

    return written;
}

size_t audio_pipeline_read_for_usb(audio_pipeline_t *pipeline, int16_t *out_pcm, size_t sample_count) {
    if (!pipeline || !out_pcm || sample_count == 0) return 0;

    // Honour a session reset on the idle path too. start_session/stop_session
    // run on the producer side and only post the request; without this the
    // previous session's samples would sit in the ring for as long as the host
    // keeps the stream muted, because the early return below never reaches the
    // ring at all.
    audio_ring_buffer_consume_pending_clear(&pipeline->ring_buf);

    if (!pipeline->active || pipeline->buffering) {
// Feed pure silence while idle or building the initial prefill cushion
// (AUDIO_JITTER_PREFILL_SAMPLES = 50ms).
        memset(out_pcm, 0, sample_count * sizeof(int16_t));
        return sample_count;
    }

    size_t avail = audio_ring_buffer_available_read(&pipeline->ring_buf);

    if (avail < sample_count) {
        if (avail == 0) pipeline->starve_count++;
        else            pipeline->partial_count++;
        pipeline->underrun_count++;
        size_t n = audio_ring_buffer_read(&pipeline->ring_buf, out_pcm, avail);
        pipeline->wire_samples    += n;
        pipeline->padded_samples  += (sample_count - n);
        // Surface underruns (starved ring) at most once every 5s so the web log
        // stays readable during long voice sessions instead of a 2s periodic beat.
        // Report the split so a real producer shortfall is distinguishable from
        // ordinary ring jitter: only starve_count means audio was actually lost.
        static uint32_t s_last_underrun_log_ms = 0;
        static uint32_t s_log_starve = 0, s_log_partial = 0, s_log_padded = 0, s_log_wire = 0;
        uint32_t now = millis();
        if (now - s_last_underrun_log_ms >= 5000) {
            s_last_underrun_log_ms = now;
            uint32_t d_starve  = pipeline->starve_count  - s_log_starve;
            uint32_t d_partial = pipeline->partial_count - s_log_partial;
            uint32_t d_padded  = pipeline->padded_samples - s_log_padded;
            uint32_t d_wire    = pipeline->wire_samples   - s_log_wire;
            s_log_starve = pipeline->starve_count;
            s_log_partial = pipeline->partial_count;
            s_log_padded = pipeline->padded_samples;
            s_log_wire = pipeline->wire_samples;
            uint32_t d_total = d_padded + d_wire;
            uint32_t loss_pct = d_total ? (d_padded * 100 / d_total) : 0;
            app_log("AUDIO_DBG",
                "ring short: avail=%d req=%d | 5s window: starve=%u partial=%u "
                "wire=%u padded=%u (%u%% silence) | frames=%u",
                (int)avail, (int)sample_count,
                (unsigned)d_starve, (unsigned)d_partial,
                (unsigned)d_wire, (unsigned)d_padded, (unsigned)loss_pct,
                (unsigned)pipeline->total_frames_decoded);
        }
        // Smooth hold / fade to 0 to prevent sharp sawtooth click
        int16_t last_val = (n > 0) ? out_pcm[n - 1] : 0;
        for (size_t i = n; i < sample_count; i++) {
            last_val = (int16_t)((last_val * 7) / 8); // Smooth exponential fade to 0
            out_pcm[i] = last_val;
        }
        return sample_count;
    }

    pipeline->wire_samples += sample_count;
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

// Accumulate energy on the decoded sequence, before rate conversion, so the bins
// are referenced to the remote's own sample spacing (1/12000 s). That is what
// makes the 6 kHz bin meaningful: it sits exactly on the assumed Nyquist.
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