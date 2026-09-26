#ifndef USB_USBD_DESC_H
#define USB_USBD_DESC_H

#include <stdint.h>

#define USB_DEVICE_DESCRIPTOR_TYPE  0x01u
#define USB_CONFIGURATION_DESCRIPTOR_TYPE 0x02u
#define USB_STRING_DESCRIPTOR_TYPE 0x03u

#define DEF_USBD_UEP0_SIZE          64u
#define DEF_USBD_MAX_PACK_SIZE      64u

#define USBD_SIZE_DEVICE_DESC       18u
#define USBD_SIZE_CONFIG_DESC       32u
#define USBD_SIZE_STRING_LANGID     4u
#define USBD_SIZE_STRING_VENDOR     24u
#define USBD_SIZE_STRING_PRODUCT    44u
#define USBD_SIZE_STRING_SERIAL     26u

extern const uint8_t USBD_DeviceDescriptor[USBD_SIZE_DEVICE_DESC];
extern const uint8_t USBD_ConfigDescriptor[USBD_SIZE_CONFIG_DESC];
extern const uint8_t USBD_StringLangID[USBD_SIZE_STRING_LANGID];
extern const uint8_t USBD_StringVendor[USBD_SIZE_STRING_VENDOR];
extern const uint8_t USBD_StringProduct[USBD_SIZE_STRING_PRODUCT];
extern uint8_t USBD_StringSerial[USBD_SIZE_STRING_SERIAL];

#endif
