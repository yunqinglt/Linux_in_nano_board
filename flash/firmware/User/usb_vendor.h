#ifndef USB_VENDOR_H
#define USB_VENDOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_VENDOR_VID                 0x1209u
#define USB_VENDOR_PID                 0x2036u
#define USB_VENDOR_PROTOCOL_VERSION    1u
#define USB_VENDOR_EP_OUT              0x01u
#define USB_VENDOR_EP_IN               0x81u
#define USB_VENDOR_EP_PACKET_SIZE      64u
#define USB_VENDOR_MAX_TRANSFER        4096u

#define USB_VENDOR_INFO_FLAG_NOR_PRESENT       0x0001u
#define USB_VENDOR_INFO_FLAG_PSRAM_PRESENT     0x0002u
#define USB_VENDOR_INFO_FLAG_NOR_ERASE         0x0004u
#define USB_VENDOR_INFO_FLAG_CRC32              0x0008u

typedef enum
{
    USB_VENDOR_STATUS_OK = 0,
    USB_VENDOR_STATUS_INVALID_OPCODE = 1,
    USB_VENDOR_STATUS_INVALID_ARGUMENT = 2,
    USB_VENDOR_STATUS_OUT_OF_RANGE = 3,
    USB_VENDOR_STATUS_BUSY = 4,
    USB_VENDOR_STATUS_IO_ERROR = 5,
    USB_VENDOR_STATUS_VERIFY_FAILED = 6,
    USB_VENDOR_STATUS_CANCELLED = 7,
    USB_VENDOR_STATUS_BAD_FRAME = 8,
    USB_VENDOR_STATUS_INTERNAL_ERROR = 9
} usb_vendor_status_t;

typedef enum
{
    USB_VENDOR_TARGET_NOR = 0,
    USB_VENDOR_TARGET_PSRAM = 1
} usb_vendor_target_t;

typedef enum
{
    USB_VENDOR_OPCODE_INFO = 0x01,
    USB_VENDOR_OPCODE_STATUS = 0x02,
    USB_VENDOR_OPCODE_FLASH_READ = 0x10,
    USB_VENDOR_OPCODE_FLASH_WRITE = 0x11,
    USB_VENDOR_OPCODE_FLASH_ERASE = 0x12,
    USB_VENDOR_OPCODE_FLASH_CRC32 = 0x13,
    USB_VENDOR_OPCODE_PSRAM_READ = 0x20,
    USB_VENDOR_OPCODE_PSRAM_WRITE = 0x21,
    USB_VENDOR_OPCODE_PSRAM_CRC32 = 0x23,
    USB_VENDOR_OPCODE_CANCEL = 0x7F
} usb_vendor_opcode_t;

typedef enum
{
    USB_VENDOR_STATE_IDLE = 0,
    USB_VENDOR_STATE_RECEIVING = 1,
    USB_VENDOR_STATE_EXECUTING = 2,
    USB_VENDOR_STATE_RESPONDING = 3
} usb_vendor_state_t;

/* INFO 按小端 <IIIIHHHH4s4s> 序列化，共 32 字节。 */
typedef struct
{
    uint32_t flash_size;
    uint32_t psram_size;
    uint32_t erase_size;
    uint32_t max_transfer;
    uint16_t page_size;
    uint16_t flags;
    uint16_t fw_major;
    uint16_t fw_minor;
    uint8_t nor_id[4];
    uint8_t psram_id[4];
} usb_vendor_info_t;

typedef struct
{
    usb_vendor_status_t (*get_info)(void *context,
                                    usb_vendor_info_t *info);
    usb_vendor_status_t (*read)(void *context,
                                usb_vendor_target_t target,
                                uint32_t address,
                                uint8_t *data,
                                uint32_t length);
    usb_vendor_status_t (*write)(void *context,
                                 usb_vendor_target_t target,
                                 uint32_t address,
                                 const uint8_t *data,
                                 uint32_t length);
    usb_vendor_status_t (*erase)(void *context,
                                 uint32_t address,
                                 uint32_t length);
    void (*cancel)(void *context);
} usb_vendor_backend_t;

/*
 * 在 PA11/PA12 启动 USB 2.0 Full-Speed Vendor 设备。
 * 系统时钟必须为 48、96 或 144 MHz，使 USBD 控制器获得 48 MHz。
 */
usb_vendor_status_t UsbVendor_Init(const usb_vendor_backend_t *backend,
                                   void *backend_context);
void UsbVendor_Poll(void);
uint8_t UsbVendor_IsConfigured(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_VENDOR_H */
