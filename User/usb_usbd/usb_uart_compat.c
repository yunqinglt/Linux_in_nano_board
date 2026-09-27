#include "UART.h"

/* CDC line coding 与物理 USART2 分离，仅保存 USB 控制请求的数据。 */
volatile UART_CTL Uart = {{0x00u, 0xc2u, 0x01u, 0x00u, 0x00u, 0x00u, 0x08u, 0x00u}};

/* USB CDC 的 line-coding 不应重新配置物理 USART2 命令行。 */
void UART2_USB_Init(void)
{
}
