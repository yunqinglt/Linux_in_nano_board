/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Author             : WCH / project contributors
 * Description        : USB maintenance firmware for the CH32V203F6P6 board.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This firmware is intended for WCH microcontrollers.
 *******************************************************************************/

#include <stdint.h>

#include "debug.h"
#include "spi_storage.h"
#include "usart_console.h"
#include "usb_vendor.h"

/* 启动时打印两颗 SPI 芯片的探测结果。 */
static void print_storage_info(const usb_vendor_info_t *info)
{
    printf("SPI1 W25Q64 CS0: present=%s id=%02X %02X %02X expected=%s\r\n",
           (info->flags & USB_VENDOR_INFO_FLAG_NOR_PRESENT) ? "yes" : "no",
           info->nor_id[0], info->nor_id[1], info->nor_id[2],
           (info->nor_id[0] == 0xEFu && info->nor_id[1] == 0x40u &&
            info->nor_id[2] == 0x17u) ? "yes" : "no");
    printf("SPI1 APS6404L CS1: present=%s mf=%02X kgd=%02X eid=%02X%02X expected=%s\r\n",
           (info->flags & USB_VENDOR_INFO_FLAG_PSRAM_PRESENT) ? "yes" : "no",
           info->psram_id[0], info->psram_id[1],
           info->psram_id[2], info->psram_id[3],
           (info->psram_id[0] == 0x0Du &&
            info->psram_id[1] == 0x5Du) ? "yes" : "no");
}

/* 初始化维护固件，并在主循环中协同轮询串口与 USB 协议。 */
int main(void)
{
    usb_vendor_info_t storage_info;
    usb_vendor_status_t status;
    uint8_t console_ready;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200u);

    printf("SystemClk:%lu\r\n", (unsigned long)SystemCoreClock);
    printf("ChipID:%08lx\r\n", (unsigned long)DBGMCU_GetCHIPID());
    printf("Maintenance firmware: USB protocol v%u\r\n",
           (unsigned int)USB_VENDOR_PROTOCOL_VERSION);

    status = SpiStorage_Init();
    SpiStorage_GetInfo(&storage_info);
    print_storage_info(&storage_info);
    if(status != USB_VENDOR_STATUS_OK)
    {
        printf("SPI storage init failed: status=%u\r\n", (unsigned int)status);
    }

    status = UsbVendor_Init(SpiStorage_Backend(), SpiStorage_Context());
    if(status != USB_VENDOR_STATUS_OK)
    {
        printf("USB Vendor init failed: status=%u\r\n", (unsigned int)status);
        while(1)
        {
        }
    }

    printf("USB Vendor ready: %04X:%04X, EP1 bulk OUT/IN, max=%lu\r\n",
           USB_VENDOR_VID, USB_VENDOR_PID,
           (unsigned long)USB_VENDOR_MAX_TRANSFER);

    console_ready = UsartConsole_Init();
    if(!console_ready)
    {
        printf("USART command table unavailable; USB maintenance remains active\r\n");
    }

    while(1)
    {
        if(console_ready)
        {
            UsartConsole_Poll();
        }
        UsbVendor_Poll();
    }
}
