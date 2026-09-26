#include "spi_storage.h"

#include <stddef.h>
#include <string.h>

#include "debug.h"

#define STORAGE_CS_NOR              GPIO_Pin_0
#define STORAGE_CS_PSRAM            GPIO_Pin_1
#define STORAGE_SPI_TIMEOUT         100000u

#define W25_CMD_READ_DATA           0x03u
#define W25_CMD_JEDEC_ID            0x9Fu

#define APS_CMD_READ_DATA           0x03u
#define APS_CMD_WRITE_DATA          0x02u
#define APS_CMD_READ_ID             0x9Fu
#define APS_CMD_RESET_ENABLE        0x66u
#define APS_CMD_RESET               0x99u

typedef struct
{
    spi_storage_info_t info;
} spi_storage_context_t;

static spi_storage_context_t g_storage;

/* 带超时等待 SPI 硬件标志达到目标状态。 */
static uint8_t spi_wait(uint16_t flag, FlagStatus state)
{
    uint32_t timeout = STORAGE_SPI_TIMEOUT;

    while(SPI_I2S_GetFlagStatus(SPI1, flag) != state)
    {
        if(timeout-- == 0u)
        {
            return 0u;
        }
    }
    return 1u;
}

/* 完成一次全双工字节传输并返回接收值。 */
static uint8_t spi_transfer(uint8_t transmitted, uint8_t *received)
{
    if(received == NULL || !spi_wait(SPI_I2S_FLAG_TXE, SET))
    {
        return 0u;
    }
    SPI_I2S_SendData(SPI1, transmitted);
    if(!spi_wait(SPI_I2S_FLAG_RXNE, SET))
    {
        return 0u;
    }
    *received = (uint8_t)SPI_I2S_ReceiveData(SPI1);
    return 1u;
}

/* 发送一个字节并丢弃同时收到的数据。 */
static uint8_t spi_send(uint8_t value)
{
    uint8_t ignored;
    return spi_transfer(value, &ignored);
}

/* 等待移位器空闲，确保拉高片选前命令已经结束。 */
static uint8_t spi_finish(void)
{
    return spi_wait(SPI_I2S_FLAG_BSY, RESET);
}

/* 先释放两颗芯片，再单独拉低目标片选。 */
static void storage_select(uint16_t chip_select)
{
    GPIO_SetBits(GPIOA, STORAGE_CS_NOR | STORAGE_CS_PSRAM);
    GPIO_ResetBits(GPIOA, chip_select);
}

/* 拉高目标片选并结束当前 SPI 命令。 */
static void storage_deselect(uint16_t chip_select)
{
    GPIO_SetBits(GPIOA, chip_select);
}

/* 以高字节优先顺序发送 24 位存储地址。 */
static uint8_t storage_send_address(uint32_t address)
{
    return (uint8_t)(spi_send((uint8_t)(address >> 16)) &&
                     spi_send((uint8_t)(address >> 8)) &&
                     spi_send((uint8_t)address));
}

/* 以防溢出的方式检查地址区间是否落在芯片容量内。 */
static uint8_t storage_range_valid(uint32_t address,
                                   uint32_t length,
                                   uint32_t capacity)
{
    return (uint8_t)(address <= capacity && length <= capacity - address);
}

/* 判断 ID 总线是否呈现全 00 或全 FF 的悬空特征。 */
static uint8_t storage_id_is_floating(const uint8_t *id, uint32_t length)
{
    uint32_t index;
    uint8_t all_zero = 1u;
    uint8_t all_ff = 1u;

    for(index = 0u; index < length; ++index)
    {
        if(id[index] != 0x00u)
        {
            all_zero = 0u;
        }
        if(id[index] != 0xFFu)
        {
            all_ff = 0u;
        }
    }
    return (uint8_t)(all_zero || all_ff);
}

/* 读取 W25Q64 的三字节 JEDEC ID。 */
static uint8_t w25_read_id(uint8_t id[3])
{
    uint8_t index;
    uint8_t ok;

    storage_select(STORAGE_CS_NOR);
    ok = spi_send(W25_CMD_JEDEC_ID);
    for(index = 0u; index < 3u && ok; ++index)
    {
        ok = spi_transfer(0xFFu, &id[index]);
    }
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(STORAGE_CS_NOR);
    return ok;
}

/* 按 APS6404L 时序执行复位使能和复位命令。 */
static uint8_t aps_reset(void)
{
    uint8_t ok;

    storage_select(STORAGE_CS_PSRAM);
    ok = spi_send(APS_CMD_RESET_ENABLE);
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(STORAGE_CS_PSRAM);
    if(!ok)
    {
        return 0u;
    }

    storage_select(STORAGE_CS_PSRAM);
    ok = spi_send(APS_CMD_RESET);
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(STORAGE_CS_PSRAM);
    Delay_Ms(1u);
    return ok;
}

/* 读取 APS6404L 制造商、KGD 和扩展 ID 前缀。 */
static uint8_t aps_read_id(uint8_t id[4])
{
    uint8_t ignored;
    uint8_t index;
    uint8_t ok;

    storage_select(STORAGE_CS_PSRAM);
    ok = spi_send(APS_CMD_READ_ID);
    for(index = 0u; index < 3u && ok; ++index)
    {
        ok = spi_transfer(0x00u, &ignored);
    }
    for(index = 0u; index < 4u && ok; ++index)
    {
        ok = spi_transfer(0x00u, &id[index]);
    }
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(STORAGE_CS_PSRAM);
    return ok;
}

/* 在指定片选上执行带 24 位地址的顺序读命令。 */
static spi_storage_status_t storage_read(uint16_t chip_select,
                                         uint32_t capacity,
                                         uint32_t address,
                                         uint8_t *data,
                                         uint32_t length)
{
    uint32_t index;
    uint8_t ok;

    if(data == NULL && length != 0u)
    {
        return SPI_STORAGE_STATUS_INVALID_ARGUMENT;
    }
    if(!storage_range_valid(address, length, capacity))
    {
        return SPI_STORAGE_STATUS_OUT_OF_RANGE;
    }
    if(length == 0u)
    {
        return SPI_STORAGE_STATUS_OK;
    }

    storage_select(chip_select);
    ok = (uint8_t)(spi_send(chip_select == STORAGE_CS_NOR ?
                            W25_CMD_READ_DATA : APS_CMD_READ_DATA) &&
                   storage_send_address(address));
    for(index = 0u; index < length && ok; ++index)
    {
        ok = spi_transfer(0xFFu, &data[index]);
    }
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(chip_select);
    return ok ? SPI_STORAGE_STATUS_OK : SPI_STORAGE_STATUS_IO_ERROR;
}

/* 初始化 SPI1、片选 GPIO，并探测两颗存储芯片。 */
spi_storage_status_t SpiStorage_Init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;
    uint8_t nor_id[3] = {0u, 0u, 0u};
    uint8_t psram_id[4] = {0u, 0u, 0u, 0u};
    uint8_t nor_ok;
    uint8_t psram_ok;

    memset(&g_storage, 0, sizeof(g_storage));
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO |
                           RCC_APB2Periph_GPIOA |
                           RCC_APB2Periph_SPI1,
                           ENABLE);

    gpio.GPIO_Pin = STORAGE_CS_NOR | STORAGE_CS_PSRAM;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_SetBits(GPIOA, STORAGE_CS_NOR | STORAGE_CS_PSRAM);

    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    SPI_StructInit(&spi);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    /* 96 MHz PCLK2 / 8 = 12 MHz，对两颗芯片和当前飞线均保守可靠。 */
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(SPI1, &spi);
    SPI_NSSInternalSoftwareConfig(SPI1, SPI_NSSInternalSoft_Set);
    SPI_Cmd(SPI1, ENABLE);

    nor_ok = w25_read_id(nor_id);
    psram_ok = (uint8_t)(aps_reset() && aps_read_id(psram_id));

    g_storage.info.nor_size = SPI_STORAGE_NOR_CAPACITY;
    g_storage.info.psram_size = SPI_STORAGE_PSRAM_CAPACITY;
    memcpy(g_storage.info.nor_id, nor_id, sizeof(nor_id));
    memcpy(g_storage.info.psram_id, psram_id, sizeof(psram_id));
    g_storage.info.nor_present =
        (uint8_t)(nor_ok && !storage_id_is_floating(nor_id, sizeof(nor_id)));
    g_storage.info.psram_present =
        (uint8_t)(psram_ok && !storage_id_is_floating(psram_id, 2u));

    /* 芯片缺席属于探测结果而非控制器初始化失败。 */
    return SPI_STORAGE_STATUS_OK;
}

/* 复制当前存储探测结果供串口或后续 boot 逻辑使用。 */
void SpiStorage_GetInfo(spi_storage_info_t *info)
{
    if(info != NULL)
    {
        memcpy(info, &g_storage.info, sizeof(*info));
    }
}

/* 从固定产品镜像所在的 W25Q64 顺序读取数据。 */
spi_storage_status_t SpiStorage_NorRead(uint32_t address,
                                       uint8_t *data,
                                       uint32_t length)
{
    if(!g_storage.info.nor_present)
    {
        return SPI_STORAGE_STATUS_IO_ERROR;
    }
    return storage_read(STORAGE_CS_NOR,
                        SPI_STORAGE_NOR_CAPACITY,
                        address,
                        data,
                        length);
}

/* 从 APS6404L 顺序读取数据，供 MIPS 内存后端使用。 */
spi_storage_status_t SpiStorage_PsramRead(uint32_t address,
                                         uint8_t *data,
                                         uint32_t length)
{
    if(!g_storage.info.psram_present)
    {
        return SPI_STORAGE_STATUS_IO_ERROR;
    }
    return storage_read(STORAGE_CS_PSRAM,
                        SPI_STORAGE_PSRAM_CAPACITY,
                        address,
                        data,
                        length);
}

/* 向 APS6404L 顺序写入数据，供 MIPS 内存后端使用。 */
spi_storage_status_t SpiStorage_PsramWrite(uint32_t address,
                                          const uint8_t *data,
                                          uint32_t length)
{
    uint32_t index;
    uint8_t ok;

    if(data == NULL && length != 0u)
    {
        return SPI_STORAGE_STATUS_INVALID_ARGUMENT;
    }
    if(!g_storage.info.psram_present)
    {
        return SPI_STORAGE_STATUS_IO_ERROR;
    }
    if(!storage_range_valid(address, length, SPI_STORAGE_PSRAM_CAPACITY))
    {
        return SPI_STORAGE_STATUS_OUT_OF_RANGE;
    }
    if(length == 0u)
    {
        return SPI_STORAGE_STATUS_OK;
    }

    storage_select(STORAGE_CS_PSRAM);
    ok = (uint8_t)(spi_send(APS_CMD_WRITE_DATA) &&
                   storage_send_address(address));
    for(index = 0u; index < length && ok; ++index)
    {
        ok = spi_send(data[index]);
    }
    if(ok)
    {
        ok = spi_finish();
    }
    storage_deselect(STORAGE_CS_PSRAM);
    return ok ? SPI_STORAGE_STATUS_OK : SPI_STORAGE_STATUS_IO_ERROR;
}
