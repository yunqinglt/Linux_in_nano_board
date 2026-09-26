#ifndef _USART_PROXY_H
#define _USART_PROXY_H

#include "usart_engine.h"
#include <stddef.h>
#include <stdint.h>

#define USART_PROXY_RECOVERY_NVIC_WAS_ENABLED (1UL << 16)
#define USART_PROXY_RECOVERY_NVIC_WAS_PENDING (1UL << 17)
#define USART_PROXY_RECOVERY_PRIORITY_CHANGED (1UL << 18)
#define USART_PROXY_RECOVERY_USART_CLOCK_GATE  (1UL << 19)
#define USART_PROXY_RECOVERY_USART_CLOCK_SOURCE (1UL << 20)
#define USART_PROXY_RECOVERY_GPIO_CLOCK_GATE   (1UL << 21)
#define USART_PROXY_RECOVERY_GPIO_CONFIG       (1UL << 22)
#define USART_PROXY_RECOVERY_USART_CONFIG      (1UL << 23)
#define USART_PROXY_RECOVERY_RX_DISCARDED      (1UL << 24)

typedef enum {
    PROXY_BLOCKING = 0,
    PROXY_NONBLOCKING
} ProxyMode_t;

typedef enum {
    USART_PROXY_STATUS_OK = 0,
    USART_PROXY_STATUS_RECOVERED,
    USART_PROXY_STATUS_INVALID_ARGUMENT,
    USART_PROXY_STATUS_INVALID_CONTEXT,
    USART_PROXY_STATUS_HW_NOT_CONFIGURED,
    USART_PROXY_STATUS_HW_TIMEOUT,
    USART_PROXY_STATUS_ENGINE_ERROR,
    USART_PROXY_STATUS_NOT_INITIALIZED
} UsartProxyStatus_t;

typedef struct {
    UsartEngine_t *p_engine;
    ProxyMode_t mode;
    UsartProxyStatus_t init_status;
    uint32_t recovery_flags;
    volatile uint32_t io_timeouts;
    uint8_t initialized;
} UsartProxy_t;

typedef struct {
    UsartProxyStatus_t init_status;
    uint32_t recovery_flags;
    uint32_t usart_isr;
    uint32_t usart_cr1;
    uint32_t usart_cr2;
    uint32_t usart_cr3;
    uint32_t usart_brr;
    uint32_t usart_presc;
    uint32_t peripheral_clock_hz;
    uint32_t baud_rate;
    uint32_t nvic_priority;
    uint8_t nvic_enabled;
    uint8_t nvic_pending;
    uint8_t global_irq_masked;
    uint8_t high_level_mode;
    uint8_t tx_busy;
    uint32_t io_timeouts;
    UsartEngineDiagnostics_t counters;
} UsartProxyDiagnostics_t;

/*
 * Stage-1/Post-Boot initialization for the USART2 singleton. This function
 * explicitly rebinds USART2, rebuilds both rings, repairs USART/NVIC state,
 * and enables USART2_IRQn only after all software state is valid.
 */
UsartProxyStatus_t UsartProxy_Init(UsartProxy_t *proxy,
                                   UsartEngine_t *engine,
                                   ProxyMode_t mode,
                                   uint8_t *rx_mem, size_t rx_size,
                                   uint8_t *tx_mem, size_t tx_size);

int UsartProxy_Getchar(UsartProxy_t *proxy);
int UsartProxy_TryGetchar(UsartProxy_t *proxy);
int UsartProxy_Putchar(UsartProxy_t *proxy, int ch);
int UsartProxy_Write(UsartProxy_t *proxy, const void *buf, size_t count);
int UsartProxy_Read(UsartProxy_t *proxy, void *buf, size_t count);
size_t UsartProxy_DiscardInput(UsartProxy_t *proxy);
UsartProxyStatus_t UsartProxy_Flush(UsartProxy_t *proxy,
                                    uint32_t timeout_ms);
UsartProxyStatus_t UsartProxy_GetDiagnostics(
    const UsartProxy_t *proxy, UsartProxyDiagnostics_t *diagnostics);

#endif
