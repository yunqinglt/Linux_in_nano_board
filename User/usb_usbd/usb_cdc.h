#ifndef CH32V203F6P_USB_CDC_H
#define CH32V203F6P_USB_CDC_H

#include <stdint.h>

/* 初始化 CDC ACM 设备，并在主循环中搬运 guest UART 数据。 */
uint8_t UsbCdc_Init(void);
void UsbCdc_Service(void);

#endif
