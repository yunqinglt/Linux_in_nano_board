#ifndef CH32V203F6P_MIPS_RUNTIME_H
#define CH32V203F6P_MIPS_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    MIPS_RUNTIME_STATUS_OK = 0,
    MIPS_RUNTIME_STATUS_NOT_INITIALIZED,
    MIPS_RUNTIME_STATUS_ALREADY_RUNNING,
    MIPS_RUNTIME_STATUS_FLASH_IO,
    MIPS_RUNTIME_STATUS_BAD_MANIFEST,
    MIPS_RUNTIME_STATUS_PAYLOAD_CRC,
    MIPS_RUNTIME_STATUS_KERNEL_LOAD,
    MIPS_RUNTIME_STATUS_DTB_LOAD,
    MIPS_RUNTIME_STATUS_INITRAMFS_LOAD,
    MIPS_RUNTIME_STATUS_DTB_UPDATE
} mips_runtime_status_t;

/* 安装 APS6404L 后端并为处理器保留静态寄存器上下文。 */
uint8_t MipsRuntime_Init(void);

/* 校验镜像并装载 Linux；成功时返回内核 ELF 入口地址。 */
mips_runtime_status_t MipsRuntime_Boot(uint32_t *entry_address);

/* 主循环按小时间片运行 guest，期间允许轮询 USART 与 USB CDC。 */
void MipsRuntime_RunSlice(void);
uint8_t MipsRuntime_IsRunning(void);
const char *MipsRuntime_StatusString(mips_runtime_status_t status);
uint8_t MipsRuntime_UartReceive(uint8_t byte);
size_t MipsRuntime_UartTxPeek(const uint8_t **bytes);
uint8_t MipsRuntime_UartTxConsume(size_t length);

#endif
