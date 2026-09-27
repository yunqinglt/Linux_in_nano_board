#ifndef __UART_H__
#define __UART_H__

#include <stdint.h>

/* 仅保留官方 CDC 控制请求所需的 line-coding 字段，不占用 USART2。 */
typedef struct
{
    uint8_t Com_Cfg[8];
} UART_CTL;

extern volatile UART_CTL Uart;
void UART2_USB_Init(void);

#endif
