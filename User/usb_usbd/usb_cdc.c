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
#define CDC_LOG_BUFFER_SIZE 2048u

/* WCH PMA 奇数长度拷贝会访问最后一个额外字节，缓冲区预留哨兵位。 */
static uint8_t g_cdc_rx_packet[CDC_PACKET_SIZE + 1u];
static uint8_t g_cdc_tx_packet[CDC_PACKET_SIZE + 1u];
/* 固件日志独立于 guest UART，USB 尚未枚举时也可以先缓存。 */
static uint8_t g_cdc_log_buffer[CDC_LOG_BUFFER_SIZE];
static volatile uint16_t g_cdc_log_head;
static volatile uint16_t g_cdc_log_tail;
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

/* 计算环形日志队列中从当前位置开始的连续字节数。 */
static uint16_t cdc_log_contiguous_length(void)
{
    if(g_cdc_log_tail <= g_cdc_log_head)
    {
        return (uint16_t)(g_cdc_log_head - g_cdc_log_tail);
    }
    return (uint16_t)(CDC_LOG_BUFFER_SIZE - g_cdc_log_tail);
}

/* 将一段固件日志写入 SRAM 环形队列；队列满时丢弃最旧字节。 */
size_t UsbCdc_LogWrite(const uint8_t *data, size_t length)
{
    size_t index;

    if(data == NULL && length != 0u)
    {
        return 0u;
    }
    for(index = 0u; index < length; ++index)
    {
        uint16_t next = (uint16_t)((g_cdc_log_head + 1u) %
                                   CDC_LOG_BUFFER_SIZE);
        g_cdc_log_buffer[g_cdc_log_head] = data[index];
        g_cdc_log_head = next;
        if(g_cdc_log_head == g_cdc_log_tail)
        {
            /* 保留一个空槽以区分 full/empty 状态。 */
            g_cdc_log_tail = (uint16_t)((g_cdc_log_tail + 1u) %
                                        CDC_LOG_BUFFER_SIZE);
        }
    }
    return length;
}

/* 初始化 WCH 官方 USBD/PMA 栈和 CDC 端点。 */
uint8_t UsbCdc_Init(void)
{
    cdc_clear_pending();
    g_cdc_log_head = 0u;
    g_cdc_log_tail = 0u;
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
    uint16_t log_length;

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
        /* 固件日志优先发送，队列耗尽后再转发 guest 16550 输出。 */
        log_length = cdc_log_contiguous_length();
        if(log_length != 0u)
        {
            size_t amount = log_length;
            if(amount > CDC_PACKET_SIZE)
            {
                amount = CDC_PACKET_SIZE;
            }
            memcpy(g_cdc_tx_packet,
                   &g_cdc_log_buffer[g_cdc_log_tail], amount);
            USB_SIL_Write(EP3_IN, g_cdc_tx_packet, (uint32_t)amount);
            USBD_Endp3_Busy = 1u;
            SetEPTxValid(ENDP3);
            g_cdc_log_tail = (uint16_t)((g_cdc_log_tail + amount) %
                                        CDC_LOG_BUFFER_SIZE);
        }
        else
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
}
