#include "audio_ring_buffer.h"
#include <string.h>

static bool is_power_of_two(size_t n) {
    return (n > 0) && ((n & (n - 1)) == 0);
}

bool audio_ring_buffer_init(audio_ring_buffer_t *rb, int16_t *storage, size_t capacity) {
    if (!rb || !storage || !is_power_of_two(capacity)) {
        return false;
    }
    rb->buffer = storage;
    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->head = 0;
    rb->tail = 0;
    rb->clear_pending = false;
    return true;
}

void audio_ring_buffer_consume_pending_clear(audio_ring_buffer_t *rb) {
    if (!rb) return;
    if (!rb->clear_pending) return;
    rb->clear_pending = false;
    __sync_synchronize();
    rb->tail = rb->head;
    __sync_synchronize();
}

size_t audio_ring_buffer_available_read(audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    // Apply a producer-posted clear here, on the consumer's own thread, before
    // anyone reads head/tail. Doing it here (rather than in read()) keeps the
    // reported availability consistent with what read() will actually return.
    audio_ring_buffer_consume_pending_clear(rb);
    return rb->head - rb->tail;
}

size_t audio_ring_buffer_peek_available(const audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->head - rb->tail;
}

size_t audio_ring_buffer_available_write(const audio_ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->capacity - (rb->head - rb->tail);
}

size_t audio_ring_buffer_write(audio_ring_buffer_t *rb, const int16_t *samples, size_t count) {
    if (!rb || !samples || count == 0) return 0;

    size_t free_slots = audio_ring_buffer_available_write(rb);
    size_t to_write = (count < free_slots) ? count : free_slots;
    size_t h = rb->head;

    for (size_t i = 0; i < to_write; i++) {
        rb->buffer[(h + i) & rb->mask] = samples[i];
    }
    __sync_synchronize();
    rb->head = h + to_write;

    return to_write;
}

size_t audio_ring_buffer_read(audio_ring_buffer_t *rb, int16_t *out_samples, size_t count) {
    if (!rb || !out_samples || count == 0) return 0;

    size_t avail = audio_ring_buffer_available_read(rb);
    size_t to_read = (count < avail) ? count : avail;
    size_t t = rb->tail;

    for (size_t i = 0; i < to_read; i++) {
        out_samples[i] = rb->buffer[(t + i) & rb->mask];
    }
    __sync_synchronize();
    rb->tail = t + to_read;

    return to_read;
}

void audio_ring_buffer_clear(audio_ring_buffer_t *rb) {
    if (!rb) return;
    // Producer-side reset. Never touch tail directly: this may run while the
    // consumer is mid-read, and its post-read "tail = t + to_read" would then
    // clobber the reset and rewind the cursor into the previous session's
    // samples. Post a request and let the consumer move its own cursor.
    rb->clear_pending = true;
    __sync_synchronize();
}
