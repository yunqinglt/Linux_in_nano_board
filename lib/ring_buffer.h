#ifndef CH32_RING_BUFFER_H
#define CH32_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RingBuffer RingBuffer_t;

typedef struct {
    uint8_t (*push)(RingBuffer_t *buffer, uint8_t value);
    uint8_t (*pop)(RingBuffer_t *buffer, uint8_t *value);
    uint8_t (*is_empty)(const RingBuffer_t *buffer);
    size_t (*get_count)(const RingBuffer_t *buffer);
    void (*clear)(RingBuffer_t *buffer);
} RingBufferOps_t;

struct RingBuffer {
    uint8_t *p_buf;
    size_t capacity;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;
    const RingBufferOps_t *ops;
};

void RingBuffer_Init(RingBuffer_t *buffer, uint8_t *storage, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* CH32_RING_BUFFER_H */
