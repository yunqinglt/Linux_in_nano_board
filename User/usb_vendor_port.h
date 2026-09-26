#ifndef USB_VENDOR_PORT_H
#define USB_VENDOR_PORT_H

#include <stdint.h>

#include "usb_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB 栈与协议状态机之间的内部适配接口。 */
usb_vendor_status_t UsbWchUsbd_Init(void);
uint8_t UsbWchUsbd_EnterCritical(void);
void UsbWchUsbd_ExitCritical(uint8_t interrupt_was_enabled);
void UsbWchUsbd_SetOutEnabled(uint8_t enabled);
uint8_t UsbWchUsbd_SendPacket(const uint8_t *data, uint8_t length);

void UsbVendor_OnBusReset(void);
void UsbVendor_OnConfigurationChanged(uint8_t configured);
void UsbVendor_OnInComplete(void);
void UsbVendor_OnOutPacket(const uint8_t *data, uint8_t length);

#ifdef __cplusplus
}
#endif

#endif
