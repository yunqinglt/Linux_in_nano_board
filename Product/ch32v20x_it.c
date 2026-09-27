/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Description        : 正式版只保留基础异常处理，不链接 USART 命令行 ISR。
 *******************************************************************************/

#include "ch32v20x_it.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* NMI 没有可恢复上下文，保持在此处等待调试器读取现场。 */
void NMI_Handler(void)
{
    while(1)
    {
    }
}

/* 通过 CDC 日志报告机器异常寄存器，随后停机等待现场分析。 */
void HardFault_Handler(void)
{
    printf("HardFault: mcause=%08lx mepc=%08lx mtval=%08lx\r\n",
           (unsigned long)__get_MCAUSE(),
           (unsigned long)__get_MEPC(),
           (unsigned long)__get_MTVAL());
    while(1)
    {
    }
}
