#ifndef CH32_IRQ_H
#define CH32_IRQ_H

#include "ch32v20x.h"

#include <stdint.h>

/*
 * CH32V2 application code runs in user mode after startup executes mret.
 * Machine CSRs such as mstatus therefore cannot be read from main().  WCH
 * exposes the global interrupt enable through user CSR 0x800 instead; the
 * vendor __enable_irq()/__disable_irq() helpers operate on the same 0x88 mask.
 */
#define CH32_IRQ_GLOBAL_ENABLE_MASK 0x88u

static inline uint32_t Ch32Irq_SaveAndDisable(void)
{
    uint32_t interrupt_state;

    __asm volatile("csrr %0, 0x800" : "=r"(interrupt_state) :: "memory");
    __disable_irq();
    __asm volatile("" ::: "memory");
    return interrupt_state;
}

static inline void Ch32Irq_Restore(uint32_t interrupt_state)
{
    if((interrupt_state & CH32_IRQ_GLOBAL_ENABLE_MASK) != 0u)
    {
        __asm volatile("" ::: "memory");
        __enable_irq();
        __asm volatile("" ::: "memory");
    }
}

#endif /* CH32_IRQ_H */
