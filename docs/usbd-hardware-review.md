# CH32V203F6P USBD 硬件审查

## 当前证据

目标工程的 `Peripheral/inc/ch32v20x.h` 同时保留了两条 USB 设备路径的符号：

| 项目 | USBD（PMA 栈） | USBFS（FIFO/DMA 栈） |
| --- | --- | --- |
| 外设时钟 | `RCC_APB1Periph_USB` | `RCC_AHBPeriph_USBFS` |
| 设备中断 | `USB_LP_CAN1_RX0_IRQn = 36` | `USBFS_IRQn = 59` |
| 唤醒中断 | `USBWakeUp_IRQn = 58` | `USBFSWakeUp_IRQn = 60` |
| EXTI | `EXTI_Line18` | `EXTI_Line20` |
| 引脚上拉 | `EXTEN_USBD_PU_EN` | USBFS 控制寄存器的 `DEV_PU_EN` |

同型号 `CH32V203F6P6` 的可运行参考工程 `C:/Temp/BlasterUsbReview2/CH32V203` 使用 WCH 官方 `USBLib/USBUsr` 栈，底层定义为：

```c
#define RegBase  (0x40005C00L)
#define PMAAddr  (0x40006000L)
```

该工程初始化顺序为 `RCC_USBCLKConfig`、`RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE)`、`USB_Init()`，并启用 `USB_LP_CAN1_RX0_IRQn` 与 `EXTI_Line18`。`USBUsr/usb_prop.c` 还明确对 PA11/PA12 做 USBD GPIO 复用和 `EXTEN_USBD_PU_EN` 内部上拉控制。

## 结论

当前 `User/usb_vendor.c` 使用的是 `USBFSD`（`0x50000000`）、`RCC_AHBPeriph_USBFS` 和 `USBFS_IRQn`。这条路径与参考工程的 PA11/PA12 USBD 连接不一致，不能通过简单修改中断函数名修复。迁移应使用 WCH 官方 USBD 分层栈，并保留 PA11/PA12 的断开、延时、重新连接及 EXTI18 唤醒流程。

目标头文件未公开 `0x40005C00/0x40006000` 的寄存器类型，但参考工程直接使用这些地址且针对同一 D6 器件编译运行；因此移植时应保留官方 `usb_regs.h` 的硬编码寄存器定义，不要把其替换为 `USBFSD` 结构体。

## 迁移约束

1. EP0 标准请求交给 `USBLib/usb_core.c`，不要继续维护手写 `GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIGURATION` 状态机。
2. EP1 OUT/IN 保持现有 vendor 描述符和 64 字节 bulk 协议，数据缓冲通过 PMA 的 `UserToPMABufferCopy` 与 `PMAToUserBufferCopy` 访问。
3. 启动顺序必须先配置 USB 时钟和 APB1 USB 时钟，再调用 `USB_Init()`；IRQ 使用 `USB_LP_CAN1_RX0_IRQHandler`，唤醒使用 `USBWakeUp_IRQHandler`。
4. 参考工程的 D6 链接脚本为 32 KiB Flash、10 KiB RAM；USB 栈引入后应重新检查 `.bss`/栈容量。
5. CherryUSB 的 `port/wch/usbfs` 仍然是另一条 USBFS 路径，不应与本次 USBD/PMA 栈混编。

## 最小枚举验证

先只接入 `INFO` 命令和设备/配置/字符串描述符，烧录后检查 Windows PnP 是否出现 `1209:2036`。枚举成功后再接入 READ/WRITE/CRC，避免把 SPI 阻塞错误与 USB 控制器错误混在一起。

