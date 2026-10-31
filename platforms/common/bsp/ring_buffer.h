#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Fixed-capacity circular ring buffer using power-of-2 capacity for fast masking. */
typedef struct {
    uint8_t *buffer;
    uint32_t capacity; /* Must be power of 2 */
    uint32_t head;     /* Write counter */
    uint32_t tail;     /* Read counter */
} ring_buffer_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline void ring_buffer_init(ring_buffer_t *rb, uint8_t *storage, uint32_t capacity) {
    if (!rb) return;
    rb->buffer = storage;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
}

static inline void ring_buffer_reset(ring_buffer_t *rb) {
    if (!rb) return;
    rb->head = 0;
    rb->tail = 0;
}

static inline uint32_t ring_buffer_size(const ring_buffer_t *rb) {
    if (!rb) return 0;
    return (rb->head - rb->tail);
}

static inline uint32_t ring_buffer_capacity(const ring_buffer_t *rb) {
    if (!rb) return 0;
    return rb->capacity;
}

static inline bool ring_buffer_is_empty(const ring_buffer_t *rb) {
    if (!rb) return true;
    return rb->head == rb->tail;
}

static inline bool ring_buffer_is_full(const ring_buffer_t *rb) {
    if (!rb) return true;
    return (rb->head - rb->tail) >= rb->capacity;
}

static inline bool ring_buffer_put(ring_buffer_t *rb, uint8_t byte) {
    if (!rb || !rb->buffer || ring_buffer_is_full(rb)) {
        return false;
    }
    rb->buffer[rb->head & (rb->capacity - 1u)] = byte;
    rb->head++;
    return true;
}

static inline bool ring_buffer_get(ring_buffer_t *rb, uint8_t *out_byte) {
    if (!rb || !rb->buffer || ring_buffer_is_empty(rb)) {
        return false;
    }
    if (out_byte) {
        *out_byte = rb->buffer[rb->tail & (rb->capacity - 1u)];
    }
    rb->tail++;
    return true;
}

static inline bool ring_buffer_peek(const ring_buffer_t *rb, uint8_t *out_byte) {
    if (!rb || !rb->buffer || ring_buffer_is_empty(rb)) {
        return false;
    }
    if (out_byte) {
        *out_byte = rb->buffer[rb->tail & (rb->capacity - 1u)];
    }
    return true;
}

#ifdef __cplusplus
}
#endif
