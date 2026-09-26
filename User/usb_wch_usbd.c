#include "usb_vendor_port.h"

#include <stddef.h>

#include "debug.h"
#include "usb_lib.h"
#include "usb_desc.h"
#include "usb_istr.h"
#include "usb_pwr.h"

static __attribute__((aligned(4))) uint8_t g_ep1_rx_buffer[USB_VENDOR_EP_PACKET_SIZE];

/* WCH legacy USBD 栈通过这些全局对象处理 EP0 标准请求。 */
DEVICE Device_Table = { EP_NUM, 1u };

static uint8_t *usbd_get_device_descriptor(uint16_t length);
static uint8_t *usbd_get_config_descriptor(uint16_t length);
static uint8_t *usbd_get_string_descriptor(uint16_t length);
static RESULT usbd_data_setup(uint8_t request);
static RESULT usbd_no_data_setup(uint8_t request);
static RESULT usbd_get_interface_setting(uint8_t interface,
                                         uint8_t alternate_setting);
static void usbd_init(void);
static void usbd_reset(void);
static void usbd_set_configuration(void);
static void usbd_set_interface(void);
static void usbd_clear_feature(void);
static void usbd_set_address(void);
static void usbd_reset_ep1(void);
static void usbd_apply_ep1_configuration(uint8_t configured);

static ONE_DESCRIPTOR g_device_descriptor = {
    (uint8_t *)USBD_DeviceDescriptor,
    USBD_SIZE_DEVICE_DESC
};
static ONE_DESCRIPTOR g_config_descriptor = {
    (uint8_t *)USBD_ConfigDescriptor,
    USBD_SIZE_CONFIG_DESC
};
static ONE_DESCRIPTOR g_string_descriptors[4] = {
    { (uint8_t *)USBD_StringLangID, USBD_SIZE_STRING_LANGID },
    { (uint8_t *)USBD_StringVendor, USBD_SIZE_STRING_VENDOR },
    { (uint8_t *)USBD_StringProduct, USBD_SIZE_STRING_PRODUCT },
    { (uint8_t *)USBD_StringSerial, USBD_SIZE_STRING_SERIAL }
};

DEVICE_PROP Device_Property = {
    usbd_init,
    usbd_reset,
    NOP_Process,
    NOP_Process,
    usbd_data_setup,
    usbd_no_data_setup,
    usbd_get_interface_setting,
    usbd_get_device_descriptor,
    usbd_get_config_descriptor,
    usbd_get_string_descriptor,
    0,
    DEF_USBD_UEP0_SIZE
};

USER_STANDARD_REQUESTS User_Standard_Requests = {
    NOP_Process,             /* 读取配置 */
    usbd_set_configuration,  /* 设置配置 */
    NOP_Process,             /* 读取接口 */
    usbd_set_interface,      /* 设置接口 */
    NOP_Process,             /* 读取状态 */
    usbd_clear_feature,      /* 清除端点特性 */
    NOP_Process,             /* 设置端点特性 */
    NOP_Process,             /* 设置设备特性 */
    usbd_set_address         /* 设置地址 */
};

const uint8_t USBD_DeviceDescriptor[USBD_SIZE_DEVICE_DESC] = {
    0x12, USB_DEVICE_DESCRIPTOR_TYPE,
    0x00, 0x02,
    0x00, 0x00, 0x00,
    DEF_USBD_UEP0_SIZE,
    (uint8_t)(USB_VENDOR_VID & 0xFFu),
    (uint8_t)(USB_VENDOR_VID >> 8),
    (uint8_t)(USB_VENDOR_PID & 0xFFu),
    (uint8_t)(USB_VENDOR_PID >> 8),
    0x00, 0x01,
    0x01, 0x02, 0x03,
    0x01
};

const uint8_t USBD_ConfigDescriptor[USBD_SIZE_CONFIG_DESC] = {
    0x09, USB_CONFIGURATION_DESCRIPTOR_TYPE,
    0x20, 0x00,
    0x01, 0x01, 0x00,
    0x80, 0x32,

    0x09, 0x04,
    0x00, 0x00, 0x02,
    0xFF, 0x53, 0x01,
    0x00,

    0x07, 0x05,
    USB_VENDOR_EP_OUT, 0x02,
    USB_VENDOR_EP_PACKET_SIZE, 0x00,
    0x00,

    0x07, 0x05,
    USB_VENDOR_EP_IN, 0x02,
    USB_VENDOR_EP_PACKET_SIZE, 0x00,
    0x00
};

const uint8_t USBD_StringLangID[USBD_SIZE_STRING_LANGID] = {
    USBD_SIZE_STRING_LANGID, USB_STRING_DESCRIPTOR_TYPE, 0x09, 0x04
};

const uint8_t USBD_StringVendor[USBD_SIZE_STRING_VENDOR] = {
    USBD_SIZE_STRING_VENDOR, USB_STRING_DESCRIPTOR_TYPE,
    'Y', 0, 'u', 0, 'n', 0, 'q', 0, 'i', 0, 'n', 0,
    'g', 0, ' ', 0, 'L', 0, 'a', 0, 'b', 0
};

const uint8_t USBD_StringProduct[USBD_SIZE_STRING_PRODUCT] = {
    USBD_SIZE_STRING_PRODUCT, USB_STRING_DESCRIPTOR_TYPE,
    'C', 0, 'H', 0, '3', 0, '2', 0, 'V', 0, '2', 0, '0', 0, '3', 0,
    ' ', 0, 'F', 0, 'l', 0, 'a', 0, 's', 0, 'h', 0, ' ', 0,
    'B', 0, 'r', 0, 'i', 0, 'd', 0, 'g', 0, 'e', 0
};

uint8_t USBD_StringSerial[USBD_SIZE_STRING_SERIAL] = {
    USBD_SIZE_STRING_SERIAL, USB_STRING_DESCRIPTOR_TYPE,
    'C', 0, 'H', 0, '3', 0, '2', 0, 'V', 0, '2', 0, '0', 0, '3', 0,
    'F', 0, '6', 0, 'P', 0, '6', 0
};

/* 根据当前系统时钟选择稳定的 48 MHz USBD 时钟，并打开 APB1 外设时钟。 */
static uint8_t usbd_configure_clock(void)
{
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);
    if(clocks.SYSCLK_Frequency == 48000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div1);
    }
    else if(clocks.SYSCLK_Frequency == 96000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div2);
    }
    else if(clocks.SYSCLK_Frequency == 144000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3);
    }
    else
    {
        return 0u;
    }

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);
    return 1u;
}

/* 初始化 WCH USBD/PMA 栈，EP0 由官方核心接管。 */
usb_vendor_status_t UsbWchUsbd_Init(void)
{
    if(!usbd_configure_clock())
    {
        return USB_VENDOR_STATUS_INVALID_ARGUMENT;
    }

    USB_Init();
    USB_Interrupts_Config();
    return USB_VENDOR_STATUS_OK;
}

/*
 * 暂停 USB 低优先级中断，保护主循环与 USBD IRQ 共享的协议/PMA 状态。
 * 这里只屏蔽 USB IRQ，不影响串口和 SPI；返回值用于恢复进入前的使能状态。
 */
uint8_t UsbWchUsbd_EnterCritical(void)
{
    uint8_t interrupt_was_enabled =
        NVIC_GetStatusIRQ(USB_LP_CAN1_RX0_IRQn) != 0u;

    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    return interrupt_was_enabled;
}

/* 恢复短临界区之前的 USB IRQ 使能状态，兼容嵌套调用。 */
void UsbWchUsbd_ExitCritical(uint8_t interrupt_was_enabled)
{
    if(interrupt_was_enabled)
    {
        NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    }
}

/* 兼容 WCH 栈预留的硬件时钟配置入口。 */
void Set_USBConfig(void)
{
    (void)usbd_configure_clock();
}

/* 记录 USB 挂起状态；维护固件保持 CPU 运行以服务串口。 */
void Enter_LowPowerMode(void)
{
    bDeviceState = SUSPENDED;
}

/* USB 唤醒后依据当前配置值恢复公开设备状态。 */
void Leave_LowPowerMode(void)
{
    bDeviceState = (Device_Info.Current_Configuration != 0u) ?
                   CONFIGURED : ATTACHED;
}

/* 配置 USBD 低优先级事务中断和 EXTI18 唤醒中断。 */
void USB_Interrupts_Config(void)
{
    NVIC_InitTypeDef nvic;
    EXTI_InitTypeDef exti;

    EXTI_ClearITPendingBit(EXTI_Line18);
    exti.EXTI_Line = EXTI_Line18;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising_Falling;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);

    nvic.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1u;
    nvic.NVIC_IRQChannelSubPriority = 0u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    nvic.NVIC_IRQChannel = USBWakeUp_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 0u;
    NVIC_Init(&nvic);
}

/* 控制 PA11/PA12 断开/接入以及芯片内部 D+ 上拉。 */
void USB_Port_Set(FunctionalState new_state, FunctionalState pin_in_ipu)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    if(new_state != DISABLE)
    {
        _SetCNTR(_GetCNTR() & (uint16_t)~CNTR_PDWN);
        GPIOA->CFGHR &= 0xFFF00FFFu;
        GPIOA->OUTDR &= ~(3u << 11);
        GPIOA->CFGHR |= 0x00044000u;
    }
    else
    {
        _SetCNTR(_GetCNTR() | CNTR_PDWN);
        GPIOA->CFGHR &= 0xFFF00FFFu;
        GPIOA->OUTDR &= ~(3u << 11);
        GPIOA->CFGHR |= 0x00033000u;
    }

    if(pin_in_ipu != DISABLE)
    {
        EXTEN->EXTEN_CTR |= EXTEN_USBD_PU_EN;
    }
    else
    {
        EXTEN->EXTEN_CTR &= ~EXTEN_USBD_PU_EN;
    }
}

/* 使用 VALID/NAK 为 EP1 OUT 提供单包背压。 */
void UsbWchUsbd_SetOutEnabled(uint8_t enabled)
{
    if(enabled && Device_Info.Current_Configuration == 1u)
    {
        SetEPRxCount(ENDP1, USB_VENDOR_EP_PACKET_SIZE);
        SetEPRxStatus(ENDP1, EP_RX_VALID);
    }
    else
    {
        SetEPRxStatus(ENDP1, EP_RX_NAK);
    }
}

/*
 * 将一个不超过 64 字节的包复制到 PMA 并启动 EP1 IN。
 * 调用者在 USB IRQ 短临界区内使用；配置失效或端点非空闲时返回失败。
 */
uint8_t UsbWchUsbd_SendPacket(const uint8_t *data, uint8_t length)
{
    if(data == NULL || length > USB_VENDOR_EP_PACKET_SIZE ||
       Device_Info.Current_Configuration != 1u ||
       _GetEPTxStatus(ENDP1) != EP_TX_NAK)
    {
        return 0u;
    }

    (void)USB_SIL_Write(EP1_IN, (uint8_t *)data, length);
    SetEPTxStatus(ENDP1, EP_TX_VALID);
    return 1u;
}

/* 向 EP0 核心提供设备描述符片段。 */
static uint8_t *usbd_get_device_descriptor(uint16_t length)
{
    return Standard_GetDescriptorData(length, &g_device_descriptor);
}

/* 向 EP0 核心提供配置描述符片段。 */
static uint8_t *usbd_get_config_descriptor(uint16_t length)
{
    return Standard_GetDescriptorData(length, &g_config_descriptor);
}

/* 按 SETUP 包中的索引选择 UTF-16LE 字符串描述符。 */
static uint8_t *usbd_get_string_descriptor(uint16_t length)
{
    uint8_t index = pInformation->USBwValue0;

    if(index >= 4u)
    {
        return NULL;
    }
    return Standard_GetDescriptorData(length, &g_string_descriptors[index]);
}

/* 本协议只使用 Bulk 端点，拒绝额外的类/厂商控制数据请求。 */
static RESULT usbd_data_setup(uint8_t request)
{
    (void)request;
    return USB_UNSUPPORT;
}

/* 本协议没有类/厂商无数据控制请求。 */
static RESULT usbd_no_data_setup(uint8_t request)
{
    (void)request;
    return USB_UNSUPPORT;
}

/* 设备仅支持接口 0 的备用设置 0。 */
static RESULT usbd_get_interface_setting(uint8_t interface,
                                         uint8_t alternate_setting)
{
    return (interface == 0u && alternate_setting == 0u) ?
           USB_SUCCESS : USB_UNSUPPORT;
}

/* 复位 USBD 核心并通过 20 ms 物理断开确保主机重新枚举。 */
static void usbd_init(void)
{
    uint8_t endpoint;

    pInformation->Current_Configuration = 0u;
    PowerOn();
    for(endpoint = 0u; endpoint < 8u; ++endpoint)
    {
        _SetENDPOINT(endpoint,
                     _GetENDPOINT(endpoint) & 0x7F7Fu & EPREG_MASK);
    }
    _SetISTR(0x00FFu);
    USB_SIL_Init();
    bDeviceState = UNCONNECTED;

    USB_Port_Set(DISABLE, DISABLE);
    Delay_Ms(20u);
    USB_Port_Set(ENABLE, ENABLE);
}

/* 在总线复位后建立 EP0 和双向 Bulk EP1 的 PMA 布局。 */
static void usbd_reset(void)
{
    pInformation->Current_Configuration = 0u;
    pInformation->Current_Feature = USBD_ConfigDescriptor[7];
    pInformation->Current_Interface = 0u;

    SetBTABLE(BTABLE_ADDRESS);

    SetEPType(ENDP0, EP_CONTROL);
    SetEPTxStatus(ENDP0, EP_TX_STALL);
    SetEPRxAddr(ENDP0, ENDP0_RXADDR);
    SetEPTxAddr(ENDP0, ENDP0_TXADDR);
    Clear_Status_Out(ENDP0);
    SetEPRxCount(ENDP0, DEF_USBD_UEP0_SIZE);
    SetEPRxStatus(ENDP0, EP_RX_VALID);
    _ClearDTOG_RX(ENDP0);
    _ClearDTOG_TX(ENDP0);

    usbd_reset_ep1();

    SetDeviceAddress(0u);
    UsbVendor_OnBusReset();
    bDeviceState = ATTACHED;
}

/*
 * 集中复位 EP1：必须先令双向端点 NAK，再清 TX/RX DTOG 和旧计数。
 * SET_CONFIGURATION、SET_INTERFACE、CLEAR_FEATURE 与总线复位共用此路径。
 */
static void usbd_reset_ep1(void)
{
    SetEPTxStatus(ENDP1, EP_TX_NAK);
    SetEPRxStatus(ENDP1, EP_RX_NAK);
    _ClearEP_CTR_TX(ENDP1);
    _ClearEP_CTR_RX(ENDP1);
    _ClearDTOG_TX(ENDP1);
    _ClearDTOG_RX(ENDP1);

    SetEPType(ENDP1, EP_BULK);
    SetEPTxAddr(ENDP1, ENDP1_TXADDR);
    SetEPRxAddr(ENDP1, ENDP1_RXADDR);
    SetEPTxCount(ENDP1, 0u);
    SetEPRxCount(ENDP1, USB_VENDOR_EP_PACKET_SIZE);
}

/*
 * 原子应用配置边界：端点先复位，随后使软件 generation 失效旧请求，
 * 最后仅在配置 1 下重新开放 OUT。重复 SET_CONFIGURATION(1) 也会完整复位。
 */
static void usbd_apply_ep1_configuration(uint8_t configured)
{
    usbd_reset_ep1();
    UsbVendor_OnConfigurationChanged(configured);
    if(configured)
    {
        UsbWchUsbd_SetOutEnabled(1u);
    }
}

/* 以 EP0 的 SET_CONFIGURATION 结果驱动协议层收发开关。 */
static void usbd_set_configuration(void)
{
    uint8_t configured = pInformation->Current_Configuration == 1u;

    usbd_apply_ep1_configuration(configured);
    bDeviceState = configured ? CONFIGURED : ADDRESSED;
}

/* SET_INTERFACE(0, alt 0) 同样建立新的 EP1 数据切换边界。 */
static void usbd_set_interface(void)
{
    usbd_apply_ep1_configuration(1u);
    bDeviceState = CONFIGURED;
}

/* 清除端点 HALT 后丢弃半帧，避免发送旧 PMA 数据。 */
static void usbd_clear_feature(void)
{
    if((pInformation->USBwIndex0 & 0x7Fu) == ENDP1)
    {
        usbd_apply_ep1_configuration(
            pInformation->Current_Configuration == 1u);
    }
}

/* 标准 SET_ADDRESS 完成后更新设备状态。 */
static void usbd_set_address(void)
{
    bDeviceState = ADDRESSED;
}

/* EP1 IN 完成回调只更新状态，下一包由主循环发送。 */
void UsbWchUsbd_EP1_IN_Callback(void)
{
    UsbVendor_OnInComplete();
}

/* EP1 OUT 回调从 PMA 取包并交给协议队列，不在 IRQ 内访问 SPI。 */
void UsbWchUsbd_EP1_OUT_Callback(void)
{
    uint32_t length = USB_SIL_Read(EP1_OUT, g_ep1_rx_buffer);

    SetEPRxStatus(ENDP1, EP_RX_NAK);
    if(length <= USB_VENDOR_EP_PACKET_SIZE)
    {
        UsbVendor_OnOutPacket(g_ep1_rx_buffer, (uint8_t)length);
    }
}

void USBWakeUp_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* 清除 USBD 唤醒对应的 EXTI18 挂起位。 */
void USBWakeUp_IRQHandler(void)
{
    EXTI_ClearITPendingBit(EXTI_Line18);
}

/* 将芯片 USBD IRQ36 交给 WCH 官方事务分派器。 */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    USB_Istr();
}
