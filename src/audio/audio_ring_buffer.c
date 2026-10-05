#include "audio_ring_buffer.h"

static bool is_power_of_two(size_t n) {
    return (n > 0) && ((n & (n - 1)) == 0);
}

bool audio_ring_buffer_init(audio_ring_buffer_t *rb, int16_t *storage, size_t capacity) {
    if (!rb || !storage || !is_power_of_two(capacity) || capacity > UINT32_MAX) return false;
    rb->buffer = storage;
    rb->capacity = capacity;
    rb->mask = capacity - 1;
    __atomic_store_n(&rb->head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->clear_watermark, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->clear_request_seq, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->clear_ack_seq, 0u, __ATOMIC_RELAXED);
    return true;
}

void audio_ring_buffer_consume_pending_clear(audio_ring_buffer_t *rb) {
    if (!rb) return;
    const uint32_t request = __atomic_load_n(&rb->clear_request_seq, __ATOMIC_ACQUIRE);
    const uint32_t ack = __atomic_load_n(&rb->clear_ack_seq, __ATOMIC_RELAXED);
    if (request == ack) return;

    const uint32_t watermark = __atomic_load_n(&rb->clear_watermark, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    // A concurrent read may already have advanced beyond the fixed watermark;
    // never rewind its consumer-owned cursor.
    if ((int32_t)(watermark - tail) > 0) {
        __atomic_store_n(&rb->tail, watermark, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&rb->clear_ack_seq, request, __ATOMIC_RELEASE);
}

size_t audio_ring_buffer_available_read(audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    audio_ring_buffer_consume_pending_clear(rb);
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    const uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    const size_t used = (size_t)(head - tail);
    return used <= rb->capacity ? used : rb->capacity;
}

size_t audio_ring_buffer_peek_available(const audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    const uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    const size_t used = (size_t)(head - tail);
    return used <= rb->capacity ? used : rb->capacity;
}

size_t audio_ring_buffer_available_write(const audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    const uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    const size_t used = (size_t)(head - tail);
    return used <= rb->capacity ? rb->capacity - used : 0;
}

size_t audio_ring_buffer_prepare_write(audio_ring_buffer_t *rb,
                                       const int16_t *samples,
                                       size_t count,
                                       uint32_t *expected_head) {
    if (!rb || !samples || !expected_head || count == 0) return 0;
    const uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    const size_t used = (size_t)(head - tail);
    if (used > rb->capacity) return 0;
    const size_t free_slots = rb->capacity - used;
    const size_t to_write = count < free_slots ? count : free_slots;
    for (size_t i = 0; i < to_write; ++i) {
        rb->buffer[(head + (uint32_t)i) & rb->mask] = samples[i];
    }
    *expected_head = head;
    return to_write;
}

bool audio_ring_buffer_commit_write(audio_ring_buffer_t *rb,
                                    uint32_t expected_head,
                                    size_t count) {
    if (!rb || count > rb->capacity) return false;
    if (__atomic_load_n(&rb->head, __ATOMIC_RELAXED) != expected_head) return false;
    __atomic_store_n(&rb->head, expected_head + (uint32_t)count, __ATOMIC_RELEASE);
    return true;
}

size_t audio_ring_buffer_write(audio_ring_buffer_t *rb, const int16_t *samples, size_t count) {
    uint32_t expected_head = 0;
    const size_t to_write = audio_ring_buffer_prepare_write(rb, samples, count, &expected_head);
    if (to_write == 0 || !audio_ring_buffer_commit_write(rb, expected_head, to_write)) return 0;
    return to_write;
}

size_t audio_ring_buffer_read(audio_ring_buffer_t *rb, int16_t *out_samples, size_t count) {
    if (!rb || !out_samples || count == 0) return 0;
    const size_t avail = audio_ring_buffer_available_read(rb);
    const size_t to_read = count < avail ? count : avail;
    const uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    for (size_t i = 0; i < to_read; ++i) {
        out_samples[i] = rb->buffer[(tail + (uint32_t)i) & rb->mask];
    }
    __atomic_store_n(&rb->tail, tail + (uint32_t)to_read, __ATOMIC_RELEASE);
    return to_read;
}

void audio_ring_buffer_clear(audio_ring_buffer_t *rb) {
    if (!rb) return;
    // The caller serializes clear requests with producer head commits. The
    // fixed watermark lets the consumer preserve any later committed samples.
    const uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&rb->clear_watermark, head, __ATOMIC_RELAXED);
    const uint32_t seq = __atomic_load_n(&rb->clear_request_seq, __ATOMIC_RELAXED) + 1u;
    __atomic_store_n(&rb->clear_request_seq, seq, __ATOMIC_RELEASE);
}
