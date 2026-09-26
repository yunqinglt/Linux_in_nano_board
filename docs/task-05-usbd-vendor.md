# CH32V203F6P6 USBD Vendor 维护固件

## 本轮结果

固件已经从不适用于当前 PA11/PA12 连线的 `USBFS@0x50000000` 迁移到 WCH
legacy `USBD + PMA` 控制器：

- 寄存器：`USBD@0x40005C00`，PMA `0x40006000`；
- 时钟：96 MHz HSI PLL 经 `PLLCLK/2` 得到 48 MHz，外设挂在 APB1；
- IRQ：`USB_LP_CAN1_RX0_IRQHandler`（IRQ36）调用 `USB_Istr()`；
- 唤醒：`USBWakeUp_IRQHandler` 清除 EXTI18；
- 连接：PA11/PA12 先拉低并关闭内部上拉 20 ms，再切为浮空并开启
  `EXTEN_USBD_PU_EN`，确保 Windows 重新枚举。

WCH 官方栈固定为子模块 `third_party/openwch-ch32v20x`，提交
`baef0054588f4826548429ff4e7a9257d752ef2f`。构建只取其中六个 USB core
源文件和 `usb_istr.c`、`usb_pwr.c`；CDC 示例的描述符和类逻辑不会进入固件。

## 端点和协议

EP0 标准请求由 WCH `usb_core.c` 处理。应用层只提供描述符和配置状态回调。
PMA 布局如下：

| 区域 | PMA 偏移 | 大小 |
|---|---:|---:|
| BTABLE | `0x000` | 端点表 |
| EP0 OUT | `0x040` | 64 B |
| EP0 IN | `0x080` | 64 B |
| EP1 OUT | `0x0C0` | 64 B |
| EP1 IN | `0x100` | 64 B |

设备保持原有上位机合同：`1209:2036`、Vendor `FF/53/01`、EP1 OUT
`0x01`、EP1 IN `0x81`、Full-Speed Bulk 64 B。中断只复制单个包并设置
pending/complete 标志；SPI 擦写、CRC 和协议状态机仍在主循环运行。EP1 OUT
用 `VALID/NAK` 实现单包背压，总线复位、取消配置或清除 HALT 时会丢弃半帧，
防止旧 PMA 数据被继续发送。每次 `SET_CONFIGURATION`（包括重复选择配置 1）
和 `SET_INTERFACE(0)` 都会先把 EP1 双向设为 NAK，再清除 CTR、DATA toggle
和旧计数；因此 PyUSB 重复打开设备不会继承上一次连接的 DATA1 状态。

主循环领取 OUT 包、提交响应和启动 PMA IN 都在只屏蔽 USB LP IRQ 的短临界区
内复核 32 位配置 generation。SPI/CRC 始终在临界区外执行；若操作期间发生总线
复位，已经开始的同步 SPI 操作会自然完成，但旧 generation 的结果不会再进入 PMA
或污染新会话。

## 构建与容量

```powershell
cd C:\Users\云晴\mounriver-studio-projects\CH32V203F6P
.\flash\firmware\Build-Flasher.ps1
```

归档构建写入 `flash/build/`，不会覆盖根目录 Product profile 的 `obj/`。

本轮最终链接结果：

- Flash：`text 23036 + data 384 = 23420 B / 32768 B`；
- RAM：`data 384 + bss 4956 + stack 2048 = 7388 B / 10240 B`；
- `_ebss=0x200014DC`，栈从 `0x20002000` 开始，中间保留 `2852 B`。

经过真机验证并冻结的发布烧录文件为：

`C:\Users\云晴\mounriver-studio-projects\CH32V203F6P\flash\release\CH32V203F6P-usb-flasher-v1.0.0.hex`

需要验证归档可重复构建时，可改用
`flash/build/CH32V203F6P.hex`；其 SHA-256 必须与上述发布文件相同。

## 验证顺序

1. 在 MounRiver Studio 中手动下载上述冻结发布 HEX。
2. COM19 应打印系统时钟、ChipID、两颗 SPI 芯片 ID、USB Vendor ready、
   `Command table: 2 registered (help, probe)`。
3. Windows 应出现 `VID_1209&PID_2036`；首次使用时仅为这个新设备安装
   WinUSB/libusbK，不改动 WCH-Link 的 `1A86:8010`。
4. 进入 `flash/host/`，运行 `python -m ch32_image_tool.cli list` 和 `info`，
   再进行小范围双芯片完整性测试。

## 真机验证（2026-09-27）

- COM19 启动日志：`SystemClk=96000000`、`ChipID=20370500`；
- W25Q64：`EF 40 17`，APS6404L：`0D 5D 52 D6`；
- Windows 正常枚举 `CH32V203 Flash Bridge`，实例为
  `USB\VID_1209&PID_2036\CH32V203F6P6`；
- 连续运行 `list`、`info`、`test`，三次独立打开均成功；
- 两颗芯片各抽样 5 个 4 KiB 块，写入、读回及原数据恢复全部通过；
- `res/` 已打包为严格 `8,388,608 B` 镜像，manifest CRC32 为
  `6C192C43`，镜像逻辑 CRC32 为 `4A01555E`；
- W25Q64 全片擦除、刷写和逐字节验证通过；随后独立回读 8 MiB，源镜像与
  回读镜像 SHA-256 均为
  `5D5DA3F3445AF5FEEBF968FC9DC7E7E5BF9374326AE3FCB252B7F48805D4400C`。

固定镜像的二进制布局、文件 offset/slot、发布指纹、CRC 覆盖范围以及
下一轮下位机读取要求见 [W25Q64 固定镜像格式](flash-image-format.md)。

上位机离线测试已通过：`16 passed`。生成的 8 MiB 镜像、回读文件与过程日志
工作副本位于已忽略的 `flash/host/out/`；经过验证的冻结镜像另存为
`flash/release/mipsel-emu-8m-v1.img`。
