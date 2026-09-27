/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Description        : 正式产品启动入口：USB CDC 日志、存储自检和 MIPS Linux 引导。
 *******************************************************************************/

#include <stdint.h>

#include "ch32v20x_rcc.h"
#include "ch32v20x_usart.h"
#include "debug.h"
#include "mips_runtime.h"
#include "spi_storage.h"
#include "usb_cdc.h"

/*
 * 正式版完全停用片上 USART，避免遗留调试配置或复位前状态重新打开串口。
 * 先关中断、清 pending，再复位外设和关闭时钟；产品日志只走 USB CDC。
 */
static void product_disable_usart(void)
{
    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    USART_Cmd(USART1, DISABLE);
    USART_DeInit(USART1);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE);

    NVIC_DisableIRQ(USART2_IRQn);
    NVIC_ClearPendingIRQ(USART2_IRQn);
    USART_Cmd(USART2, DISABLE);
    USART_DeInit(USART2);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2 |
                           RCC_APB1Periph_USART3, DISABLE);

    NVIC_DisableIRQ(USART3_IRQn);
    NVIC_ClearPendingIRQ(USART3_IRQn);
    USART_Cmd(USART3, DISABLE);
    USART_DeInit(USART3);
}

/* 输出两颗外部存储器的硬件探测结果，便于现场区分接线和镜像故障。 */
static void product_print_storage_info(const spi_storage_info_t *info)
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

/* 产品启动只接受已验证的两种存储器型号，避免错接后破坏 guest 内存。 */
static uint8_t product_nor_id_valid(const spi_storage_info_t *info)
{
    return (uint8_t)(info->nor_present &&
                     info->nor_id[0] == 0xEFu &&
                     info->nor_id[1] == 0x40u &&
                     info->nor_id[2] == 0x17u);
}

static uint8_t product_psram_id_valid(const spi_storage_info_t *info)
{
    return (uint8_t)(info->psram_present &&
                     info->psram_id[0] == 0x0Du &&
                     info->psram_id[1] == 0x5Du);
}

/* 在等待 USB 主机枚举的同时保持 CDC 中断和发送队列得到服务。 */
static void product_delay_with_usb(uint32_t milliseconds)
{
    while(milliseconds != 0u)
    {
        Delay_Ms(1u);
        UsbCdc_Service();
        --milliseconds;
    }
}

/* 将 MIPS loader 的阶段进度转换成简短 CDC 日志，并及时发送到主机。 */
static void product_boot_progress(mips_runtime_progress_stage_t stage,
                                  uint32_t completed,
                                  uint32_t total)
{
    const char *stage_name;
    unsigned long percent = 100ul;

    switch(stage)
    {
        case MIPS_RUNTIME_PROGRESS_MANIFEST:
            stage_name = "manifest";
            break;
        case MIPS_RUNTIME_PROGRESS_VERIFY:
            stage_name = "payload CRC32";
            break;
        case MIPS_RUNTIME_PROGRESS_PT_LOAD:
            stage_name = "PT_LOAD -> APS6404L";
            break;
        case MIPS_RUNTIME_PROGRESS_DTB:
            stage_name = "DTB -> APS6404L";
            break;
        case MIPS_RUNTIME_PROGRESS_INITRAMFS:
            stage_name = "initramfs -> APS6404L";
            break;
        case MIPS_RUNTIME_PROGRESS_HANDOFF:
            stage_name = "Linux handoff";
            break;
        default:
            stage_name = "boot";
            break;
    }

    if(total != 0u)
    {
        percent = ((unsigned long)completed * 100ul) /
                  (unsigned long)total;
    }
    printf("boot: %s %lu/%lu (%lu%%)\r\n",
           stage_name,
           (unsigned long)completed,
           (unsigned long)total,
           percent);
    /* 同步 SPI 读取期间没有主循环机会，回调必须主动泵 USB。 */
    UsbCdc_Service();
}

/* 产品入口不再初始化 USART2 或注册命令行，所有诊断只走 USB CDC。 */
int main(void)
{
    spi_storage_info_t storage_info;
    spi_storage_status_t storage_status;
    mips_runtime_status_t runtime_status;
    uint8_t nor_valid;
    uint8_t psram_valid;
    uint8_t usb_ready;
    uint8_t runtime_ready = 0u;
    uint32_t entry_address = 0u;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    product_disable_usart();

    /* USB 必须早于 SPI 自检，确保外部存储故障也能报告给主机。 */
    usb_ready = UsbCdc_Init();
    if(!usb_ready)
    {
        /* UsbCdc_Init 当前不会失败，但保留明确的产品故障文本。 */
        printf("ERROR: USB CDC initialization failed\r\n");
    }
    else
    {
        printf("CH32V203F6P product firmware\r\n");
        printf("SystemClk:%lu\r\n", (unsigned long)SystemCoreClock);
        printf("ChipID:%08lx\r\n", (unsigned long)DBGMCU_GetCHIPID());
        printf("USB CDC ACM: firmware log + guest 16550 UART\r\n");
    }
    UsbCdc_Service();

    storage_status = SpiStorage_Init();
    SpiStorage_GetInfo(&storage_info);
    if(storage_status != SPI_STORAGE_STATUS_OK)
    {
        printf("ERROR: SPI storage controller init failed status=%u\r\n",
               (unsigned int)storage_status);
    }
    product_print_storage_info(&storage_info);
    nor_valid = product_nor_id_valid(&storage_info);
    psram_valid = product_psram_id_valid(&storage_info);
    if(!nor_valid)
    {
        printf("ERROR: W25Q64 JEDEC ID is missing or unexpected; check CS0/power/wiring\r\n");
    }
    if(!psram_valid)
    {
        printf("ERROR: APS6404L ID is missing or unexpected; check CS1/power/wiring\r\n");
    }
    if(storage_status == SPI_STORAGE_STATUS_OK && nor_valid && psram_valid)
    {
        runtime_ready = MipsRuntime_Init();
        printf("APS6404L MIPS memory backend: %s\r\n",
               runtime_ready ? "ready" : "failed");
    }
    else if(storage_status != SPI_STORAGE_STATUS_OK)
    {
        printf("ERROR: MIPS runtime not started because SPI storage init failed\r\n");
    }
    MipsRuntime_SetProgressCallback(product_boot_progress);
    UsbCdc_Service();

    printf("Boot delay: 3000 ms; connect to USB CDC for loader progress\r\n");
    product_delay_with_usb(3000u);

    if(!runtime_ready)
    {
        printf("ERROR: Linux boot stopped because external storage is unavailable\r\n");
    }
    else
    {
        printf("boot: verifying image and loading PT_LOAD into PSRAM\r\n");
        UsbCdc_Service();
        runtime_status = MipsRuntime_Boot(&entry_address);
        if(runtime_status != MIPS_RUNTIME_STATUS_OK)
        {
            printf("ERROR: boot failed: %s\r\n",
                   MipsRuntime_StatusString(runtime_status));
            if(runtime_status == MIPS_RUNTIME_STATUS_PSRAM_IO)
            {
                printf("ERROR: APS6404L transaction failed; inspect SPI1 wiring and power\r\n");
            }
        }
        else
        {
            printf("Linux boot at %08lx; entering guest run loop\r\n",
                   (unsigned long)entry_address);
        }
    }

    while(1)
    {
        UsbCdc_Service();
        if(MipsRuntime_IsRunning())
        {
            MipsRuntime_RunSlice();
        }
    }
}
