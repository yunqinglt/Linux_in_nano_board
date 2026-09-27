#include "mips_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mipsel_emu.h"
#include "serial_log.h"
#include "spi_storage.h"

#define IMAGE_SIZE              0x00800000u
#define MANIFEST_SIZE           0x00000100u
#define MANIFEST_CRC_OFFSET     0x0000001cu
#define IMAGE_ENTRY_BASE        0x00000040u
#define IMAGE_ENTRY_SIZE        0x00000040u
#define IMAGE_CHUNK_SIZE        128u
#define PSRAM_CACHE_LINE_SIZE   32u
#define PSRAM_CACHE_LINE_COUNT  4u
#define MIPS_RUN_SLICE_STEPS    32u
#define BOOT_PROGRESS_INTERVAL  (256u * 1024u)
#define ELF32_HEADER_SIZE       52u
#define ELF32_PROGRAM_HEADER_SIZE 32u
#define ELF32_PT_LOAD            1u

typedef struct
{
    uint32_t offset;
    uint32_t slot_size;
    uint32_t data_size;
    uint32_t crc32;
} mips_runtime_image_entry_t;

typedef struct
{
    uint32_t address;
    uint8_t valid;
    uint8_t bytes[PSRAM_CACHE_LINE_SIZE];
} mips_runtime_cache_line_t;

/* 寄存器、镜像块、缓存和 ELF 区间均常驻 SRAM，避免压入 2 KiB 栈。 */
static Registers g_mips_registers;
static Registers *g_mips_registers_pointer = &g_mips_registers;
static mipsel_elf_info_t g_kernel_info;
static mipsel_memory_range_t g_reserved_ranges[MIPSEL_EMU_ELF_MAX_LOAD_SEGMENTS + 1u];
static mips_runtime_image_entry_t g_image_entries[3];
static mips_runtime_cache_line_t g_psram_cache[PSRAM_CACHE_LINE_COUNT];
static uint8_t g_manifest[MANIFEST_SIZE];
static uint8_t g_image_chunk[IMAGE_CHUNK_SIZE];
static uint8_t g_fill_chunk[256];
static uint8_t g_runtime_initialized;
static uint8_t g_runtime_running;
static uint8_t g_psram_io_fault;
static uint32_t g_psram_fault_address;
static uint32_t g_dtb_size;
static mips_runtime_progress_callback_t g_progress_callback;
static mips_runtime_progress_stage_t g_progress_stage;
static uint32_t g_progress_total;
static uint32_t g_progress_done;
static uint32_t g_progress_next_report;

/* 开始一个加载阶段，并立即给板级日志一个可见的 0% 事件。 */
static void runtime_progress_begin(mips_runtime_progress_stage_t stage,
                                   uint32_t total)
{
    g_progress_stage = stage;
    g_progress_total = total;
    g_progress_done = 0u;
    g_progress_next_report = BOOT_PROGRESS_INTERVAL;
    if(g_progress_callback != NULL)
    {
        g_progress_callback(stage, 0u, total);
    }
}

/* 累计底层读写进度，并限制回调频率以免占满 SRAM 日志队列。 */
static void runtime_progress_add(uint32_t amount)
{
    uint32_t next_done;

    if(g_progress_callback == NULL)
    {
        return;
    }
    next_done = g_progress_done + amount;
    if(next_done < g_progress_done)
    {
        next_done = UINT32_MAX;
    }
    if(g_progress_total != 0u && next_done > g_progress_total)
    {
        next_done = g_progress_total;
    }
    g_progress_done = next_done;
    if(g_progress_done >= g_progress_next_report ||
       (g_progress_total != 0u && g_progress_done == g_progress_total))
    {
        g_progress_callback(g_progress_stage, g_progress_done,
                            g_progress_total);
        if(g_progress_done <= UINT32_MAX - BOOT_PROGRESS_INTERVAL)
        {
            g_progress_next_report = g_progress_done +
                                      BOOT_PROGRESS_INTERVAL;
        }
        else
        {
            g_progress_next_report = UINT32_MAX;
        }
    }
}

/* 强制发出一个阶段完成事件，即使该阶段实际只写入少量字节。 */
static void runtime_progress_end(mips_runtime_progress_stage_t stage,
                                 uint32_t total)
{
    g_progress_stage = stage;
    g_progress_total = total;
    g_progress_done = total;
    g_progress_next_report = UINT32_MAX;
    if(g_progress_callback != NULL)
    {
        g_progress_callback(stage, total, total);
    }
}

/* 将通用 image loader 错误映射为产品可操作的 Flash/PSRAM 故障。 */
static mips_runtime_status_t runtime_map_image_status(
    mipsel_image_status_t image_status,
    mips_runtime_status_t fallback)
{
    if(image_status == MIPSEL_IMAGE_SOURCE_IO)
    {
        return MIPS_RUNTIME_STATUS_FLASH_IO;
    }
    if(image_status == MIPSEL_IMAGE_MEMORY_IO)
    {
        return MIPS_RUNTIME_STATUS_PSRAM_IO;
    }
    return fallback;
}

/* 按 manifest 規定的 little-endian 格式读取 16 位整数。 */
static uint16_t image_read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* 按 manifest 規定的 little-endian 格式读取 32 位整数。 */
static uint32_t image_read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* 采用与 zlib.crc32 一致的反射 CRC-32/ISO-HDLC 流式更新规则。 */
static uint32_t image_crc32_update(uint32_t previous,
                                   const uint8_t *bytes,
                                   size_t length)
{
    uint32_t crc = ~previous;
    size_t index;

    for(index = 0u; index < length; ++index)
    {
        uint8_t bit;
        crc ^= bytes[index];
        for(bit = 0u; bit < 8u; ++bit)
        {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

/* 从 APS6404L 同步读取；小型直映缓存合并连续取指的 4 字节请求。 */
static bool psram_memory_read(void *opaque, uint32_t pa,
                              void *destination, size_t length)
{
    uint8_t *output = (uint8_t *)destination;

    (void)opaque;
    if((destination == NULL && length != 0u) || length > UINT32_MAX)
    {
        return false;
    }

    while(length != 0u)
    {
        uint32_t line_address = pa & ~(PSRAM_CACHE_LINE_SIZE - 1u);
        uint32_t line_index =
            (line_address / PSRAM_CACHE_LINE_SIZE) % PSRAM_CACHE_LINE_COUNT;
        mips_runtime_cache_line_t *line = &g_psram_cache[line_index];
        uint32_t within_line = pa - line_address;
        size_t amount = PSRAM_CACHE_LINE_SIZE - within_line;

        if(!line->valid || line->address != line_address)
        {
            if(SpiStorage_PsramRead(line_address, line->bytes,
                                    PSRAM_CACHE_LINE_SIZE) !=
               SPI_STORAGE_STATUS_OK)
            {
                g_psram_io_fault = 1u;
                g_psram_fault_address = pa;
                return false;
            }
            line->address = line_address;
            line->valid = 1u;
        }

        if(amount > length)
        {
            amount = length;
        }
        memcpy(output, &line->bytes[within_line], amount);
        output += amount;
        pa += (uint32_t)amount;
        length -= amount;
    }
    return true;
}

/* 将 APS6404L 的状态码映射为平台 backend 的布尔结果并失效重叠缓存。 */
static bool psram_memory_write(void *opaque, uint32_t pa,
                               const void *source, size_t length)
{
    uint64_t end_address = (uint64_t)pa + length;
    uint32_t index;

    (void)opaque;
    if((source == NULL && length != 0u) || length > UINT32_MAX)
    {
        return false;
    }
    if(length == 0u)
    {
        return true;
    }
    if(SpiStorage_PsramWrite(pa, (const uint8_t *)source,
                             (uint32_t)length) != SPI_STORAGE_STATUS_OK)
    {
        g_psram_io_fault = 1u;
        g_psram_fault_address = pa;
        return false;
    }

    for(index = 0u; index < PSRAM_CACHE_LINE_COUNT; ++index)
    {
        mips_runtime_cache_line_t *line = &g_psram_cache[index];
        uint64_t line_end = (uint64_t)line->address + PSRAM_CACHE_LINE_SIZE;
        if(line->valid && (uint64_t)pa < line_end &&
           (uint64_t)line->address < end_address)
        {
            line->valid = 0u;
        }
    }
    if(g_progress_stage == MIPS_RUNTIME_PROGRESS_PT_LOAD ||
       g_progress_stage == MIPS_RUNTIME_PROGRESS_DTB ||
       g_progress_stage == MIPS_RUNTIME_PROGRESS_INITRAMFS)
    {
        runtime_progress_add((uint32_t)length);
    }
    return true;
}

/* 用固定 SRAM 块向 APS6404L 分批填充 ELF 的 BSS 区域。 */
static bool psram_memory_fill(void *opaque, uint32_t pa,
                              uint8_t value, size_t length)
{
    (void)opaque;
    if(length > UINT32_MAX)
    {
        return false;
    }
    memset(g_fill_chunk, value, sizeof(g_fill_chunk));

    while(length != 0u)
    {
        uint32_t amount = (length > sizeof(g_fill_chunk)) ?
                          (uint32_t)sizeof(g_fill_chunk) : (uint32_t)length;
        if(!psram_memory_write(NULL, pa, g_fill_chunk, amount))
        {
            return false;
        }
        pa += amount;
        length -= amount;
    }
    return true;
}

static const platform_memory_ops_t g_psram_memory_ops = {
    .read = psram_memory_read,
    .write = psram_memory_write,
    .fill = psram_memory_fill,
};

/* 比较固定 manifest 名称，并要求其余字节为 NUL 填充。 */
static uint8_t image_name_matches(const uint8_t *field, const char *name)
{
    size_t index;
    size_t length = strlen(name);

    if(length > 15u)
    {
        return 0u;
    }
    for(index = 0u; index < 16u; ++index)
    {
        uint8_t expected = index < length ? (uint8_t)name[index] : 0u;
        if(field[index] != expected)
        {
            return 0u;
        }
    }
    return 1u;
}

/* 读取并验证 manifest 头、固定槽位、边界和自身 CRC。 */
static mips_runtime_status_t image_manifest_load(void)
{
    static const uint8_t magic[8] = {'M', 'I', 'P', 'S', 'I', 'M', 'G', 0};
    static const char *const names[3] = {"vmlinux", "dtb", "initramfs"};
    static const uint32_t offsets[3] = {0x010000u, 0x610000u, 0x620000u};
    static const uint32_t slots[3] = {0x600000u, 0x010000u, 0x1e0000u};
    uint16_t header_size;
    uint16_t version;
    uint32_t expected_crc;
    uint32_t index;

    if(SpiStorage_NorRead(0u, g_manifest, sizeof(g_manifest)) !=
       SPI_STORAGE_STATUS_OK)
    {
        return MIPS_RUNTIME_STATUS_FLASH_IO;
    }
    if(memcmp(g_manifest, magic, sizeof(magic)) != 0)
    {
        return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
    }

    version = image_read_le16(&g_manifest[0x08]);
    header_size = image_read_le16(&g_manifest[0x0a]);
    if(version != 1u || header_size != MANIFEST_SIZE ||
       image_read_le32(&g_manifest[0x0c]) != IMAGE_SIZE ||
       image_read_le32(&g_manifest[0x10]) != 0x1000u ||
       image_read_le32(&g_manifest[0x14]) != 0x1000u ||
       image_read_le32(&g_manifest[0x18]) != 3u)
    {
        return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
    }
    for(index = 0x24u; index < 0x40u; ++index)
    {
        if(g_manifest[index] != 0u)
        {
            return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
        }
    }

    expected_crc = image_read_le32(&g_manifest[MANIFEST_CRC_OFFSET]);
    memset(&g_manifest[MANIFEST_CRC_OFFSET], 0, 4u);
    if(image_crc32_update(0u, g_manifest, header_size) != expected_crc)
    {
        return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
    }

    for(index = 0u; index < 3u; ++index)
    {
        const uint8_t *entry =
            &g_manifest[IMAGE_ENTRY_BASE + index * IMAGE_ENTRY_SIZE];
        uint32_t offset = image_read_le32(&entry[0x10]);
        uint32_t slot_size = image_read_le32(&entry[0x14]);
        uint32_t data_size = image_read_le32(&entry[0x18]);
        uint32_t end_address;

        if(!image_name_matches(entry, names[index]) ||
           offset != offsets[index] || slot_size != slots[index] ||
           data_size == 0u || data_size > slot_size || offset > IMAGE_SIZE ||
           slot_size > IMAGE_SIZE - offset)
        {
            return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
        }
        end_address = offset + data_size;
        if(end_address < offset)
        {
            return MIPS_RUNTIME_STATUS_BAD_MANIFEST;
        }

        g_image_entries[index].offset = offset;
        g_image_entries[index].slot_size = slot_size;
        g_image_entries[index].data_size = data_size;
        g_image_entries[index].crc32 = image_read_le32(&entry[0x1c]);
    }
    return MIPS_RUNTIME_STATUS_OK;
}

/* 对三个有效 payload 分别流式计算 CRC32，不读取 slot 中的擦除填充。 */
static mips_runtime_status_t image_payloads_verify(void)
{
    uint32_t entry_index;

    for(entry_index = 0u; entry_index < 3u; ++entry_index)
    {
        uint32_t address = g_image_entries[entry_index].offset;
        uint32_t remaining = g_image_entries[entry_index].data_size;
        uint32_t crc = 0u;

        while(remaining != 0u)
        {
            uint32_t amount = remaining > sizeof(g_image_chunk) ?
                              (uint32_t)sizeof(g_image_chunk) : remaining;
            if(SpiStorage_NorRead(address, g_image_chunk, amount) !=
               SPI_STORAGE_STATUS_OK)
            {
                return MIPS_RUNTIME_STATUS_FLASH_IO;
            }
            crc = image_crc32_update(crc, g_image_chunk, amount);
            address += amount;
            remaining -= amount;
            runtime_progress_add(amount);
        }
        if(crc != g_image_entries[entry_index].crc32)
        {
            return MIPS_RUNTIME_STATUS_PAYLOAD_CRC;
        }
    }
    return MIPS_RUNTIME_STATUS_OK;
}

/* 将 loader 的文件视图限制在单个 manifest payload 的 data_size 内。 */
static bool payload_image_read(void *opaque, uint32_t offset,
                               void *destination, size_t length)
{
    const mips_runtime_image_entry_t *entry =
        (const mips_runtime_image_entry_t *)opaque;

    if(entry == NULL || (destination == NULL && length != 0u) ||
       length > UINT32_MAX || offset > entry->data_size ||
       length > entry->data_size - offset)
    {
        return false;
    }
    if(length == 0u)
    {
        return true;
    }
    return SpiStorage_NorRead(entry->offset + offset,
                              (uint8_t *)destination, (uint32_t)length) ==
           SPI_STORAGE_STATUS_OK;
}

/* 预扫描 ELF program header，得到所有 PT_LOAD 的实际 PSRAM 写入总量。 */
static uint32_t image_pt_load_memory_total(const mipsel_image_t *image)
{
    uint8_t header[ELF32_HEADER_SIZE];
    uint8_t program_header[ELF32_PROGRAM_HEADER_SIZE];
    uint32_t program_header_offset;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint64_t program_header_end;
    uint32_t total = 0u;
    uint16_t index;

    if(image == NULL || image->read == NULL || image->size < sizeof(header) ||
       !image->read(image->opaque, 0u, header, sizeof(header)) ||
       header[0] != 0x7fu || header[1] != 'E' || header[2] != 'L' ||
       header[3] != 'F')
    {
        return 0u;
    }
    program_header_offset = image_read_le32(&header[28]);
    program_header_size = image_read_le16(&header[42]);
    program_header_count = image_read_le16(&header[44]);
    program_header_end = (uint64_t)program_header_offset +
                         (uint64_t)program_header_size *
                         (uint64_t)program_header_count;
    if(program_header_size < ELF32_PROGRAM_HEADER_SIZE ||
       program_header_offset > image->size ||
       program_header_end > image->size)
    {
        return 0u;
    }
    for(index = 0u; index < program_header_count; ++index)
    {
        uint32_t offset = program_header_offset +
                          (uint32_t)index * program_header_size;
        uint32_t segment_size;
        if(!image->read(image->opaque, offset, program_header,
                        sizeof(program_header)))
        {
            return 0u;
        }
        if(image_read_le32(&program_header[0]) != ELF32_PT_LOAD)
        {
            continue;
        }
        segment_size = image_read_le32(&program_header[20]);
        if(segment_size > UINT32_MAX - total)
        {
            return 0u;
        }
        total += segment_size;
    }
    return total;
}

/* 为 ELF/FDT loader 构造指向固定 manifest 项的只读镜像视图。 */
static mipsel_image_t payload_as_image(uint32_t index)
{
    mipsel_image_t image;
    image.opaque = &g_image_entries[index];
    image.size = g_image_entries[index].data_size;
    image.read = payload_image_read;
    return image;
}

/* 注册 PSRAM backend；平台宽度读写器会统一转入这些 byte-range 回调。 */
uint8_t MipsRuntime_Init(void)
{
    if(!platform_memory_configure(&g_psram_memory_ops, NULL,
                                  SPI_STORAGE_PSRAM_CAPACITY))
    {
        return 0u;
    }
    memset(g_psram_cache, 0, sizeof(g_psram_cache));
    platform_init(NULL, NULL);
    platform_reset();
    reset_cpu(g_mips_registers_pointer);
    g_runtime_initialized = 1u;
    g_runtime_running = 0u;
    return 1u;
}

/* 安装或清除产品层的启动进度回调。 */
void MipsRuntime_SetProgressCallback(mips_runtime_progress_callback_t callback)
{
    g_progress_callback = callback;
}

/* 按 manifest 顺序校验、加载 ELF/DTB/initramfs，再设置 Linux handoff。 */
mips_runtime_status_t MipsRuntime_Boot(uint32_t *entry_address)
{
    mips_runtime_status_t status;
    mipsel_image_status_t image_status;
    mipsel_image_t kernel_image;
    mipsel_image_t dtb_image;
    mipsel_image_t initramfs_image;
    uint32_t index;
    size_t range_count;
    mipsel_memory_range_t dtb_reservation;
    mipsel_memory_range_t initramfs_range;
    uint32_t pt_load_total;

    if(!g_runtime_initialized)
    {
        return MIPS_RUNTIME_STATUS_NOT_INITIALIZED;
    }
    if(g_runtime_running)
    {
        return MIPS_RUNTIME_STATUS_ALREADY_RUNNING;
    }

    g_psram_io_fault = 0u;
    memset(g_psram_cache, 0, sizeof(g_psram_cache));
    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_MANIFEST, 1u);
    status = image_manifest_load();
    if(status != MIPS_RUNTIME_STATUS_OK)
    {
        return status;
    }
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_MANIFEST, 1u);

    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_VERIFY,
                           g_image_entries[0].data_size +
                           g_image_entries[1].data_size +
                           g_image_entries[2].data_size);
    status = image_payloads_verify();
    if(status != MIPS_RUNTIME_STATUS_OK)
    {
        return status;
    }
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_VERIFY,
                         g_image_entries[0].data_size +
                         g_image_entries[1].data_size +
                         g_image_entries[2].data_size);

    kernel_image = payload_as_image(0u);
    dtb_image = payload_as_image(1u);
    initramfs_image = payload_as_image(2u);
    dtb_reservation.start = MIPSEL_EMU_DTB_PHYSICAL_ADDRESS;
    dtb_reservation.end = MIPSEL_EMU_DTB_PHYSICAL_ADDRESS +
                          MIPSEL_EMU_DTB_RESERVED_SIZE;
    pt_load_total = image_pt_load_memory_total(&kernel_image);
    if(pt_load_total == 0u)
    {
        pt_load_total = kernel_image.size;
    }
    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_PT_LOAD, pt_load_total);
    image_status = mipsel_elf_load(&kernel_image, &dtb_reservation, 1u,
                                   &g_kernel_info);
    status = image_status == MIPSEL_IMAGE_OK ? MIPS_RUNTIME_STATUS_OK :
             runtime_map_image_status(image_status,
                                      MIPS_RUNTIME_STATUS_KERNEL_LOAD);
    if(status != MIPS_RUNTIME_STATUS_OK)
    {
        if(g_psram_io_fault)
        {
            return MIPS_RUNTIME_STATUS_PSRAM_IO;
        }
        return status;
    }
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_PT_LOAD,
                         pt_load_total);

    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_DTB, dtb_image.size);
    image_status = mipsel_dtb_load(&dtb_image,
                                   MIPSEL_EMU_DTB_PHYSICAL_ADDRESS,
                                   MIPSEL_EMU_DTB_RESERVED_SIZE,
                                   &g_dtb_size);
    if(image_status != MIPSEL_IMAGE_OK)
    {
        return runtime_map_image_status(image_status,
                                        MIPS_RUNTIME_STATUS_DTB_LOAD);
    }
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_DTB, dtb_image.size);

    if(g_kernel_info.load_segment_count > MIPSEL_EMU_ELF_MAX_LOAD_SEGMENTS)
    {
        return MIPS_RUNTIME_STATUS_KERNEL_LOAD;
    }
    range_count = g_kernel_info.load_segment_count;
    for(index = 0u; index < range_count; ++index)
    {
        g_reserved_ranges[index].start =
            g_kernel_info.load_segments[index].start;
        g_reserved_ranges[index].end =
            g_kernel_info.load_segments[index].end;
    }
    g_reserved_ranges[range_count++] = dtb_reservation;
    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_INITRAMFS,
                           initramfs_image.size);
    image_status = mipsel_initramfs_load(&initramfs_image, g_reserved_ranges,
                                         range_count,
                                         MIPSEL_EMU_INITRAMFS_ALIGNMENT,
                                         &initramfs_range);
    if(image_status != MIPSEL_IMAGE_OK)
    {
        return runtime_map_image_status(image_status,
                                        MIPS_RUNTIME_STATUS_INITRAMFS_LOAD);
    }
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_INITRAMFS,
                         initramfs_image.size);

    runtime_progress_begin(MIPS_RUNTIME_PROGRESS_HANDOFF, 1u);
    image_status = mipsel_fdt_set_initramfs(
        MIPSEL_EMU_DTB_PHYSICAL_ADDRESS, g_dtb_size,
        initramfs_range.start, initramfs_range.end);
    if(image_status != MIPSEL_IMAGE_OK)
    {
        return runtime_map_image_status(image_status,
                                        MIPS_RUNTIME_STATUS_DTB_UPDATE);
    }

    memset(g_psram_cache, 0, sizeof(g_psram_cache));
    platform_reset();
    linux_load_reset(g_mips_registers_pointer);
    g_mips_registers_pointer->pc = g_kernel_info.entry;
    g_mips_registers_pointer->next_pc = g_kernel_info.entry + 4u;
    g_mips_registers_pointer->gpr[4] = 0xfffffffeu;
    g_mips_registers_pointer->gpr[5] = MIPSEL_EMU_DTB_VIRTUAL_ADDRESS;
    g_runtime_running = 1u;
    runtime_progress_end(MIPS_RUNTIME_PROGRESS_HANDOFF, 1u);
    if(entry_address != NULL)
    {
        *entry_address = g_kernel_info.entry;
    }
    return MIPS_RUNTIME_STATUS_OK;
}

/* 运行一个短 guest 时间片；遇到 APS6404L 错误时停机并保留故障地址。 */
void MipsRuntime_RunSlice(void)
{
    if(!g_runtime_running)
    {
        return;
    }
    (void)mipsel_emu_run_steps(g_mips_registers_pointer,
                               MIPS_RUN_SLICE_STEPS);
    if(g_psram_io_fault)
    {
        g_runtime_running = 0u;
        Firmware_Printf("MIPS stopped: APS6404L I/O error at PA=%08lx PC=%08lx\r\n",
                        (unsigned long)g_psram_fault_address,
                        (unsigned long)g_mips_registers_pointer->pc);
    }
}

/* 返回当前 guest 状态，避免控制台 probe 重置运行中的 PSRAM。 */
uint8_t MipsRuntime_IsRunning(void)
{
    return g_runtime_running;
}

/* 将板级启动错误转换成可直接显示在 USART 命令行的说明。 */
const char *MipsRuntime_StatusString(mips_runtime_status_t status)
{
    switch(status)
    {
        case MIPS_RUNTIME_STATUS_OK:
            return "ok";
        case MIPS_RUNTIME_STATUS_NOT_INITIALIZED:
            return "runtime not initialized; APS6404L was not detected";
        case MIPS_RUNTIME_STATUS_ALREADY_RUNNING:
            return "guest is already running";
        case MIPS_RUNTIME_STATUS_FLASH_IO:
            return "W25Q64 read failed";
        case MIPS_RUNTIME_STATUS_BAD_MANIFEST:
            return "manifest format or CRC is invalid";
        case MIPS_RUNTIME_STATUS_PAYLOAD_CRC:
            return "payload CRC32 mismatch";
        case MIPS_RUNTIME_STATUS_KERNEL_LOAD:
            return "MIPS ELF load failed";
        case MIPS_RUNTIME_STATUS_DTB_LOAD:
            return "DTB load failed";
        case MIPS_RUNTIME_STATUS_INITRAMFS_LOAD:
            return "initramfs load failed";
        case MIPS_RUNTIME_STATUS_DTB_UPDATE:
            return "DTB has no writable initramfs properties";
        case MIPS_RUNTIME_STATUS_PSRAM_IO:
            return "APS6404L PSRAM read/write failed; check hardware";
        default:
            return "unknown runtime error";
    }
}

/* 将 CDC 收到的字节注入模拟 16550A 的接收 FIFO。 */
uint8_t MipsRuntime_UartReceive(uint8_t byte)
{
    return (uint8_t)platform_uart_receive(byte);
}

/* 提供 guest UART 待发送数据的连续缓冲视图。 */
size_t MipsRuntime_UartTxPeek(const uint8_t **bytes)
{
    return platform_uart_tx_peek(bytes);
}

/* 确认 CDC 已同步复制 PMA 数据后消费 guest UART 字节。 */
uint8_t MipsRuntime_UartTxConsume(size_t length)
{
    return (uint8_t)platform_uart_tx_consume(length);
}
