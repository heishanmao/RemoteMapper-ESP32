#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t *buffer;
    size_t   capacity;      // Must be power of 2
    size_t   mask;          // capacity - 1
    uint32_t head;          // Published write index (producer-owned, release/acquire)
    uint32_t tail;          // Read index (consumer-owned, release/acquire)
    // A clear advances the consumer to a fixed producer watermark. New samples
    // committed after the request remain readable.
    uint32_t clear_watermark;
    uint32_t clear_request_seq;
    uint32_t clear_ack_seq;
} audio_ring_buffer_t;

/**
 * @brief Initialize ring buffer with allocated storage
 * @param rb Ring buffer pointer
 * @param storage Pre-allocated array of int16_t samples
 * @param capacity Number of samples (must be power of 2, e.g. 1024, 2048, 4096)
 * @return true if valid power of 2, false otherwise
 */
bool audio_ring_buffer_init(audio_ring_buffer_t *rb, int16_t *storage, size_t capacity);

/**
 * @brief Get available samples for reading
 * @note  Not const: applies a pending cross-core clear before reporting, so the
 *        caller never sees availability from a buffer it just asked to reset.
 *        Consumer-only -- calling it from another context races the read cursor.
 */
size_t audio_ring_buffer_available_read(audio_ring_buffer_t *rb);

/**
 * @brief Apply a producer-posted clear, if any. Consumer-only (it moves tail).
 *        Exposed separately so the consumer can honour a session reset even on
 *        the idle path, where it never reaches available_read().
 */
void audio_ring_buffer_consume_pending_clear(audio_ring_buffer_t *rb);

/**
 * @brief Side-effect-free availability snapshot for diagnostics (web/status).
 *        Safe to call from any context; never touches tail.
 */
size_t audio_ring_buffer_peek_available(const audio_ring_buffer_t *rb);

/**
 * @brief Get available free sample slots for writing
 */
size_t audio_ring_buffer_available_write(const audio_ring_buffer_t *rb);

/**
 * @brief Write samples into ring buffer
 * @return Number of samples actually written
 */
size_t audio_ring_buffer_write(audio_ring_buffer_t *rb, const int16_t *samples, size_t count);
// Copy into currently free slots without publishing head. The sole producer
// later commits this bounded write after validating its session epoch.
size_t audio_ring_buffer_prepare_write(audio_ring_buffer_t *rb,
                                       const int16_t *samples,
                                       size_t count,
                                       uint32_t *expected_head);
bool audio_ring_buffer_commit_write(audio_ring_buffer_t *rb,
                                    uint32_t expected_head,
                                    size_t count);

/**
 * @brief Read samples from ring buffer
 * @return Number of samples actually read
 */
size_t audio_ring_buffer_read(audio_ring_buffer_t *rb, int16_t *out_samples, size_t count);

/**
 * @brief Clear ring buffer contents
 * @note  Producer-side. Posts a request that the consumer applies on its next
 *        read; see clear_pending above.
 */
void audio_ring_buffer_clear(audio_ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif
