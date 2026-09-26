#include "usart_console.h"

#include <stddef.h>

#include "cmdline.h"
#include "debug.h"
#include "spi_storage.h"
#include "usart_proxy.h"

#define ARRAY_COUNT(array)       (sizeof(array) / sizeof((array)[0]))
#define CONSOLE_RX_BUFFER_SIZE   128u
#define CONSOLE_TX_BUFFER_SIZE   64u
#define CONSOLE_LINE_BUFFER_SIZE 96u

static cmdline_status_t command_help(cmdline_argument_t arguments);
static cmdline_status_t command_probe(cmdline_argument_t arguments);

static cmdline_t g_cmdline;
static cmdline_mount_t g_cmdline_mounts[1];
static UsartProxy_t g_usart_proxy;
static uint8_t g_console_rx_buffer[CONSOLE_RX_BUFFER_SIZE];
static uint8_t g_console_tx_buffer[CONSOLE_TX_BUFFER_SIZE];
static char g_console_line[CONSOLE_LINE_BUFFER_SIZE];
static size_t g_console_line_length;
static uint8_t g_console_line_overflow;
static uint8_t g_console_last_was_cr;

static const cmdline_table_t g_command_table[] = {
    {"help", command_help},
    {"probe", command_probe},
};

/* 以便于 COM19 脚本识别的固定格式打印两颗芯片 ID。 */
static void console_print_storage_info(const usb_vendor_info_t *info)
{
    printf("SPI1 W25Q64 CS0: present=%s id=%02X %02X %02X expected=%s\r\n",
           (info->flags & USB_VENDOR_INFO_FLAG_NOR_PRESENT) ? "yes" : "no",
           info->nor_id[0], info->nor_id[1], info->nor_id[2],
           (info->nor_id[0] == 0xEFu && info->nor_id[1] == 0x40u &&
            info->nor_id[2] == 0x17u) ? "yes" : "no");
    printf("SPI1 APS6404L CS1: present=%s mf=%02X kgd=%02X eid=%02X%02X expected=%s\r\n",
           (info->flags & USB_VENDOR_INFO_FLAG_PSRAM_PRESENT) ? "yes" : "no",
           info->psram_id[0], info->psram_id[1],
           info->psram_id[2], info->psram_id[3],
           (info->psram_id[0] == 0x0Du &&
            info->psram_id[1] == 0x5Du) ? "yes" : "no");
}

/* 输出当前固件注册的命令表。 */
static cmdline_status_t command_help(cmdline_argument_t arguments)
{
    if(arguments.argc != 1u)
    {
        return CMDLINE_STATUS_ARGUMENT_COUNT;
    }

    printf("Commands:\r\n");
    printf("  help   Show this command table\r\n");
    printf("  probe  Read W25Q64 and APS6404L IDs (resets APS6404L)\r\n");
    return CMDLINE_STATUS_OK;
}

/* 立即重新初始化 SPI 存储并打印探测结果。 */
static cmdline_status_t command_probe(cmdline_argument_t arguments)
{
    usb_vendor_info_t info;
    usb_vendor_status_t status;

    if(arguments.argc != 1u)
    {
        return CMDLINE_STATUS_ARGUMENT_COUNT;
    }

    status = SpiStorage_Init();
    if(status != USB_VENDOR_STATUS_OK)
    {
        printf("probe: SPI storage init failed: status=%u\r\n",
               (unsigned int)status);
        return CMDLINE_STATUS_IO_ERROR;
    }
    SpiStorage_GetInfo(&info);
    console_print_storage_info(&info);
    return CMDLINE_STATUS_OK;
}

/* 将命令行状态码转换为简短的串口错误文本。 */
static const char *console_status_text(cmdline_status_t status)
{
    switch(status)
    {
        case CMDLINE_STATUS_INVALID_ARGUMENT:
            return "invalid argument";
        case CMDLINE_STATUS_TOO_MANY_ARGUMENTS:
            return "too many arguments";
        case CMDLINE_STATUS_MOUNT_FULL:
            return "mount table full";
        case CMDLINE_STATUS_DUPLICATE_COMMAND:
            return "duplicate command";
        case CMDLINE_STATUS_BAD_FORMAT:
            return "bad format";
        case CMDLINE_STATUS_ARGUMENT_COUNT:
            return "unexpected arguments";
        case CMDLINE_STATUS_ARGUMENT_VALUE:
            return "bad argument value";
        case CMDLINE_STATUS_ARGUMENT_RANGE:
            return "argument out of range";
        case CMDLINE_STATUS_IO_ERROR:
            return "I/O error";
        default:
            return "internal error";
    }
}

/* 输出命令提示符。 */
static void console_prompt(void)
{
    printf("ch32> ");
}

/* 初始化 USART 代理并注册 help、probe 两个命令。 */
uint8_t UsartConsole_Init(void)
{
    cmdline_status_t cmdline_status;
    UsartProxyStatus_t proxy_status;

    proxy_status = UsartProxy_Init(&g_usart_proxy,
                                   UsartEngine_GetInstance(),
                                   PROXY_NONBLOCKING,
                                   g_console_rx_buffer,
                                   sizeof(g_console_rx_buffer),
                                   g_console_tx_buffer,
                                   sizeof(g_console_tx_buffer));
    if(proxy_status != USART_PROXY_STATUS_OK &&
       proxy_status != USART_PROXY_STATUS_RECOVERED)
    {
        printf("Console init failed: USART proxy status=%u\r\n",
               (unsigned int)proxy_status);
        return 0u;
    }

    cmdline_status = Cmdline_Init(&g_cmdline,
                                  g_cmdline_mounts,
                                  ARRAY_COUNT(g_cmdline_mounts));
    if(cmdline_status == CMDLINE_STATUS_OK)
    {
        cmdline_status = Cmdline_Mount(&g_cmdline,
                                       g_command_table,
                                       ARRAY_COUNT(g_command_table),
                                       NULL);
    }
    if(cmdline_status != CMDLINE_STATUS_OK)
    {
        printf("Console init failed: command table status=%u\r\n",
               (unsigned int)cmdline_status);
        return 0u;
    }

    g_console_line_length = 0u;
    g_console_line_overflow = 0u;
    g_console_last_was_cr = 0u;
    printf("Console: USART2 PA2/PA3, 115200 8N1\r\n");
    printf("Command table: %u registered (help, probe)\r\n",
           (unsigned int)ARRAY_COUNT(g_command_table));
    printf("USART proxy: %s\r\n",
           proxy_status == USART_PROXY_STATUS_RECOVERED ? "recovered" : "ready");
    console_prompt();
    return 1u;
}

/* 结束当前输入行并交给命令表解析执行。 */
static void console_execute_line(void)
{
    cmdline_status_t status;
    const char *command;

    printf("\r\n");
    if(g_console_line_overflow)
    {
        printf("error: command line exceeds %u characters\r\n",
               (unsigned int)(sizeof(g_console_line) - 1u));
    }
    else
    {
        g_console_line[g_console_line_length] = '\0';
        status = Cmdline_Execute(&g_cmdline, g_console_line);
        if(status == CMDLINE_STATUS_UNKNOWN_COMMAND)
        {
            command = Cmdline_LastCommand(&g_cmdline);
            printf("error: unknown command%s%s\r\n",
                   command != NULL ? ": " : "",
                   command != NULL ? command : "");
        }
        else if(status != CMDLINE_STATUS_OK && status != CMDLINE_STATUS_EMPTY)
        {
            printf("error: %s\r\n", console_status_text(status));
        }
    }

    g_console_line_length = 0u;
    g_console_line_overflow = 0u;
    console_prompt();
}

/* 处理回车、退格、可打印字符和行溢出。 */
static void console_process_character(int character)
{
    if(character == '\r')
    {
        console_execute_line();
        g_console_last_was_cr = 1u;
        return;
    }
    if(character == '\n')
    {
        if(g_console_last_was_cr)
        {
            g_console_last_was_cr = 0u;
            return;
        }
        console_execute_line();
        return;
    }

    g_console_last_was_cr = 0u;
    if(character == '\b' || character == 0x7F)
    {
        if(!g_console_line_overflow && g_console_line_length != 0u)
        {
            --g_console_line_length;
            printf("\b \b");
        }
        return;
    }

    if(character >= 0x20 && character <= 0x7E)
    {
        if(!g_console_line_overflow &&
           g_console_line_length < (sizeof(g_console_line) - 1u))
        {
            g_console_line[g_console_line_length++] = (char)character;
            printf("%c", character);
        }
        else
        {
            g_console_line_overflow = 1u;
        }
    }
}

/* 非阻塞抽取 USART 字符并推进命令行模型。 */
void UsartConsole_Poll(void)
{
    int character;

    while((character = UsartProxy_TryGetchar(&g_usart_proxy)) >= 0)
    {
        console_process_character(character);
    }
}
