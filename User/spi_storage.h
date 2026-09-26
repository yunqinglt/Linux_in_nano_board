#ifndef SPI_STORAGE_H
#define SPI_STORAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_STORAGE_NOR_CAPACITY       (8u * 1024u * 1024u)
#define SPI_STORAGE_PSRAM_CAPACITY     (8u * 1024u * 1024u)

/* SPI 存储操作的通用返回状态，不依赖维护固件的 USB 协议。 */
typedef enum
{
    SPI_STORAGE_STATUS_OK = 0,
    SPI_STORAGE_STATUS_INVALID_ARGUMENT,
    SPI_STORAGE_STATUS_OUT_OF_RANGE,
    SPI_STORAGE_STATUS_IO_ERROR
} spi_storage_status_t;

/* 初始化阶段缓存的芯片容量、在线状态及硬件 ID。 */
typedef struct
{
    uint32_t nor_size;
    uint32_t psram_size;
    uint8_t nor_id[3];
    uint8_t psram_id[4];
    uint8_t nor_present;
    uint8_t psram_present;
} spi_storage_info_t;

/*
 * pin-configuration.txt 所述板级 SPI1 存储接口：
 * PA0=W25Q64 CS，PA1=APS6404L CS，PA5/6/7=SCK/MISO/MOSI。
 */
spi_storage_status_t SpiStorage_Init(void);
void SpiStorage_GetInfo(spi_storage_info_t *info);

/* 下轮 boot/MIPS 引擎使用的通用流式访问接口。 */
spi_storage_status_t SpiStorage_NorRead(uint32_t address,
                                       uint8_t *data,
                                       uint32_t length);
spi_storage_status_t SpiStorage_PsramRead(uint32_t address,
                                         uint8_t *data,
                                         uint32_t length);
spi_storage_status_t SpiStorage_PsramWrite(uint32_t address,
                                          const uint8_t *data,
                                          uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* SPI_STORAGE_H */
