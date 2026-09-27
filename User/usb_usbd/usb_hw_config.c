#include "hw_config.h"

#include "usb_istr.h"
#include "usb_lib.h"
#include "usb_pwr.h"

void USBWakeUp_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* 由当前系统时钟分频得到 USBD 所需的 48 MHz 时钟。 */
void Set_USBConfig(void)
{
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    if(clocks.SYSCLK_Frequency == 96000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div2);
    }
    else if(clocks.SYSCLK_Frequency == 48000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div1);
    }
    else if(clocks.SYSCLK_Frequency == 144000000u)
    {
        RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3);
    }
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);
}

/* USB 低功耗状态不触碰 USART2 命令行。 */
void Enter_LowPowerMode(void)
{
    bDeviceState = SUSPENDED;
}

void Leave_LowPowerMode(void)
{
    bDeviceState = Device_Info.Current_Configuration != 0u ?
                   CONFIGURED : ATTACHED;
}

/* 启用 USBD 低优先级中断和 USB 唤醒 EXTI18。 */
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

/* 按 CH32V20x USBD 要求切换 PA11/PA12 和内部上拉。 */
void USB_Port_Set(FunctionalState new_state, FunctionalState pin_in_ipu)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    if(new_state != DISABLE)
    {
        _SetCNTR(_GetCNTR() & ~(1u << 1));
        GPIOA->CFGHR &= 0xfff00fffu;
        GPIOA->OUTDR &= ~(3u << 11);
        GPIOA->CFGHR |= 0x00044000u;
    }
    else
    {
        _SetCNTR(_GetCNTR() | (1u << 1));
        GPIOA->CFGHR &= 0xfff00fffu;
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

/* 清除 USBD 唤醒产生的 EXTI18 挂起标志。 */
void USBWakeUp_IRQHandler(void)
{
    EXTI_ClearITPendingBit(EXTI_Line18);
}

/* 将 USBD/PMA 事务分发给 WCH 官方设备栈。 */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    USB_Istr();
}
