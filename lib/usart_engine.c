#include "usart_engine.h"
#include "ch32_irq.h"

#include <limits.h>

#define USART_ENGINE_ERROR_FLAGS \
    (USART_FLAG_PE | USART_FLAG_FE | USART_FLAG_NE | USART_FLAG_ORE)
#define USART_ENGINE_DATA_ERROR_FLAGS \
    (USART_FLAG_PE | USART_FLAG_FE | USART_FLAG_NE)
#define USART_ENGINE_DEFAULT_CLOCK_HZ 8000000UL

typedef struct {
    uint32_t spins_left;
} usart_wait_budget_t;

static UsartEngine_t g_usart_engine;

static void usart_engine_clear_diagnostics(UsartEngineDiagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->init_count = 0U;
    diagnostics->recovery_count = 0U;
    diagnostics->irq_count = 0U;
    diagnostics->rx_bytes = 0U;
    diagnostics->tx_bytes = 0U;
    diagnostics->rx_dropped = 0U;
    diagnostics->rx_error_dropped = 0U;
    diagnostics->rx_faults = 0U;
    diagnostics->parity_errors = 0U;
    diagnostics->framing_errors = 0U;
    diagnostics->noise_errors = 0U;
    diagnostics->overrun_errors = 0U;
    diagnostics->ll_rx_timeouts = 0U;
    diagnostics->ll_tx_timeouts = 0U;
}

static uint8_t usart_engine_state_is_valid(const UsartEngine_t *self)
{
    return (uint8_t)(self != NULL &&
                     self->state_magic == USART_ENGINE_STATE_MAGIC &&
                     self->abi_version == USART_ENGINE_ABI_VERSION &&
                     self->struct_size == (uint16_t)sizeof(*self));
}

static void usart_engine_reset_state(UsartEngine_t *self,
                                     USART_TypeDef *p_usart)
{
    self->state_magic = 0U;
    self->abi_version = USART_ENGINE_ABI_VERSION;
    self->struct_size = (uint16_t)sizeof(*self);
    self->usart = p_usart;
    self->tx.p_buf = NULL;
    self->tx.capacity = 0U;
    self->tx.head = 0U;
    self->tx.tail = 0U;
    self->tx.count = 0U;
    self->tx.ops = NULL;
    self->rx.p_buf = NULL;
    self->rx.capacity = 0U;
    self->rx.head = 0U;
    self->rx.tail = 0U;
    self->rx.count = 0U;
    self->rx.ops = NULL;
    self->tx_busy = 0U;
    self->high_level_mode = 0U;
    self->reserved = 0U;
    self->last_recovery_flags = 0U;
    usart_engine_clear_diagnostics((UsartEngineDiagnostics_t *)&self->diagnostics);
    __asm volatile("fence rw, rw" ::: "memory");
    self->state_magic = USART_ENGINE_STATE_MAGIC;
}

static void usart_engine_disable_interrupts(USART_TypeDef *usart)
{
    USART_ITConfig(usart, USART_IT_RXNE, DISABLE);
    USART_ITConfig(usart, USART_IT_TXE, DISABLE);
    USART_ITConfig(usart, USART_IT_ERR, DISABLE);
    USART_ITConfig(usart, USART_IT_PE, DISABLE);
}

static uint32_t usart_engine_spin_budget(uint32_t timeout_ms)
{
    uint32_t clock_hz = SystemCoreClock;
    uint32_t cycles_per_ms;

    if (clock_hz == 0U) {
        clock_hz = USART_ENGINE_DEFAULT_CLOCK_HZ;
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

static void usart_engine_wait_start(usart_wait_budget_t *budget,
                                    uint32_t timeout_ms)
{
    budget->spins_left = usart_engine_spin_budget(timeout_ms);
}

static uint8_t usart_engine_wait_expired(usart_wait_budget_t *budget)
{
    if (budget->spins_left == 0U) {
        return 1U;
    }
    --budget->spins_left;
    return 0U;
}

static void usart_engine_record_status_errors(UsartEngine_t *self,
                                              uint16_t status)
{
    if ((status & USART_ENGINE_ERROR_FLAGS) == 0U) {
        return;
    }
    self->diagnostics.rx_faults++;
    if ((status & USART_FLAG_PE) != 0U) {
        self->diagnostics.parity_errors++;
    }
    if ((status & USART_FLAG_FE) != 0U) {
        self->diagnostics.framing_errors++;
    }
    if ((status & USART_FLAG_NE) != 0U) {
        self->diagnostics.noise_errors++;
    }
    if ((status & USART_FLAG_ORE) != 0U) {
        self->diagnostics.overrun_errors++;
        self->diagnostics.rx_dropped++;
    }
}

static uint8_t usart_engine_hw_ready(const UsartEngine_t *self)
{
    if (!usart_engine_state_is_valid(self) || self->usart == NULL) {
        return 0U;
    }
    return (uint8_t)((self->usart->CTLR1 &
                      (USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE)) ==
                     (USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE));
}

UsartEngine_t *UsartEngine_GetInstance(void)
{
    return &g_usart_engine;
}

UsartEngineStatus_t UsartEngine_LL_Init(UsartEngine_t *self,
                                        USART_TypeDef *p_usart)
{
    uint16_t initial_status;
    uint32_t recovery_flags = 0U;

    if (self == NULL || p_usart == NULL) {
        return USART_ENGINE_STATUS_INVALID_ARGUMENT;
    }

    initial_status = p_usart->STATR;
    if ((p_usart->CTLR1 & (USART_CTLR1_RXNEIE | USART_CTLR1_TXEIE)) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_IRQ_SOURCES_ENABLED;
    }
    if ((p_usart->CTLR1 & (USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE)) !=
        (USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE)) {
        recovery_flags |= USART_ENGINE_RECOVERY_HW_DISABLED;
    }
    if ((initial_status & USART_ENGINE_ERROR_FLAGS) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_HW_ERROR;
    }
    if ((initial_status & USART_FLAG_RXNE) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_PENDING_RX;
    }

    usart_engine_disable_interrupts(p_usart);
    usart_engine_reset_state(self, p_usart);
    self->diagnostics.init_count = 1U;
    self->last_recovery_flags = recovery_flags;

    /* A CH32 USART clears PE/FE/NE/ORE (and RXNE) only after the
     * STATR-then-DATAR read sequence.  Drain both pending data and error
     * state left by the bootloader/debug monitor before publishing the
     * low-level instance. */
    {
        uint32_t drain_budget = USART_ENGINE_ISR_RX_BUDGET;
        while (drain_budget-- != 0U) {
            const uint16_t status = p_usart->STATR;

            if ((status & USART_ENGINE_ERROR_FLAGS) != 0U) {
                usart_engine_record_status_errors(self, status);
            }
            if ((status & (USART_FLAG_RXNE | USART_ENGINE_ERROR_FLAGS)) == 0U) {
                break;
            }
            (void)USART_ReceiveData(p_usart);
            if ((status & USART_FLAG_RXNE) != 0U) {
                self->diagnostics.rx_dropped++;
            }
        }
    }
    if (p_usart->BRR == 0U) {
        return USART_ENGINE_STATUS_HW_NOT_CONFIGURED;
    }

    p_usart->CTLR1 |= USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE;
    return recovery_flags != 0U ? USART_ENGINE_STATUS_RECOVERED
                                : USART_ENGINE_STATUS_OK;
}

uint8_t UsartEngine_LL_SendChar(UsartEngine_t *self, uint8_t data,
                                uint32_t timeout_ms)
{
    usart_wait_budget_t budget;

    if (!usart_engine_hw_ready(self)) {
        return 0U;
    }
    usart_engine_wait_start(&budget, timeout_ms);
    while (USART_GetFlagStatus(self->usart, USART_FLAG_TXE) == RESET) {
        if (usart_engine_wait_expired(&budget)) {
            if (timeout_ms != 0U) {
                self->diagnostics.ll_tx_timeouts++;
            }
            return 0U;
        }
    }
    USART_SendData(self->usart, data);
    self->diagnostics.tx_bytes++;
    return 1U;
}

uint8_t UsartEngine_LL_RecvChar(UsartEngine_t *self, uint8_t *data,
                                uint32_t timeout_ms)
{
    usart_wait_budget_t budget;

    if (!usart_engine_hw_ready(self) || data == NULL) {
        return 0U;
    }
    usart_engine_wait_start(&budget, timeout_ms);
    for (;;) {
        const uint16_t status = self->usart->STATR;
        if ((status & (USART_FLAG_RXNE | USART_ENGINE_ERROR_FLAGS)) != 0U) {
            const uint8_t rx_data = (uint8_t)USART_ReceiveData(self->usart);

            if ((status & USART_ENGINE_ERROR_FLAGS) != 0U) {
                usart_engine_record_status_errors(self, status);
            }
            if ((status & USART_FLAG_RXNE) != 0U &&
                (status & USART_ENGINE_DATA_ERROR_FLAGS) == 0U) {
                *data = rx_data;
                self->diagnostics.rx_bytes++;
                return 1U;
            }
            if ((status & USART_FLAG_RXNE) != 0U &&
                (status & USART_ENGINE_DATA_ERROR_FLAGS) != 0U) {
                self->diagnostics.rx_error_dropped++;
            }
        }
        if (usart_engine_wait_expired(&budget)) {
            if (timeout_ms != 0U) {
                self->diagnostics.ll_rx_timeouts++;
            }
            return 0U;
        }
    }
}

uint8_t UsartEngine_LL_TryRecvChar(UsartEngine_t *self, uint8_t *data)
{
    uint16_t status;
    uint8_t rx_data;

    if (!usart_engine_hw_ready(self) || data == NULL) {
        return 0U;
    }
    status = self->usart->STATR;
    if ((status & (USART_FLAG_RXNE | USART_ENGINE_ERROR_FLAGS)) == 0U) {
        return 0U;
    }
    rx_data = (uint8_t)USART_ReceiveData(self->usart);
    if ((status & USART_ENGINE_ERROR_FLAGS) != 0U) {
        usart_engine_record_status_errors(self, status);
    }
    if ((status & USART_FLAG_RXNE) == 0U ||
        (status & USART_ENGINE_DATA_ERROR_FLAGS) != 0U) {
        if ((status & USART_FLAG_RXNE) != 0U &&
            (status & USART_ENGINE_DATA_ERROR_FLAGS) != 0U) {
            self->diagnostics.rx_error_dropped++;
        }
        (void)rx_data;
        return 0U;
    }
    *data = rx_data;
    self->diagnostics.rx_bytes++;
    return 1U;
}

uint8_t UsartEngine_LL_Flush(UsartEngine_t *self, uint32_t timeout_ms)
{
    usart_wait_budget_t budget;

    if (!usart_engine_hw_ready(self)) {
        return 0U;
    }
    usart_engine_wait_start(&budget, timeout_ms);
    while (USART_GetFlagStatus(self->usart, USART_FLAG_TC) == RESET) {
        if (usart_engine_wait_expired(&budget)) {
            if (timeout_ms != 0U) {
                self->diagnostics.ll_tx_timeouts++;
            }
            return 0U;
        }
    }
    return 1U;
}

void UsartEngine_LL_SendString(UsartEngine_t *self, const char *str)
{
    if (str == NULL) {
        return;
    }
    while (*str != '\0') {
        if (!UsartEngine_LL_SendChar(self, (uint8_t)*str++, 10U)) {
            break;
        }
    }
}

UsartEngineStatus_t UsartEngine_HL_Init(UsartEngine_t *self,
                                        USART_TypeDef *p_usart,
                                        uint8_t *rx_mem, size_t rx_size,
                                        uint8_t *tx_mem, size_t tx_size)
{
    uint32_t recovery_flags = 0U;
    uint8_t state_valid;

    if (self == NULL || p_usart == NULL || rx_mem == NULL || tx_mem == NULL ||
        rx_size == 0U || tx_size == 0U) {
        return USART_ENGINE_STATUS_INVALID_ARGUMENT;
    }

    state_valid = usart_engine_state_is_valid(self);
    if (!state_valid) {
        recovery_flags |= USART_ENGINE_RECOVERY_STATE_INVALID;
    } else {
        if (self->usart != p_usart) {
            recovery_flags |= USART_ENGINE_RECOVERY_REBOUND;
        }
        if (self->high_level_mode != 0U || self->tx_busy != 0U) {
            recovery_flags |= USART_ENGINE_RECOVERY_STALE_OPERATION;
        }
    }

    if ((p_usart->CTLR1 & (USART_CTLR1_RXNEIE | USART_CTLR1_TXEIE)) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_IRQ_SOURCES_ENABLED;
    }
    if ((p_usart->STATR & USART_ENGINE_ERROR_FLAGS) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_HW_ERROR;
    }
    if ((p_usart->STATR & USART_FLAG_RXNE) != 0U) {
        recovery_flags |= USART_ENGINE_RECOVERY_PENDING_RX;
    }

    usart_engine_disable_interrupts(p_usart);
    if (!state_valid) {
        usart_engine_reset_state(self, p_usart);
    } else {
        self->usart = p_usart;
        self->high_level_mode = 0U;
        self->tx_busy = 0U;
    }
    self->last_recovery_flags = recovery_flags;
    self->diagnostics.init_count++;
    RingBuffer_Init(&self->rx, rx_mem, rx_size);
    RingBuffer_Init(&self->tx, tx_mem, tx_size);

    if (p_usart->BRR == 0U) {
        return USART_ENGINE_STATUS_HW_NOT_CONFIGURED;
    }
    p_usart->CTLR1 |= USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE;
    self->high_level_mode = 1U;
    if (recovery_flags != 0U) {
        self->diagnostics.recovery_count++;
    }

    USART_ITConfig(p_usart, USART_IT_ERR, ENABLE);
    USART_ITConfig(p_usart, USART_IT_PE, ENABLE);
    USART_ITConfig(p_usart, USART_IT_RXNE, ENABLE);
    return recovery_flags != 0U ? USART_ENGINE_STATUS_RECOVERED
                                : USART_ENGINE_STATUS_OK;
}

void UsartEngine_HL_EnableInterrupts(UsartEngine_t *self,
                                      uint8_t *rx_mem, size_t rx_size,
                                      uint8_t *tx_mem, size_t tx_size)
{
    (void)UsartEngine_HL_Init(self, USART_ENGINE_DEFAULT_USART,
                              rx_mem, rx_size, tx_mem, tx_size);
}

void UsartEngine_HL_TriggerTx(UsartEngine_t *self)
{
    if (!usart_engine_state_is_valid(self) || self->usart == NULL ||
        self->high_level_mode == 0U || self->tx.ops == NULL) {
        return;
    }
    if (self->tx_busy == 0U && !self->tx.ops->is_empty(&self->tx)) {
        self->tx_busy = 1U;
        USART_ITConfig(self->usart, USART_IT_TXE, ENABLE);
    }
}

void UsartEngine_HL_IRQHandler(UsartEngine_t *self)
{
    USART_TypeDef *usart;
    uint32_t rx_budget = USART_ENGINE_ISR_RX_BUDGET;

    if (!usart_engine_state_is_valid(self) || self->usart == NULL) {
        return;
    }
    usart = self->usart;
    if (self->high_level_mode == 0U || self->rx.ops == NULL ||
        self->tx.ops == NULL) {
        usart_engine_disable_interrupts(usart);
        return;
    }
    self->diagnostics.irq_count++;

    while (rx_budget-- != 0U) {
        const uint16_t status = usart->STATR;
        uint8_t data;

        if ((status & (USART_FLAG_RXNE | USART_ENGINE_ERROR_FLAGS)) == 0U) {
            break;
        }

        /* Reading DATAR is required even when RXNE is clear: it completes
         * the CH32 error-flag clear sequence and prevents an EIE storm. */
        data = (uint8_t)USART_ReceiveData(usart);

        if ((status & USART_ENGINE_ERROR_FLAGS) != 0U) {
            usart_engine_record_status_errors(self, status);
        }
        if ((status & USART_FLAG_RXNE) == 0U) {
            continue;
        }
        if ((status & USART_ENGINE_DATA_ERROR_FLAGS) != 0U) {
            self->diagnostics.rx_error_dropped++;
        } else if (self->rx.ops->push(&self->rx, data)) {
            self->diagnostics.rx_bytes++;
        } else {
            self->diagnostics.rx_dropped++;
        }
    }

    if ((usart->CTLR1 & USART_CTLR1_TXEIE) != 0U &&
        (usart->STATR & USART_FLAG_TXE) != 0U) {
        uint8_t data;
        if (self->tx.ops->pop(&self->tx, &data)) {
            USART_SendData(usart, data);
            self->diagnostics.tx_bytes++;
        } else {
            USART_ITConfig(usart, USART_IT_TXE, DISABLE);
            self->tx_busy = 0U;
        }
    }
}

void UsartEngine_RecordDiscardedInput(UsartEngine_t *self,
                                       const UsartEngineRxDiscard_t *discarded)
{
    if (!usart_engine_state_is_valid(self) || discarded == NULL) {
        return;
    }
    self->diagnostics.rx_faults += discarded->rx_faults;
    self->diagnostics.parity_errors += discarded->parity_errors;
    self->diagnostics.framing_errors += discarded->framing_errors;
    self->diagnostics.noise_errors += discarded->noise_errors;
    self->diagnostics.overrun_errors += discarded->overrun_errors;
    self->diagnostics.rx_dropped +=
        discarded->discarded_bytes + discarded->overrun_errors;
    self->diagnostics.rx_error_dropped += discarded->error_discarded_bytes;
}

void UsartEngine_GetDiagnostics(const UsartEngine_t *self,
                                UsartEngineDiagnostics_t *diagnostics)
{
    uint32_t interrupt_state;

    if (diagnostics == NULL) {
        return;
    }
    usart_engine_clear_diagnostics(diagnostics);
    if (!usart_engine_state_is_valid(self)) {
        return;
    }
    interrupt_state = Ch32Irq_SaveAndDisable();
    *diagnostics = self->diagnostics;
    Ch32Irq_Restore(interrupt_state);
}
