#ifndef SPI_STORAGE_H
#define SPI_STORAGE_H

#include "usb_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * pin-configuration.txt 所述板级 SPI1 存储后端：
 * PA0=W25Q64 CS，PA1=APS6404L CS，PA5/6/7=SCK/MISO/MOSI。
 */
usb_vendor_status_t SpiStorage_Init(void);
const usb_vendor_backend_t *SpiStorage_Backend(void);
void *SpiStorage_Context(void);
void SpiStorage_GetInfo(usb_vendor_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* SPI_STORAGE_H */
