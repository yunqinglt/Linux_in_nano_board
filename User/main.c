/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH / project contributors
 * Description        : Product runtime firmware for the CH32V203F6P6 board.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This firmware is intended for WCH microcontrollers.
 *******************************************************************************/

#include <stdint.h>

#include "debug.h"
#include "spi_storage.h"
#include "usart_console.h"

/* 启动时打印两颗 SPI 芯片的探测结果。 */
static void print_storage_info(const spi_storage_info_t *info)
{
    printf("SPI1 W25Q64 CS0: present=%s id=%02X %02X %02X expected=%s\r\n",
           info->nor_present ? "yes" : "no",
           info->nor_id[0], info->nor_id[1], info->nor_id[2],
           (info->nor_id[0] == 0xEFu && info->nor_id[1] == 0x40u &&
            info->nor_id[2] == 0x17u) ? "yes" : "no");
    printf("SPI1 APS6404L CS1: present=%s mf=%02X kgd=%02X eid=%02X%02X expected=%s\r\n",
           info->psram_present ? "yes" : "no",
           info->psram_id[0], info->psram_id[1],
           info->psram_id[2], info->psram_id[3],
           (info->psram_id[0] == 0x0Du &&
            info->psram_id[1] == 0x5Du) ? "yes" : "no");
}

/* 初始化产品固件，并在主循环中非阻塞轮询 USART 命令行。 */
int main(void)
{
    spi_storage_info_t storage_info;
    spi_storage_status_t status;
    uint8_t console_ready;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200u);

    printf("SystemClk:%lu\r\n", (unsigned long)SystemCoreClock);
    printf("ChipID:%08lx\r\n", (unsigned long)DBGMCU_GetCHIPID());
    printf("Product firmware: SPI runtime profile\r\n");

    status = SpiStorage_Init();
    SpiStorage_GetInfo(&storage_info);
    print_storage_info(&storage_info);
    if(status != SPI_STORAGE_STATUS_OK)
    {
        printf("SPI storage init failed: status=%u\r\n", (unsigned int)status);
    }

    console_ready = UsartConsole_Init();
    if(!console_ready)
    {
        printf("USART command table unavailable\r\n");
    }

    while(1)
    {
        if(console_ready)
        {
            UsartConsole_Poll();
        }
    }
}
