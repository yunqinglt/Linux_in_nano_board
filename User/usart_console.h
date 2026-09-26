#ifndef USART_CONSOLE_H
#define USART_CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化命令表，并在主循环中非阻塞处理 USART2 输入。 */
uint8_t UsartConsole_Init(void);
void UsartConsole_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* USART_CONSOLE_H */
