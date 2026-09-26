/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2023/12/29
 * Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v20x_it.h"
#include "usart_engine.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   捕获 HardFault 并打印机器异常寄存器，便于串口定位启动故障。
 *
 * @return  none
 */
void HardFault_Handler(void)
{
  printf("HardFault: mcause=%08lx mepc=%08lx mtval=%08lx\r\n",
         (unsigned long)__get_MCAUSE(),
         (unsigned long)__get_MEPC(),
         (unsigned long)__get_MTVAL());
  while (1)
  {
  }
}

/*********************************************************************
 * @fn      USART2_IRQHandler
 *
 * @brief   处理 USART 命令行的接收与发送中断。
 *
 * @return  none
 */
void USART2_IRQHandler(void)
{
  UsartEngine_HL_IRQHandler(UsartEngine_GetInstance());
}


