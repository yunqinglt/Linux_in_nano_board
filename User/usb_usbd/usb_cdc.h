#ifndef CH32V203F6P_USB_CDC_H
#define CH32V203F6P_USB_CDC_H

#include <stddef.h>
#include <stdint.h>

/* 初始化 CDC ACM 设备，并在主循环中搬运固件日志和 guest UART 数据。 */
uint8_t UsbCdc_Init(void);
void UsbCdc_Service(void);

/*
 * 将固件日志放入独立的 SRAM 环形队列；该队列与 guest 16550 FIFO 分离，
 * 因而启动阶段的诊断信息不会被模拟器串口数据覆盖。
 */
size_t UsbCdc_LogWrite(const uint8_t *data, size_t length);

#endif
