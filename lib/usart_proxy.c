#include "usart_proxy.h"
#include "ch32_irq.h"

#include <limits.h>

#define USART_PROXY_BLOCK_TIMEOUT_MS 100U
#define USART_PROXY_DEFAULT_BAUD_RATE 115200U

static uint32_t usart_proxy_enter_critical(void)
{
    return Ch32Irq_SaveAndDisable();
}

static void usart_proxy_exit_critical(uint32_t interrupt_state)
{
    Ch32Irq_Restore(interrupt_state);
}

static uint8_t usart_proxy_is_ready(const UsartProxy_t *proxy)
{
    return (uint8_t)(proxy != NULL && proxy->initialized != 0U &&
                     proxy->p_engine != NULL &&
                     proxy->p_engine->high_level_mode != 0U);
}

static uint32_t usart_proxy_spin_budget(uint32_t timeout_ms)
{
    uint32_t clock_hz = SystemCoreClock;
    uint32_t cycles_per_ms;

    if (clock_hz == 0U) {
        clock_hz = 8000000UL;
    }
    cycles_per_ms = clock_hz / 1000U;
    if (cycles_per_ms == 0U) {
        cycles_per_ms = 1U;
    }
    if (timeout_ms > UINT32_MAX / cycles_per_ms) {
        return UINT32_MAX;
    }
    return timeout_ms * cycles_per_ms;
}

static uint8_t usart_proxy_expired(uint32_t *spins_left)
{
    if (*spins_left == 0U) {
        return 1U;
    }
    --(*spins_left);
    return 0U;
}

static UsartProxyStatus_t usart_proxy_map_engine_status(
    UsartEngineStatus_t status)
{
    switch (status) {
    case USART_ENGINE_STATUS_OK:
        return USART_PROXY_STATUS_OK;
    case USART_ENGINE_STATUS_RECOVERED:
        return USART_PROXY_STATUS_RECOVERED;
    case USART_ENGINE_STATUS_INVALID_ARGUMENT:
        return USART_PROXY_STATUS_INVALID_ARGUMENT;
    case USART_ENGINE_STATUS_HW_NOT_CONFIGURED:
        return USART_PROXY_STATUS_HW_NOT_CONFIGURED;
    case USART_ENGINE_STATUS_HW_TIMEOUT:
        return USART_PROXY_STATUS_HW_TIMEOUT;
    default:
        return USART_PROXY_STATUS_ENGINE_ERROR;
    }
}

UsartProxyStatus_t UsartProxy_Init(UsartProxy_t *proxy,
                                   UsartEngine_t *engine,
                                   ProxyMode_t mode,
                                   uint8_t *rx_mem, size_t rx_size,
                                   uint8_t *tx_mem, size_t tx_size)
{
    UsartProxyStatus_t status;
    uint32_t interrupt_state;

    if (proxy == NULL || engine == NULL || rx_mem == NULL || tx_mem == NULL ||
        rx_size == 0U || tx_size == 0U ||
        (mode != PROXY_BLOCKING && mode != PROXY_NONBLOCKING)) {
        return USART_PROXY_STATUS_INVALID_ARGUMENT;
    }

    proxy->p_engine = engine;
    proxy->mode = mode;
    proxy->init_status = USART_PROXY_STATUS_NOT_INITIALIZED;
    proxy->recovery_flags = 0U;
    proxy->io_timeouts = 0U;
    proxy->initialized = 0U;

    interrupt_state = usart_proxy_enter_critical();
    NVIC_DisableIRQ(USART_ENGINE_DEFAULT_IRQ);
    NVIC_ClearPendingIRQ(USART_ENGINE_DEFAULT_IRQ);

    status = usart_proxy_map_engine_status(
        UsartEngine_HL_Init(engine, USART_ENGINE_DEFAULT_USART,
                            rx_mem, rx_size, tx_mem, tx_size));
    proxy->recovery_flags = engine->last_recovery_flags;
    if (status == USART_PROXY_STATUS_OK ||
        status == USART_PROXY_STATUS_RECOVERED) {
        proxy->initialized = 1U;
        proxy->init_status = status;
        NVIC_ClearPendingIRQ(USART_ENGINE_DEFAULT_IRQ);
        NVIC_EnableIRQ(USART_ENGINE_DEFAULT_IRQ);
    } else {
        proxy->init_status = status;
    }
    usart_proxy_exit_critical(interrupt_state);
    return status;
}

int UsartProxy_TryGetchar(UsartProxy_t *proxy)
{
    uint8_t data;
    uint32_t interrupt_state;

    if (!usart_proxy_is_ready(proxy)) {
        return -1;
    }
    interrupt_state = usart_proxy_enter_critical();
    if (proxy->p_engine->rx.ops->pop(&proxy->p_engine->rx, &data) == 0U) {
        usart_proxy_exit_critical(interrupt_state);
        return -1;
    }
    usart_proxy_exit_critical(interrupt_state);
    return (int)data;
}

int UsartProxy_Getchar(UsartProxy_t *proxy)
{
    uint32_t spins_left;

    if (!usart_proxy_is_ready(proxy)) {
        return -1;
    }
    spins_left = usart_proxy_spin_budget(USART_PROXY_BLOCK_TIMEOUT_MS);
    for (;;) {
        const int character = UsartProxy_TryGetchar(proxy);
        if (character >= 0) {
            return character;
        }
        if (proxy->mode == PROXY_NONBLOCKING ||
            usart_proxy_expired(&spins_left)) {
            if (proxy->mode == PROXY_BLOCKING) {
                proxy->io_timeouts++;
            }
            return -1;
        }
        __NOP();
    }
}

int UsartProxy_Putchar(UsartProxy_t *proxy, int ch)
{
    uint32_t spins_left;

    if (!usart_proxy_is_ready(proxy)) {
        return -1;
    }
    spins_left = usart_proxy_spin_budget(USART_PROXY_BLOCK_TIMEOUT_MS);
    for (;;) {
        uint32_t interrupt_state = usart_proxy_enter_critical();
        const uint8_t pushed = proxy->p_engine->tx.ops->push(
            &proxy->p_engine->tx, (uint8_t)ch);
        if (pushed != 0U) {
            UsartEngine_HL_TriggerTx(proxy->p_engine);
        }
        usart_proxy_exit_critical(interrupt_state);

        if (pushed != 0U) {
            return (uint8_t)ch;
        }
        if (proxy->mode == PROXY_NONBLOCKING ||
            usart_proxy_expired(&spins_left)) {
            if (proxy->mode == PROXY_BLOCKING) {
                proxy->io_timeouts++;
            }
            return -1;
        }
        __NOP();
    }
}

int UsartProxy_Write(UsartProxy_t *proxy, const void *buf, size_t count)
{
    const uint8_t *source = (const uint8_t *)buf;
    size_t written = 0U;

    if (buf == NULL && count != 0U) {
        return -1;
    }
    while (written < count &&
           UsartProxy_Putchar(proxy, source[written]) >= 0) {
        ++written;
    }
    return (int)written;
}

int UsartProxy_Read(UsartProxy_t *proxy, void *buf, size_t count)
{
    uint8_t *destination = (uint8_t *)buf;
    size_t read_count = 0U;

    if (buf == NULL && count != 0U) {
        return -1;
    }
    while (read_count < count) {
        const int character = UsartProxy_Getchar(proxy);
        if (character < 0) {
            break;
        }
        destination[read_count++] = (uint8_t)character;
    }
    return (int)read_count;
}

size_t UsartProxy_DiscardInput(UsartProxy_t *proxy)
{
    size_t discarded;
    uint32_t interrupt_state;

    if (!usart_proxy_is_ready(proxy)) {
        return 0U;
    }
    interrupt_state = usart_proxy_enter_critical();
    discarded = proxy->p_engine->rx.ops->get_count(&proxy->p_engine->rx);
    proxy->p_engine->rx.ops->clear(&proxy->p_engine->rx);
    usart_proxy_exit_critical(interrupt_state);
    return discarded;
}

UsartProxyStatus_t UsartProxy_Flush(UsartProxy_t *proxy, uint32_t timeout_ms)
{
    uint32_t spins_left;

    if (!usart_proxy_is_ready(proxy)) {
        return USART_PROXY_STATUS_NOT_INITIALIZED;
    }
    spins_left = usart_proxy_spin_budget(timeout_ms);
    for (;;) {
        uint8_t empty;
        uint32_t interrupt_state = usart_proxy_enter_critical();
        empty = proxy->p_engine->tx.ops->is_empty(&proxy->p_engine->tx);
        usart_proxy_exit_critical(interrupt_state);
        if (empty != 0U &&
            USART_GetFlagStatus(USART_ENGINE_DEFAULT_USART,
                                USART_FLAG_TC) == SET) {
            return proxy->init_status;
        }
        if (usart_proxy_expired(&spins_left)) {
            proxy->io_timeouts++;
            return USART_PROXY_STATUS_HW_TIMEOUT;
        }
        __NOP();
    }
}

UsartProxyStatus_t UsartProxy_GetDiagnostics(
    const UsartProxy_t *proxy, UsartProxyDiagnostics_t *diagnostics)
{
    uint32_t interrupt_state;

    if (proxy == NULL || diagnostics == NULL || proxy->p_engine == NULL) {
        return USART_PROXY_STATUS_INVALID_ARGUMENT;
    }
    interrupt_state = usart_proxy_enter_critical();
    diagnostics->init_status = proxy->init_status;
    diagnostics->recovery_flags = proxy->recovery_flags;
    diagnostics->usart_isr = USART_ENGINE_DEFAULT_USART->STATR;
    diagnostics->usart_cr1 = USART_ENGINE_DEFAULT_USART->CTLR1;
    diagnostics->usart_cr2 = USART_ENGINE_DEFAULT_USART->CTLR2;
    diagnostics->usart_cr3 = USART_ENGINE_DEFAULT_USART->CTLR3;
    diagnostics->usart_brr = USART_ENGINE_DEFAULT_USART->BRR;
    diagnostics->usart_presc = 1U;
    diagnostics->peripheral_clock_hz = SystemCoreClock;
    diagnostics->baud_rate = USART_PROXY_DEFAULT_BAUD_RATE;
    diagnostics->nvic_priority = 0U;
    diagnostics->nvic_enabled = 1U;
    diagnostics->nvic_pending = 0U;
    diagnostics->global_irq_masked =
        (uint8_t)((interrupt_state & CH32_IRQ_GLOBAL_ENABLE_MASK) == 0u);
    diagnostics->high_level_mode = proxy->p_engine->high_level_mode;
    diagnostics->tx_busy = proxy->p_engine->tx_busy;
    diagnostics->io_timeouts = proxy->io_timeouts;
    UsartEngine_GetDiagnostics(proxy->p_engine, &diagnostics->counters);
    usart_proxy_exit_critical(interrupt_state);

    return proxy->initialized != 0U ? proxy->init_status
                                    : USART_PROXY_STATUS_NOT_INITIALIZED;
}
