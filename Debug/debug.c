/********************************** (C) COPYRIGHT  *******************************
 * File Name          : debug.c
 * Author             : WCH
 * Version            : V1.0.1
 * Date               : 2026/07/10
 * Description        : This file contains all the functions prototypes for UART
 *                      Printf , Delay functions.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
#include "debug.h"

#include <stdarg.h>
#include <stddef.h>

#if defined(PRODUCT_BUILD) && PRODUCT_BUILD
#include "usb_cdc.h"
#endif

static uint8_t  p_us = 0;
static uint16_t p_ms = 0;

int _write(int fd, char *buf, int size);

/* 通过 WCH _write 输出单个字符，避开 newlib 的 FILE/heap 缓冲状态。 */
static void firmware_log_char(char character)
{
    (void)_write(1, &character, 1);
}

/* 输出无符号整数并按格式串要求补齐宽度。 */
static unsigned int firmware_log_unsigned(unsigned long value,
                                          unsigned int base,
                                          unsigned int width,
                                          char padding,
                                          uint8_t uppercase)
{
    char digits[16];
    unsigned int count = 0u;
    unsigned int output_count = 0u;
    const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    do
    {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while(value != 0u && count < sizeof(digits));
    while(count < width)
    {
        firmware_log_char(padding);
        ++output_count;
        --width;
    }
    while(count != 0u)
    {
        firmware_log_char(digits[--count]);
        ++output_count;
    }
    return output_count;
}

/* 实现项目所用的轻量 printf 子集：s/c/d/u/x/X 和 l/宽度/零填充。 */
int Firmware_Printf(const char *format, ...)
{
    va_list arguments;
    int output_count = 0;

    if(format == NULL)
    {
        return 0;
    }
    va_start(arguments, format);
    while(*format != '\0')
    {
        unsigned int width = 0u;
        char padding = ' ';
        uint8_t long_value = 0u;
        char conversion;

        if(*format != '%')
        {
            firmware_log_char(*format++);
            ++output_count;
            continue;
        }
        ++format;
        if(*format == '%')
        {
            firmware_log_char(*format++);
            ++output_count;
            continue;
        }
        if(*format == '0')
        {
            padding = '0';
            ++format;
        }
        while(*format >= '0' && *format <= '9')
        {
            width = width * 10u + (unsigned int)(*format - '0');
            ++format;
        }
        if(*format == 'l')
        {
            long_value = 1u;
            ++format;
        }
        conversion = *format;
        if(conversion != '\0')
        {
            ++format;
        }

        if(conversion == 's')
        {
            const char *text = va_arg(arguments, const char *);
            if(text == NULL)
            {
                text = "(null)";
            }
            while(*text != '\0')
            {
                firmware_log_char(*text++);
                ++output_count;
            }
        }
        else if(conversion == 'c')
        {
            firmware_log_char((char)va_arg(arguments, int));
            ++output_count;
        }
        else if(conversion == 'd' || conversion == 'i')
        {
            long value = long_value ? va_arg(arguments, long) :
                                      (long)va_arg(arguments, int);
            unsigned long magnitude;
            if(value < 0)
            {
                firmware_log_char('-');
                ++output_count;
                magnitude = (unsigned long)(-(value + 1L)) + 1u;
                if(width != 0u)
                {
                    --width;
                }
            }
            else
            {
                magnitude = (unsigned long)value;
            }
            output_count += firmware_log_unsigned(magnitude, 10u, width,
                                                  padding, 0u);
        }
        else if(conversion == 'u' || conversion == 'x' || conversion == 'X')
        {
            unsigned long value = long_value ? va_arg(arguments, unsigned long) :
                                              (unsigned long)va_arg(arguments, unsigned int);
            output_count += firmware_log_unsigned(
                value, conversion == 'u' ? 10u : 16u,
                width, padding, conversion == 'X');
        }
        else
        {
            firmware_log_char('%');
            ++output_count;
            if(conversion != '\0')
            {
                firmware_log_char(conversion);
                ++output_count;
            }
        }
    }
    va_end(arguments);
    return output_count;
}

#define DEBUG_DATA0_ADDRESS  ((volatile uint32_t*)0xE0000380)
#define DEBUG_DATA1_ADDRESS  ((volatile uint32_t*)0xE0000384)

/*********************************************************************
 * @fn      Delay_Init
 *
 * @brief   Initializes Delay Funcation.
 *
 * @return  none
 */
void Delay_Init(void)
{
    p_us = SystemCoreClock / 8000000;
    p_ms = (uint16_t)p_us * 1000;
}

/*********************************************************************
 * @fn      Delay_Us
 *
 * @brief   Microsecond Delay Time.
 *
 * @param   n - Microsecond number.
 *
 * @return  None
 */
void Delay_Us(uint32_t n)
{
    uint32_t i;

    SysTick->SR &= ~(1 << 0);
    i = (uint32_t)n * p_us;

    SysTick->CMP = i;
    SysTick->CTLR |= (1 << 4);
    SysTick->CTLR |= (1 << 5) | (1 << 0);

    while((SysTick->SR & (1 << 0)) != (1 << 0));
    SysTick->CTLR &= ~(1 << 0);
}

/*********************************************************************
 * @fn      Delay_Ms
 *
 * @brief   Millisecond Delay Time.
 *
 * @param   n - Millisecond number.
 *
 * @return  None
 */
void Delay_Ms(uint32_t n)
{
    uint32_t i;

    SysTick->SR &= ~(1 << 0);
    i = (uint32_t)n * p_ms;

    SysTick->CMP = i;
    SysTick->CTLR |= (1 << 4);
    SysTick->CTLR |= (1 << 5) | (1 << 0);

    while((SysTick->SR & (1 << 0)) != (1 << 0));
    SysTick->CTLR &= ~(1 << 0);
}

/*********************************************************************
 * @fn      USART_Printf_Init
 *
 * @brief   Initializes the USARTx peripheral.
 *
 * @param   baudrate - USART communication baud rate.
 *
 * @return  None
 */
void USART_Printf_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

#if(DEBUG == DEBUG_UART1)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

#elif(DEBUG == DEBUG_UART2)
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

#elif(DEBUG == DEBUG_UART3)
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

#endif

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
#if(DEBUG == DEBUG_UART2)
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
#else
    USART_InitStructure.USART_Mode = USART_Mode_Tx;
#endif

#if(DEBUG == DEBUG_UART1)
    USART_Init(USART1, &USART_InitStructure);
    USART_Cmd(USART1, ENABLE);

#elif(DEBUG == DEBUG_UART2)
    USART_Init(USART2, &USART_InitStructure);
    USART_Cmd(USART2, ENABLE);

#elif(DEBUG == DEBUG_UART3)
    USART_Init(USART3, &USART_InitStructure);
    USART_Cmd(USART3, ENABLE);

#endif
}

/*********************************************************************
 * @fn      SDI_Printf_Enable
 *
 * @brief   Initializes the SDI printf Function.
 *
 * @param   None
 *
 * @return  None
 */
void SDI_Printf_Enable(void)
{
    *(DEBUG_DATA0_ADDRESS) = 0;
    Delay_Init();
    Delay_Ms(1);
}

/*********************************************************************
 * @fn      _write
 *
 * @brief   Support Printf Function
 *
 * @param   *buf - UART send Data.
 *          size - Data length
 *
 * @return  size: Data length
 */
__attribute__((used))
int _write(int fd, char *buf, int size)
{
#if defined(PRODUCT_BUILD) && PRODUCT_BUILD
    (void)fd;
    if(buf == NULL || size <= 0)
    {
        return 0;
    }
    /* 正式版不初始化 USART2，所有 Firmware_Printf 字节进入 CDC 日志队列。 */
    return (int)UsbCdc_LogWrite((const uint8_t *)buf, (size_t)size);
#else
    int i = 0;
#if (SDI_PRINT == SDI_PR_OPEN)
    int writeSize = size;
    do
    {
        /**
         * data0 + data1 = 8 bytes
         * data0 The lowest byte storage length, the maximum is 7
         *
         */

        while( (*(DEBUG_DATA0_ADDRESS) != 0u))
        {

        }

        if(writeSize>7)
        {
            *(DEBUG_DATA1_ADDRESS) =      \
              ((uint8_t)(buf[i+3])) |     \
              ((uint8_t)(buf[i+4]) << 8) |  \
              ((uint8_t)(buf[i+5]) << 16) | \
              ((uint8_t)(buf[i+6]) << 24);
            *(DEBUG_DATA0_ADDRESS) =      \
              (7u) |                      \
              ((uint8_t)(buf[i]) << 8) |    \
              ((uint8_t)(buf[i+1]) << 16) | \
              ((uint8_t)(buf[i+2]) << 24);   

            i += 7;
            writeSize -= 7;
        }
        else
        {
            uint8_t tmp = (uint8_t)writeSize;
            *(DEBUG_DATA1_ADDRESS) =                       \
                ((tmp > 3 ? (uint8_t)buf[i+3] : 0)) |      \
                ((tmp > 4 ? (uint8_t)buf[i+4] : 0) << 8) | \
                ((tmp > 5 ? (uint8_t)buf[i+5] : 0) << 16) |\
                ((tmp > 6 ? (uint8_t)buf[i+6] : 0) << 24);
            *(DEBUG_DATA0_ADDRESS) = 
                (tmp) | ((uint8_t)buf[i] << 8) |           \
                ((tmp > 1 ? (uint8_t)buf[i+1] : 0) << 16) |\
                ((tmp > 2 ? (uint8_t)buf[i+2] : 0) << 24);
            writeSize = 0;
        }

    } while (writeSize);

#else
    for(i = 0; i < size; i++){
#if(DEBUG == DEBUG_UART1)
        while(USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
        USART_SendData(USART1, *buf++);
#elif(DEBUG == DEBUG_UART2)
        while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
        USART_SendData(USART2, *buf++);
#elif(DEBUG == DEBUG_UART3)
        while(USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET);
        USART_SendData(USART3, *buf++);
#endif
    }
#endif
    return size;
#endif
}

/*********************************************************************
 * @fn      _sbrk
 *
 * @brief   Change the spatial position of data segment.
 *
 * @return  size: Data length
 */
__attribute__((used))
void *_sbrk(ptrdiff_t incr)
{
    extern char _end[];
    extern char _heap_end[];
    static char *curbrk = _end;

    if ((curbrk + incr < _end) || (curbrk + incr > _heap_end))
    return NULL - 1;

    curbrk += incr;
    return curbrk - incr;
}
