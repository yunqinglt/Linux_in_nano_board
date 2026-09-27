#include "usb_cdc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mips_runtime.h"
#include "usb_conf.h"
#include "usb_desc.h"
#include "usb_istr.h"
#include "usb_lib.h"
#include "usb_pwr.h"

#define CDC_PACKET_SIZE 64u

/* WCH PMA 奇数长度拷贝会访问最后一个额外字节，缓冲区预留哨兵位。 */
static uint8_t g_cdc_rx_packet[CDC_PACKET_SIZE + 1u];
static uint8_t g_cdc_tx_packet[CDC_PACKET_SIZE + 1u];
volatile uint8_t USBD_Endp3_Busy;
static volatile uint8_t g_cdc_rx_pending;
static volatile uint16_t g_cdc_rx_length;
static volatile uint16_t g_cdc_rx_delivered;

/* 清除 guest UART 队列与待处理中断状态。 */
static void cdc_clear_pending(void)
{
    USBD_Endp3_Busy = 0u;
    g_cdc_rx_pending = 0u;
    g_cdc_rx_length = 0u;
    g_cdc_rx_delivered = 0u;
}

/* 初始化 WCH 官方 USBD/PMA 栈和 CDC 端点。 */
uint8_t UsbCdc_Init(void)
{
    cdc_clear_pending();
    Set_USBConfig();
    USB_Init();
    USB_Interrupts_Config();
    return 1u;
}

/* EP1 为 CDC 通知端点，本轮不发送串口状态通知。 */
void UsbCdc_EP1_IN_Callback(void)
{
}

/* EP3 IN 完成后释放 CDC 发送端点。 */
void UsbCdc_EP3_IN_Callback(void)
{
    USBD_Endp3_Busy = 0u;
}

/* EP2 OUT 只复制至 SRAM 暂存区，不在 USB IRQ 内触碰 guest UART。 */
void UsbCdc_EP2_OUT_Callback(void)
{
    uint16_t length = GetEPRxCount(ENDP2);

    if(length > CDC_PACKET_SIZE)
    {
        SetEPRxValid(ENDP2);
        return;
    }
    if(g_cdc_rx_pending)
    {
        return;
    }

    g_cdc_rx_packet[CDC_PACKET_SIZE] = 0u;
    PMAToUserBufferCopy(g_cdc_rx_packet, GetEPRxAddr(ENDP2), length);
    g_cdc_rx_length = length;
    g_cdc_rx_delivered = 0u;
    g_cdc_rx_pending = 1u;
}

/* 在 guest 执行时间片之间，以小包安全搬运 16550A 与 USB CDC 字节流。 */
void UsbCdc_Service(void)
{
    const uint8_t *tx_bytes;

    if(bDeviceState == SUSPENDED)
    {
        return;
    }
    if(bDeviceState != CONFIGURED)
    {
        cdc_clear_pending();
        return;
    }

    while(g_cdc_rx_pending && g_cdc_rx_delivered < g_cdc_rx_length)
    {
        if(!MipsRuntime_UartReceive(g_cdc_rx_packet[g_cdc_rx_delivered]))
        {
            break;
        }
        ++g_cdc_rx_delivered;
    }
    if(g_cdc_rx_pending && g_cdc_rx_delivered == g_cdc_rx_length)
    {
        g_cdc_rx_pending = 0u;
        g_cdc_rx_length = 0u;
        g_cdc_rx_delivered = 0u;
        SetEPRxValid(ENDP2);
    }

    if(!USBD_Endp3_Busy)
    {
        size_t amount = MipsRuntime_UartTxPeek(&tx_bytes);
        if(amount > CDC_PACKET_SIZE)
        {
            amount = CDC_PACKET_SIZE;
        }
        if(amount != 0u && tx_bytes != NULL)
        {
            memset(g_cdc_tx_packet, 0, sizeof(g_cdc_tx_packet));
            memcpy(g_cdc_tx_packet, tx_bytes, amount);
            USB_SIL_Write(EP3_IN, g_cdc_tx_packet, (uint32_t)amount);
            USBD_Endp3_Busy = 1u;
            SetEPTxValid(ENDP3);
            (void)MipsRuntime_UartTxConsume(amount);
        }
    }
}
