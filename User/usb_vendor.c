#include "usb_vendor.h"

#include <stddef.h>
#include <string.h>

#include "usb_vendor_port.h"

#define USB_VENDOR_REQUEST_SIZE       16u
#define USB_VENDOR_RESPONSE_SIZE      16u
#define USB_VENDOR_INFO_SIZE          32u

#define USB_VENDOR_REQ_MAGIC_0        ((uint8_t)'M')
#define USB_VENDOR_REQ_MAGIC_1        ((uint8_t)'V')
#define USB_VENDOR_REQ_MAGIC_2        ((uint8_t)'C')
#define USB_VENDOR_REQ_MAGIC_3        ((uint8_t)'H')
#define USB_VENDOR_RSP_MAGIC_0        ((uint8_t)'M')
#define USB_VENDOR_RSP_MAGIC_1        ((uint8_t)'V')
#define USB_VENDOR_RSP_MAGIC_2        ((uint8_t)'R')
#define USB_VENDOR_RSP_MAGIC_3        ((uint8_t)'S')

typedef enum
{
    PROTOCOL_IDLE = 0,
    PROTOCOL_RX_WRITE,
    PROTOCOL_RESPONSE
} protocol_phase_t;

typedef enum
{
    TX_NONE = 0,
    TX_HEADER_READY,
    TX_HEADER_WAIT,
    TX_PAYLOAD_READY,
    TX_PAYLOAD_WAIT
} tx_phase_t;

typedef struct
{
    uint8_t opcode;
    uint16_t sequence;
    uint32_t address;
    uint32_t length;
} request_t;

static const usb_vendor_backend_t *g_backend;
static void *g_backend_context;
static usb_vendor_info_t g_info;

static uint8_t g_out_packet[USB_VENDOR_EP_PACKET_SIZE];
static uint8_t g_transfer_buffer[USB_VENDOR_MAX_TRANSFER];
static uint8_t g_response_header[USB_VENDOR_RESPONSE_SIZE];

static volatile uint8_t g_configured;
static volatile uint8_t g_out_ready;
static volatile uint8_t g_out_length;
static volatile uint8_t g_in_busy;
static volatile uint8_t g_in_complete;
static volatile uint32_t g_bus_generation;

static protocol_phase_t g_protocol_phase;
static tx_phase_t g_tx_phase;
static usb_vendor_state_t g_public_state;
static request_t g_active_request;
static uint32_t g_transfer_offset;
static uint32_t g_response_payload_length;
static uint32_t g_response_payload_offset;
static uint32_t g_write_crc;
static uint32_t g_last_processed;

/* 从协议缓冲区读取 16 位小端整数。 */
static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

/* 从协议缓冲区读取 32 位小端整数。 */
static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/* 向协议缓冲区写入 16 位小端整数。 */
static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

/* 向协议缓冲区写入 32 位小端整数。 */
static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

/* 按 IEEE 多项式增量更新 CRC32。 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data,
                             uint32_t length)
{
    uint32_t i;
    uint8_t bit;

    for(i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for(bit = 0; bit < 8u; ++bit)
        {
            if(crc & 1u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* 计算一段连续数据的完整 CRC32。 */
static uint32_t crc32_calculate(const uint8_t *data, uint32_t length)
{
    return crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

/* 将后端越界状态值折叠成协议内部错误。 */
static usb_vendor_status_t normalize_status(usb_vendor_status_t status)
{
    if((unsigned int)status > (unsigned int)USB_VENDOR_STATUS_INTERNAL_ERROR)
    {
        return USB_VENDOR_STATUS_INTERNAL_ERROR;
    }
    return status;
}

/* 丢弃半帧和待发送数据，回到空闲协议状态。 */
static void protocol_reset(void)
{
    g_protocol_phase = PROTOCOL_IDLE;
    g_tx_phase = TX_NONE;
    g_public_state = USB_VENDOR_STATE_IDLE;
    g_out_ready = 0u;
    g_out_length = 0u;
    g_in_busy = 0u;
    g_in_complete = 0u;
    g_transfer_offset = 0u;
    g_response_payload_length = 0u;
    g_response_payload_offset = 0u;
    g_write_crc = 0xFFFFFFFFu;
    g_last_processed = 0u;
}

/* 临界区内判断一次软件状态是否仍属于同一 USB 配置世代。 */
static uint8_t transport_is_current_locked(uint32_t generation)
{
    return g_configured && g_bus_generation == generation;
}

/* 在需要跨越一次短操作时取得稳定的配置世代判断。 */
static uint8_t transport_is_current(uint32_t generation)
{
    uint8_t current;
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    current = transport_is_current_locked(generation);
    UsbWchUsbd_ExitCritical(irq_state);
    return current;
}

/* 临界区内仅在状态机能够接收下一包时重新打开 EP1 OUT。 */
static void endpoint_out_rearm_locked(uint32_t generation)
{
    if(transport_is_current_locked(generation) && !g_out_ready &&
       g_tx_phase == TX_NONE &&
       (g_protocol_phase == PROTOCOL_IDLE ||
        g_protocol_phase == PROTOCOL_RX_WRITE))
    {
        UsbWchUsbd_SetOutEnabled(1u);
    }
}

/* 在 USB IRQ 短临界区内复核 generation 后重新开放 OUT。 */
static void endpoint_out_rearm(uint32_t generation)
{
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    endpoint_out_rearm_locked(generation);
    UsbWchUsbd_ExitCritical(irq_state);
}

/* 在调用 SPI 前把公开状态切为执行中，并确认请求仍属于当前配置。 */
static uint8_t protocol_mark_executing(uint32_t generation)
{
    uint8_t current;
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    current = transport_is_current_locked(generation);
    if(current)
    {
        g_public_state = USB_VENDOR_STATE_EXECUTING;
        g_last_processed = 0u;
    }
    UsbWchUsbd_ExitCritical(irq_state);
    return current;
}

/* 开始解释新请求前清零进度，并拒绝复位边界之前取出的旧包。 */
static uint8_t protocol_prepare_request(uint32_t generation)
{
    uint8_t current;
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    current = transport_is_current_locked(generation) &&
              g_protocol_phase == PROTOCOL_IDLE;
    if(current)
    {
        g_last_processed = 0u;
    }
    UsbWchUsbd_ExitCritical(irq_state);
    return current;
}

/* 仅让同一配置世代提交处理进度，防止复位后的旧请求污染新状态。 */
static void protocol_set_progress(uint32_t generation, uint32_t processed)
{
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    if(transport_is_current_locked(generation))
    {
        g_last_processed = processed;
    }
    UsbWchUsbd_ExitCritical(irq_state);
}

/* 复核 generation 后构造固定响应头并切换到响应发送阶段。 */
static uint8_t response_begin(uint32_t generation, uint16_t sequence,
                           usb_vendor_status_t status,
                           uint32_t value, uint32_t payload_length)
{
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    if(!transport_is_current_locked(generation))
    {
        UsbWchUsbd_ExitCritical(irq_state);
        return 0u;
    }

    g_response_header[0] = USB_VENDOR_RSP_MAGIC_0;
    g_response_header[1] = USB_VENDOR_RSP_MAGIC_1;
    g_response_header[2] = USB_VENDOR_RSP_MAGIC_2;
    g_response_header[3] = USB_VENDOR_RSP_MAGIC_3;
    g_response_header[4] = USB_VENDOR_PROTOCOL_VERSION;
    g_response_header[5] = (uint8_t)normalize_status(status);
    write_le16(&g_response_header[6], sequence);
    write_le32(&g_response_header[8], value);
    write_le32(&g_response_header[12], payload_length);

    UsbWchUsbd_SetOutEnabled(0u);
    g_protocol_phase = PROTOCOL_RESPONSE;
    g_public_state = USB_VENDOR_STATE_RESPONDING;
    g_response_payload_length = payload_length;
    g_response_payload_offset = 0u;
    g_in_complete = 0u;
    g_tx_phase = TX_HEADER_READY;
    UsbWchUsbd_ExitCritical(irq_state);
    return 1u;
}

/* 请求 USBD/PMA 适配层发送一个 Bulk IN 包。 */
static uint8_t tx_start(const uint8_t *data, uint8_t length)
{
    g_in_complete = 0u;
    if(!UsbWchUsbd_SendPacket(data, length))
    {
        return 0u;
    }

    g_in_busy = 1u;
    return 1u;
}

/* 在主循环中依次发送响应头和可选负载。 */
static void tx_service(void)
{
    uint32_t remaining;
    uint8_t length;
    uint32_t generation;
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    if(!g_configured)
    {
        UsbWchUsbd_ExitCritical(irq_state);
        return;
    }
    generation = g_bus_generation;

    switch(g_tx_phase)
    {
        case TX_HEADER_READY:
            if(!g_in_busy &&
               tx_start(g_response_header, USB_VENDOR_RESPONSE_SIZE))
            {
                g_tx_phase = TX_HEADER_WAIT;
            }
            break;

        case TX_HEADER_WAIT:
            if(g_in_complete)
            {
                g_in_complete = 0u;
                if(g_response_payload_length != 0u)
                {
                    g_tx_phase = TX_PAYLOAD_READY;
                }
                else
                {
                    g_tx_phase = TX_NONE;
                    g_protocol_phase = PROTOCOL_IDLE;
                    g_public_state = USB_VENDOR_STATE_IDLE;
                    endpoint_out_rearm_locked(generation);
                }
            }
            break;

        case TX_PAYLOAD_READY:
            if(!g_in_busy)
            {
                remaining = g_response_payload_length -
                            g_response_payload_offset;
                length = (remaining > USB_VENDOR_EP_PACKET_SIZE) ?
                         USB_VENDOR_EP_PACKET_SIZE : (uint8_t)remaining;
                if(tx_start(&g_transfer_buffer[g_response_payload_offset],
                            length))
                {
                    g_response_payload_offset += length;
                    g_tx_phase = TX_PAYLOAD_WAIT;
                }
            }
            break;

        case TX_PAYLOAD_WAIT:
            if(g_in_complete)
            {
                g_in_complete = 0u;
                if(g_response_payload_offset < g_response_payload_length)
                {
                    g_tx_phase = TX_PAYLOAD_READY;
                }
                else
                {
                    g_tx_phase = TX_NONE;
                    g_protocol_phase = PROTOCOL_IDLE;
                    g_public_state = USB_VENDOR_STATE_IDLE;
                    endpoint_out_rearm_locked(generation);
                }
            }
            break;

        default:
            break;
    }

    UsbWchUsbd_ExitCritical(irq_state);
}

/* 从 SPI 后端刷新容量、几何和芯片 ID。 */
static usb_vendor_status_t refresh_info(void)
{
    usb_vendor_status_t status;

    memset(&g_info, 0, sizeof(g_info));
    status = normalize_status(g_backend->get_info(g_backend_context, &g_info));
    if(status != USB_VENDOR_STATUS_OK)
    {
        return status;
    }

    if(g_info.max_transfer == 0u ||
       g_info.max_transfer > USB_VENDOR_MAX_TRANSFER)
    {
        g_info.max_transfer = USB_VENDOR_MAX_TRANSFER;
    }
    g_info.flags |= USB_VENDOR_INFO_FLAG_CRC32;
    if(g_backend->erase != NULL)
    {
        g_info.flags |= USB_VENDOR_INFO_FLAG_NOR_ERASE;
    }
    return USB_VENDOR_STATUS_OK;
}

/* 按协议规定的小端布局序列化 32 字节 INFO 数据。 */
static void serialize_info(void)
{
    write_le32(&g_transfer_buffer[0], g_info.flash_size);
    write_le32(&g_transfer_buffer[4], g_info.psram_size);
    write_le32(&g_transfer_buffer[8], g_info.erase_size);
    write_le32(&g_transfer_buffer[12], g_info.max_transfer);
    write_le16(&g_transfer_buffer[16], g_info.page_size);
    write_le16(&g_transfer_buffer[18], g_info.flags);
    write_le16(&g_transfer_buffer[20], g_info.fw_major);
    write_le16(&g_transfer_buffer[22], g_info.fw_minor);
    memcpy(&g_transfer_buffer[24], g_info.nor_id, 4u);
    memcpy(&g_transfer_buffer[28], g_info.psram_id, 4u);
}

/* 检查命令范围、长度和目标芯片容量。 */
static usb_vendor_status_t validate_region(usb_vendor_target_t target,
                                           uint32_t address,
                                           uint32_t length,
                                           uint8_t apply_transfer_limit)
{
    uint32_t size = target == USB_VENDOR_TARGET_NOR ?
                    g_info.flash_size : g_info.psram_size;
    uint16_t present_flag = target == USB_VENDOR_TARGET_NOR ?
                            USB_VENDOR_INFO_FLAG_NOR_PRESENT :
                            USB_VENDOR_INFO_FLAG_PSRAM_PRESENT;

    if(length == 0u)
    {
        return USB_VENDOR_STATUS_INVALID_ARGUMENT;
    }
    if(apply_transfer_limit && length > g_info.max_transfer)
    {
        return USB_VENDOR_STATUS_INVALID_ARGUMENT;
    }
    if((g_info.flags & present_flag) == 0u || size == 0u)
    {
        return USB_VENDOR_STATUS_IO_ERROR;
    }
    if(address > size || length > (size - address))
    {
        return USB_VENDOR_STATUS_OUT_OF_RANGE;
    }
    return USB_VENDOR_STATUS_OK;
}

/* 调用后端读取并统一返回状态值。 */
static usb_vendor_status_t backend_read(usb_vendor_target_t target,
                                        uint32_t address,
                                        uint8_t *data,
                                        uint32_t length)
{
    if(g_backend->read == NULL)
    {
        return USB_VENDOR_STATUS_INVALID_OPCODE;
    }
    return normalize_status(g_backend->read(g_backend_context, target,
                                            address, data, length));
}

/* 执行 Flash/PSRAM 读取并准备带 CRC 的响应。 */
static void command_read(const request_t *request,
                         usb_vendor_target_t target,
                         uint32_t generation)
{
    usb_vendor_status_t status;
    uint32_t crc = 0u;

    if(!protocol_mark_executing(generation))
    {
        return;
    }
    status = refresh_info();
    if(status == USB_VENDOR_STATUS_OK)
    {
        status = validate_region(target, request->address, request->length, 1u);
    }
    if(status == USB_VENDOR_STATUS_OK &&
       transport_is_current(generation))
    {
        status = backend_read(target, request->address, g_transfer_buffer,
                              request->length);
        if(status == USB_VENDOR_STATUS_OK)
        {
            crc = crc32_calculate(g_transfer_buffer, request->length);
            protocol_set_progress(generation, request->length);
        }
    }
    (void)response_begin(generation, request->sequence, status, crc,
                         status == USB_VENDOR_STATUS_OK ?
                         request->length : 0u);
}

/* 分块读取指定区域并返回设备端 CRC32。 */
static void command_crc(const request_t *request,
                        usb_vendor_target_t target,
                        uint32_t generation)
{
    usb_vendor_status_t status;
    uint32_t offset = 0u;
    uint32_t chunk;
    uint32_t crc = 0xFFFFFFFFu;

    if(!protocol_mark_executing(generation))
    {
        return;
    }
    status = refresh_info();
    if(status == USB_VENDOR_STATUS_OK)
    {
        status = validate_region(target, request->address, request->length, 0u);
    }
    if(status == USB_VENDOR_STATUS_OK)
    {
        while(offset < request->length &&
              transport_is_current(generation))
        {
            chunk = request->length - offset;
            if(chunk > g_info.max_transfer)
            {
                chunk = g_info.max_transfer;
            }
            status = backend_read(target, request->address + offset,
                                  g_transfer_buffer, chunk);
            if(status != USB_VENDOR_STATUS_OK)
            {
                break;
            }
            crc = crc32_update(crc, g_transfer_buffer, chunk);
            offset += chunk;
            protocol_set_progress(generation, offset);
        }
    }
    (void)response_begin(generation, request->sequence, status,
                         status == USB_VENDOR_STATUS_OK ?
                         (crc ^ 0xFFFFFFFFu) : 0u,
                         0u);
}

/* 校验扇区对齐后执行 NOR 擦除。 */
static void command_erase(const request_t *request, uint32_t generation)
{
    usb_vendor_status_t status;

    if(!protocol_mark_executing(generation))
    {
        return;
    }
    status = refresh_info();
    if(status == USB_VENDOR_STATUS_OK)
    {
        status = validate_region(USB_VENDOR_TARGET_NOR, request->address,
                                 request->length, 0u);
    }
    if(status == USB_VENDOR_STATUS_OK &&
       (g_info.erase_size == 0u ||
        request->length != g_info.erase_size ||
        (request->address % g_info.erase_size) != 0u))
    {
        status = USB_VENDOR_STATUS_INVALID_ARGUMENT;
    }
    if(status == USB_VENDOR_STATUS_OK && g_backend->erase == NULL)
    {
        status = USB_VENDOR_STATUS_INVALID_OPCODE;
    }
    if(status == USB_VENDOR_STATUS_OK &&
       transport_is_current(generation))
    {
        status = normalize_status(g_backend->erase(g_backend_context,
                                                   request->address,
                                                   request->length));
        if(status == USB_VENDOR_STATUS_OK)
        {
            protocol_set_progress(generation, request->length);
        }
    }
    (void)response_begin(generation, request->sequence, status,
                         status == USB_VENDOR_STATUS_OK ? request->length : 0u,
                         0u);
}

/* 进入写入负载接收阶段并初始化增量 CRC。 */
static void command_start_write(const request_t *request,
                                usb_vendor_target_t target,
                                uint32_t generation)
{
    usb_vendor_status_t status;
    uint8_t current;
    uint8_t irq_state;

    if(!protocol_mark_executing(generation))
    {
        return;
    }
    status = refresh_info();
    if(status == USB_VENDOR_STATUS_OK)
    {
        status = validate_region(target, request->address, request->length, 1u);
    }
    if(status == USB_VENDOR_STATUS_OK && g_backend->write == NULL)
    {
        status = USB_VENDOR_STATUS_INVALID_OPCODE;
    }
    if(status != USB_VENDOR_STATUS_OK)
    {
        (void)response_begin(generation, request->sequence, status, 0u, 0u);
        return;
    }

    /* 只在配置仍有效时提交写入会话，避免 reset 后重新打开旧半帧。 */
    irq_state = UsbWchUsbd_EnterCritical();
    current = transport_is_current_locked(generation);
    if(current)
    {
        g_active_request = *request;
        g_transfer_offset = 0u;
        g_last_processed = 0u;
        g_write_crc = 0xFFFFFFFFu;
        g_protocol_phase = PROTOCOL_RX_WRITE;
        g_public_state = USB_VENDOR_STATE_RECEIVING;
    }
    UsbWchUsbd_ExitCritical(irq_state);
}

/* 分派一个经过帧头校验的协议命令。 */
static void process_request(const request_t *request, uint32_t generation)
{
    usb_vendor_status_t status;

    if(!protocol_prepare_request(generation))
    {
        return;
    }
    switch(request->opcode)
    {
        case USB_VENDOR_OPCODE_INFO:
            if(request->address != 0u || request->length != 0u)
            {
                (void)response_begin(generation, request->sequence,
                                     USB_VENDOR_STATUS_INVALID_ARGUMENT,
                                     0u, 0u);
                break;
            }
            if(!protocol_mark_executing(generation))
            {
                break;
            }
            status = refresh_info();
            if(status == USB_VENDOR_STATUS_OK)
            {
                serialize_info();
            }
            (void)response_begin(generation, request->sequence, status, 0u,
                                 status == USB_VENDOR_STATUS_OK ?
                                 USB_VENDOR_INFO_SIZE : 0u);
            break;

        case USB_VENDOR_OPCODE_STATUS:
            if(request->address != 0u || request->length != 0u)
            {
                (void)response_begin(generation, request->sequence,
                                     USB_VENDOR_STATUS_INVALID_ARGUMENT,
                                     0u, 0u);
            }
            else
            {
                (void)response_begin(generation, request->sequence,
                                     USB_VENDOR_STATUS_OK,
                                     (uint32_t)g_public_state,
                                     g_last_processed);
            }
            break;

        case USB_VENDOR_OPCODE_FLASH_READ:
            command_read(request, USB_VENDOR_TARGET_NOR, generation);
            break;

        case USB_VENDOR_OPCODE_FLASH_WRITE:
            command_start_write(request, USB_VENDOR_TARGET_NOR, generation);
            break;

        case USB_VENDOR_OPCODE_FLASH_ERASE:
            command_erase(request, generation);
            break;

        case USB_VENDOR_OPCODE_FLASH_CRC32:
            command_crc(request, USB_VENDOR_TARGET_NOR, generation);
            break;

        case USB_VENDOR_OPCODE_PSRAM_READ:
            command_read(request, USB_VENDOR_TARGET_PSRAM, generation);
            break;

        case USB_VENDOR_OPCODE_PSRAM_WRITE:
            command_start_write(request, USB_VENDOR_TARGET_PSRAM,
                                generation);
            break;

        case USB_VENDOR_OPCODE_PSRAM_CRC32:
            command_crc(request, USB_VENDOR_TARGET_PSRAM, generation);
            break;

        case USB_VENDOR_OPCODE_CANCEL:
            if(request->address != 0u || request->length != 0u)
            {
                (void)response_begin(generation, request->sequence,
                                     USB_VENDOR_STATUS_INVALID_ARGUMENT,
                                     0u, 0u);
            }
            else
            {
                if(g_backend->cancel != NULL)
                {
                    if(!protocol_mark_executing(generation))
                    {
                        break;
                    }
                    g_backend->cancel(g_backend_context);
                }
                (void)response_begin(generation, request->sequence,
                                     USB_VENDOR_STATUS_OK, 0u, 0u);
            }
            break;

        default:
            (void)response_begin(generation, request->sequence,
                                 USB_VENDOR_STATUS_INVALID_OPCODE, 0u, 0u);
            break;
    }
}

/* 校验独立的 16 字节请求头并提取字段。 */
static void process_header_packet(const uint8_t *data, uint8_t length,
                                  uint32_t generation)
{
    request_t request;

    request.sequence = length >= 8u ? read_le16(&data[6]) : 0u;
    if(length != USB_VENDOR_REQUEST_SIZE ||
       data[0] != USB_VENDOR_REQ_MAGIC_0 ||
       data[1] != USB_VENDOR_REQ_MAGIC_1 ||
       data[2] != USB_VENDOR_REQ_MAGIC_2 ||
       data[3] != USB_VENDOR_REQ_MAGIC_3 ||
       data[4] != USB_VENDOR_PROTOCOL_VERSION)
    {
        (void)response_begin(generation, request.sequence,
                             USB_VENDOR_STATUS_BAD_FRAME, 0u, 0u);
        return;
    }

    request.opcode = data[5];
    request.address = read_le32(&data[8]);
    request.length = read_le32(&data[12]);
    process_request(&request, generation);
}

/* 累积 WRITE 负载，收齐后同步调用 SPI 后端。 */
static void process_write_packet(const uint8_t *data, uint8_t length,
                                 uint32_t generation)
{
    request_t request;
    usb_vendor_target_t target;
    usb_vendor_status_t status;
    uint8_t bad_frame = 0u;
    uint8_t complete = 0u;
    uint8_t irq_state;
    uint32_t offset;
    uint32_t crc;

    /* 先在临界区内取得本包对应的稳定写会话快照。 */
    irq_state = UsbWchUsbd_EnterCritical();
    if(!transport_is_current_locked(generation) ||
       g_protocol_phase != PROTOCOL_RX_WRITE)
    {
        UsbWchUsbd_ExitCritical(irq_state);
        return;
    }
    request = g_active_request;
    offset = g_transfer_offset;
    crc = g_write_crc;
    if(length == 0u || offset > request.length ||
       (uint32_t)length > (request.length - offset))
    {
        bad_frame = 1u;
    }
    UsbWchUsbd_ExitCritical(irq_state);

    if(bad_frame)
    {
        (void)response_begin(generation, request.sequence,
                             USB_VENDOR_STATUS_BAD_FRAME, 0u, 0u);
        return;
    }

    /* 复制与 CRC 计算不屏蔽中断；提交时再次核对 generation 和偏移。 */
    memcpy(&g_transfer_buffer[offset], data, length);
    crc = crc32_update(crc, data, length);
    offset += length;

    irq_state = UsbWchUsbd_EnterCritical();
    if(transport_is_current_locked(generation) &&
       g_protocol_phase == PROTOCOL_RX_WRITE &&
       g_transfer_offset + length == offset)
    {
        g_write_crc = crc;
        g_transfer_offset = offset;
        g_last_processed = offset;
        complete = offset == request.length;
        if(complete)
        {
            g_public_state = USB_VENDOR_STATE_EXECUTING;
        }
    }
    UsbWchUsbd_ExitCritical(irq_state);

    if(!complete)
    {
        return;
    }

    target = request.opcode == USB_VENDOR_OPCODE_FLASH_WRITE ?
             USB_VENDOR_TARGET_NOR : USB_VENDOR_TARGET_PSRAM;
    status = normalize_status(g_backend->write(g_backend_context, target,
                                               request.address,
                                               g_transfer_buffer,
                                               request.length));
    crc ^= 0xFFFFFFFFu;
    (void)response_begin(generation, request.sequence, status, crc, 0u);
}

/* 根据当前阶段解释主循环取出的 EP1 OUT 包。 */
static void process_out_packet(const uint8_t *data, uint8_t length,
                               protocol_phase_t phase, uint32_t generation)
{
    if(phase == PROTOCOL_IDLE)
    {
        process_header_packet(data, length, generation);
    }
    else if(phase == PROTOCOL_RX_WRITE)
    {
        process_write_packet(data, length, generation);
    }
    else
    {
        (void)response_begin(generation, 0u, USB_VENDOR_STATUS_BUSY, 0u, 0u);
    }
}

/* 绑定存储后端并启动 WCH USBD/PMA 传输层。 */
usb_vendor_status_t UsbVendor_Init(const usb_vendor_backend_t *backend,
                                   void *backend_context)
{
    if(backend == NULL || backend->get_info == NULL)
    {
        return USB_VENDOR_STATUS_INVALID_ARGUMENT;
    }

    g_backend = backend;
    g_backend_context = backend_context;
    g_bus_generation = 0u;
    g_configured = 0u;

    protocol_reset();
    return UsbWchUsbd_Init();
}

/* 主循环入口：处理一个 OUT 包并推进 IN 响应。 */
void UsbVendor_Poll(void)
{
    uint8_t packet[USB_VENDOR_EP_PACKET_SIZE];
    uint32_t generation = 0u;
    uint8_t length = 0u;
    uint8_t have_packet = 0u;
    uint8_t irq_state;
    protocol_phase_t phase = PROTOCOL_IDLE;

    /* 原子取走 IRQ 队列；后续 SPI 工作全部在临界区之外执行。 */
    irq_state = UsbWchUsbd_EnterCritical();
    if(g_configured && g_out_ready)
    {
        generation = g_bus_generation;
        length = g_out_length;
        phase = g_protocol_phase;
        memcpy(packet, g_out_packet, length);
        g_out_ready = 0u;
        g_out_length = 0u;
        have_packet = 1u;
    }
    UsbWchUsbd_ExitCritical(irq_state);

    if(have_packet)
    {
        process_out_packet(packet, length, phase, generation);
        endpoint_out_rearm(generation);
    }
    tx_service();
}

/* 返回主机是否已选择配置 1。 */
uint8_t UsbVendor_IsConfigured(void)
{
    uint8_t configured;
    uint8_t irq_state = UsbWchUsbd_EnterCritical();

    configured = g_configured;
    UsbWchUsbd_ExitCritical(irq_state);
    return configured;
}

/* 总线复位回调：失效正在执行的请求并清空协议状态。 */
void UsbVendor_OnBusReset(void)
{
    ++g_bus_generation;
    g_configured = 0u;
    protocol_reset();
}

/* 配置变更回调：重置半帧并按配置状态控制 OUT 端点。 */
void UsbVendor_OnConfigurationChanged(uint8_t configured)
{
    ++g_bus_generation;
    g_configured = configured != 0u;
    protocol_reset();
}

/* Bulk IN 完成回调：允许状态机发送下一包。 */
void UsbVendor_OnInComplete(void)
{
    if(g_configured && g_in_busy)
    {
        g_in_busy = 0u;
        g_in_complete = 1u;
    }
}

/* Bulk OUT 回调：复制单包到软件队列并立即施加 NAK 背压。 */
void UsbVendor_OnOutPacket(const uint8_t *data, uint8_t length)
{
    UsbWchUsbd_SetOutEnabled(0u);
    if(!g_configured || g_out_ready || data == NULL ||
       length > USB_VENDOR_EP_PACKET_SIZE)
    {
        return;
    }

    memcpy(g_out_packet, data, length);
    g_out_length = length;
    g_out_ready = 1u;
}
