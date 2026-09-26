#include "ring_buffer.h"

static uint8_t ring_buffer_push(RingBuffer_t *buffer, uint8_t value)
{
    if (buffer == NULL || buffer->p_buf == NULL || buffer->capacity == 0u ||
        buffer->count >= buffer->capacity) {
        return 0u;
    }

    buffer->p_buf[buffer->head] = value;
    buffer->head++;
    if (buffer->head == buffer->capacity) {
        buffer->head = 0u;
    }
    buffer->count++;
    return 1u;
}

static uint8_t ring_buffer_pop(RingBuffer_t *buffer, uint8_t *value)
{
    if (buffer == NULL || value == NULL || buffer->p_buf == NULL ||
        buffer->capacity == 0u || buffer->count == 0u) {
        return 0u;
    }

    *value = buffer->p_buf[buffer->tail];
    buffer->tail++;
    if (buffer->tail == buffer->capacity) {
        buffer->tail = 0u;
    }
    buffer->count--;
    return 1u;
}

static uint8_t ring_buffer_is_empty(const RingBuffer_t *buffer)
{
    return (uint8_t)(buffer == NULL || buffer->count == 0u);
}

static size_t ring_buffer_get_count(const RingBuffer_t *buffer)
{
    return buffer != NULL ? buffer->count : 0u;
}

static void ring_buffer_clear(RingBuffer_t *buffer)
{
    if (buffer != NULL) {
        buffer->head = 0u;
        buffer->tail = 0u;
        buffer->count = 0u;
    }
}

static const RingBufferOps_t g_ring_buffer_ops = {
    ring_buffer_push,
    ring_buffer_pop,
    ring_buffer_is_empty,
    ring_buffer_get_count,
    ring_buffer_clear
};

void RingBuffer_Init(RingBuffer_t *buffer, uint8_t *storage, size_t capacity)
{
    if (buffer == NULL) {
        return;
    }

    buffer->p_buf = storage;
    buffer->capacity = capacity;
    buffer->head = 0u;
    buffer->tail = 0u;
    buffer->count = 0u;
    buffer->ops = &g_ring_buffer_ops;
}
