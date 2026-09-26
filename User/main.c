/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/06/06
 * Description        : Main program body.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*
 *@Note
 *USART Print debugging routine:
 *USART1_Tx(PA9).
 *This example demonstrates using USART1(PA9) as a print debug port output.
 *
 */

#include <stddef.h>

#include "debug.h"

#define SPI_PROBE_CS_W25Q64       GPIO_Pin_0
#define SPI_PROBE_CS_APS6404L     GPIO_Pin_1
#define SPI_PROBE_TIMEOUT         100000u

#define W25Q64_CMD_READ_ID        0x9Fu
#define APS6404L_CMD_RESET_ENABLE 0x66u
#define APS6404L_CMD_RESET        0x99u
#define APS6404L_CMD_READ_ID      0x9Fu

typedef struct
{
    uint8_t transfer_ok;
    uint8_t present;
    uint8_t expected;
    uint8_t id[3];
} spi_probe_w25q64_id_t;

typedef struct
{
    uint8_t transfer_ok;
    uint8_t present;
    uint8_t expected;
    uint8_t mf_id;
    uint8_t kgd;
    uint8_t eid[6];
} spi_probe_aps6404l_id_t;

static void spi_probe_select(uint16_t chip_select)
{
    GPIO_ResetBits(GPIOA, chip_select);
}

static void spi_probe_deselect(uint16_t chip_select)
{
    GPIO_SetBits(GPIOA, chip_select);
    /* Keep CE# high between commands and leave the bus idle. */
    Delay_Us(1);
}

static uint8_t spi_probe_wait(uint16_t flag, FlagStatus state)
{
    uint32_t timeout = SPI_PROBE_TIMEOUT;

    while(SPI_I2S_GetFlagStatus(SPI1, flag) != state)
    {
        if(timeout-- == 0u)
        {
            return 0u;
        }
    }
    return 1u;
}

static uint8_t spi_probe_transfer(uint8_t tx, uint8_t *rx)
{
    if(!spi_probe_wait(SPI_I2S_FLAG_TXE, SET))
    {
        return 0u;
    }

    SPI_I2S_SendData(SPI1, tx);
    if(!spi_probe_wait(SPI_I2S_FLAG_RXNE, SET))
    {
        return 0u;
    }

    *rx = (uint8_t)SPI_I2S_ReceiveData(SPI1);
    return 1u;
}

static uint8_t spi_probe_finish(void)
{
    return spi_probe_wait(SPI_I2S_FLAG_BSY, RESET);
}

static uint8_t spi_probe_is_floating(const uint8_t *id, size_t length)
{
    size_t i;
    uint8_t all_zero = 1u;
    uint8_t all_ff = 1u;

    for(i = 0u; i < length; ++i)
    {
        if(id[i] != 0x00u)
        {
            all_zero = 0u;
        }
        if(id[i] != 0xFFu)
        {
            all_ff = 0u;
        }
    }
    return (uint8_t)(all_zero || all_ff);
}

static void spi_probe_reset_aps6404l(void)
{
    uint8_t ignored;

    spi_probe_select(SPI_PROBE_CS_APS6404L);
    (void)spi_probe_transfer(APS6404L_CMD_RESET_ENABLE, &ignored);
    (void)spi_probe_finish();
    spi_probe_deselect(SPI_PROBE_CS_APS6404L);

    spi_probe_select(SPI_PROBE_CS_APS6404L);
    (void)spi_probe_transfer(APS6404L_CMD_RESET, &ignored);
    (void)spi_probe_finish();
    spi_probe_deselect(SPI_PROBE_CS_APS6404L);

    /* APS6404L specifies at least 150 us for reset completion. */
    Delay_Ms(1);
}

static void spi_probe_w25q64(spi_probe_w25q64_id_t *result)
{
    uint8_t ignored;
    uint8_t i;

    result->transfer_ok = 1u;
    result->id[0] = 0u;
    result->id[1] = 0u;
    result->id[2] = 0u;

    spi_probe_select(SPI_PROBE_CS_W25Q64);
    if(!spi_probe_transfer(W25Q64_CMD_READ_ID, &ignored))
    {
        result->transfer_ok = 0u;
    }
    for(i = 0u; i < 3u && result->transfer_ok; ++i)
    {
        if(!spi_probe_transfer(0x00u, &result->id[i]))
        {
            result->transfer_ok = 0u;
        }
    }
    if(result->transfer_ok && !spi_probe_finish())
    {
        result->transfer_ok = 0u;
    }
    spi_probe_deselect(SPI_PROBE_CS_W25Q64);

    result->present = (uint8_t)(result->transfer_ok &&
                                !spi_probe_is_floating(result->id, 3u));
    result->expected = (uint8_t)(result->present &&
                                 result->id[0] == 0xEFu &&
                                 result->id[1] == 0x40u &&
                                 result->id[2] == 0x17u);
}

static void spi_probe_aps6404l(spi_probe_aps6404l_id_t *result)
{
    uint8_t ignored;
    uint8_t address_byte;
    uint8_t i;

    result->transfer_ok = 1u;
    result->mf_id = 0u;
    result->kgd = 0u;
    for(i = 0u; i < 6u; ++i)
    {
        result->eid[i] = 0u;
    }

    spi_probe_reset_aps6404l();
    spi_probe_select(SPI_PROBE_CS_APS6404L);
    if(!spi_probe_transfer(APS6404L_CMD_READ_ID, &ignored))
    {
        result->transfer_ok = 0u;
    }

    /* APS6404L Read ID is 0x9F followed by 24 don't-care address bits. */
    for(address_byte = 0u;
        address_byte < 3u && result->transfer_ok;
        ++address_byte)
    {
        if(!spi_probe_transfer(0x00u, &ignored))
        {
            result->transfer_ok = 0u;
        }
    }
    if(result->transfer_ok && !spi_probe_transfer(0x00u, &result->mf_id))
    {
        result->transfer_ok = 0u;
    }
    if(result->transfer_ok && !spi_probe_transfer(0x00u, &result->kgd))
    {
        result->transfer_ok = 0u;
    }
    for(i = 0u; i < 6u && result->transfer_ok; ++i)
    {
        if(!spi_probe_transfer(0x00u, &result->eid[i]))
        {
            result->transfer_ok = 0u;
        }
    }
    if(result->transfer_ok && !spi_probe_finish())
    {
        result->transfer_ok = 0u;
    }
    spi_probe_deselect(SPI_PROBE_CS_APS6404L);

    result->present = (uint8_t)(result->transfer_ok &&
                                !(result->mf_id == 0x00u &&
                                  result->kgd == 0x00u) &&
                                !(result->mf_id == 0xFFu &&
                                  result->kgd == 0xFFu));
    result->expected = (uint8_t)(result->present &&
                                 result->mf_id == 0x0Du &&
                                 result->kgd == 0x5Du);
}

static void spi_probe_init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO |
                           RCC_APB2Periph_GPIOA |
                           RCC_APB2Periph_SPI1,
                           ENABLE);

    gpio.GPIO_Pin = SPI_PROBE_CS_W25Q64 | SPI_PROBE_CS_APS6404L;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_SetBits(GPIOA, SPI_PROBE_CS_W25Q64 | SPI_PROBE_CS_APS6404L);

    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    SPI_StructInit(&spi);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(SPI1, &spi);
    /* Software NSS also needs the internal NSS level held high in master mode. */
    SPI_NSSInternalSoftwareConfig(SPI1, SPI_NSSInternalSoft_Set);
    SPI_Cmd(SPI1, ENABLE);
}

static void spi_probe_read_ids(spi_probe_w25q64_id_t *w25q64,
                               spi_probe_aps6404l_id_t *aps6404l)
{
    GPIO_SetBits(GPIOA, SPI_PROBE_CS_W25Q64 | SPI_PROBE_CS_APS6404L);
    spi_probe_w25q64(w25q64);
    spi_probe_aps6404l(aps6404l);
}

/* Global typedef */

/* Global define */

/* Global Variable */

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    spi_probe_w25q64_id_t w25q64;
    spi_probe_aps6404l_id_t aps6404l;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);
    printf("SystemClk:%d\r\n", SystemCoreClock);
    printf( "ChipID:%08x\r\n", DBGMCU_GetCHIPID() );

    spi_probe_init();
    spi_probe_read_ids(&w25q64, &aps6404l);

    printf("SPI1 W25Q64 CS0: transfer=%s present=%s id=%02X %02X %02X expected=%s\r\n",
           w25q64.transfer_ok ? "ok" : "timeout",
           w25q64.present ? "yes" : "no",
           w25q64.id[0], w25q64.id[1], w25q64.id[2],
           w25q64.expected ? "yes" : "no");
    printf("SPI1 APS6404L CS1: transfer=%s present=%s mf=%02X kgd=%02X eid=%02X%02X%02X%02X%02X%02X expected=%s\r\n",
           aps6404l.transfer_ok ? "ok" : "timeout",
           aps6404l.present ? "yes" : "no",
           aps6404l.mf_id, aps6404l.kgd,
           aps6404l.eid[0], aps6404l.eid[1], aps6404l.eid[2],
           aps6404l.eid[3], aps6404l.eid[4], aps6404l.eid[5],
           aps6404l.expected ? "yes" : "no");

    while(1)
    {
    }
}
