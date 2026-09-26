#ifndef CH32_USART_ENGINE_H
#define CH32_USART_ENGINE_H

#include "ch32v20x_usart.h"
#include "ring_buffer.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USART_ENGINE_STATE_MAGIC 0x55534152UL /* "USAR" */
#define USART_ENGINE_ABI_VERSION 3U
#define USART_ENGINE_ISR_RX_BUDGET 16UL

/* The board routes the command console to USART2 (PA2/PA3). */
#define USART_ENGINE_DEFAULT_USART USART2
#define USART_ENGINE_DEFAULT_IRQ USART2_IRQn

#define USART_ENGINE_RECOVERY_STATE_INVALID       (1UL << 0)
#define USART_ENGINE_RECOVERY_REBOUND             (1UL << 1)
#define USART_ENGINE_RECOVERY_STALE_OPERATION     (1UL << 2)
#define USART_ENGINE_RECOVERY_IRQ_SOURCES_ENABLED (1UL << 3)
#define USART_ENGINE_RECOVERY_HW_DISABLED         (1UL << 4)
#define USART_ENGINE_RECOVERY_HW_ERROR            (1UL << 5)
#define USART_ENGINE_RECOVERY_PENDING_RX          (1UL << 6)

typedef enum {
    USART_ENGINE_STATUS_OK = 0,
    USART_ENGINE_STATUS_RECOVERED,
    USART_ENGINE_STATUS_INVALID_ARGUMENT,
    USART_ENGINE_STATUS_HW_NOT_CONFIGURED,
    USART_ENGINE_STATUS_HW_TIMEOUT,
    USART_ENGINE_STATUS_NOT_READY
} UsartEngineStatus_t;

typedef struct {
    uint32_t init_count;
    uint32_t recovery_count;
    uint32_t irq_count;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;
    uint32_t rx_error_dropped;
    uint32_t rx_faults;
    uint32_t parity_errors;
    uint32_t framing_errors;
    uint32_t noise_errors;
    uint32_t overrun_errors;
    uint32_t ll_rx_timeouts;
    uint32_t ll_tx_timeouts;
} UsartEngineDiagnostics_t;

typedef struct {
    uint32_t discarded_bytes;
    uint32_t error_discarded_bytes;
    uint32_t rx_faults;
    uint32_t parity_errors;
    uint32_t framing_errors;
    uint32_t noise_errors;
    uint32_t overrun_errors;
} UsartEngineRxDiscard_t;

typedef struct {
    uint32_t state_magic;
    uint16_t abi_version;
    uint16_t struct_size;
    USART_TypeDef *usart;
    RingBuffer_t tx;
    RingBuffer_t rx;
    volatile uint8_t tx_busy;
    volatile uint8_t high_level_mode;
    uint16_t reserved;
    volatile uint32_t last_recovery_flags;
    volatile UsartEngineDiagnostics_t diagnostics;
} UsartEngine_t;

UsartEngine_t *UsartEngine_GetInstance(void);

UsartEngineStatus_t UsartEngine_LL_Init(UsartEngine_t *self,
                                        USART_TypeDef *p_usart);
uint8_t UsartEngine_LL_SendChar(UsartEngine_t *self, uint8_t data,
                                uint32_t timeout_ms);
uint8_t UsartEngine_LL_RecvChar(UsartEngine_t *self, uint8_t *data,
                                uint32_t timeout_ms);
uint8_t UsartEngine_LL_TryRecvChar(UsartEngine_t *self, uint8_t *data);
uint8_t UsartEngine_LL_Flush(UsartEngine_t *self, uint32_t timeout_ms);
void UsartEngine_LL_SendString(UsartEngine_t *self, const char *str);

/* Rebuild the rings, bind the singleton to p_usart, and enable USART IRQs. */
UsartEngineStatus_t UsartEngine_HL_Init(UsartEngine_t *self,
                                        USART_TypeDef *p_usart,
                                        uint8_t *rx_mem, size_t rx_size,
                                        uint8_t *tx_mem, size_t tx_size);

/* Compatibility entry point retained from the original command-line model. */
void UsartEngine_HL_EnableInterrupts(UsartEngine_t *self,
                                      uint8_t *rx_mem, size_t rx_size,
                                      uint8_t *tx_mem, size_t tx_size);

void UsartEngine_HL_TriggerTx(UsartEngine_t *self);
void UsartEngine_HL_IRQHandler(UsartEngine_t *self);
void UsartEngine_RecordDiscardedInput(UsartEngine_t *self,
                                       const UsartEngineRxDiscard_t *discarded);
void UsartEngine_GetDiagnostics(const UsartEngine_t *self,
                                UsartEngineDiagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif /* CH32_USART_ENGINE_H */
