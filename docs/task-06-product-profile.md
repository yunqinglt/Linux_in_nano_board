# 刷写产品归档与运行固件基线（历史记录）

> 本文记录 Product profile 切换前的调试基线。当前正式版已经关闭
> USART1/2/3，并通过 USB CDC 输出；最新行为见仓库根目录 `README.md` 和
> `docs/task-08-product-boot.md`。

## 本轮边界

已经验证的 USB Vendor 刷写实现没有被丢弃，而是整体迁入独立的
[`flash/`](../flash/README.md) 目录。根目录 `User/` 现在是下一轮开发使用的
Product profile，不再枚举 `1209:2036`，也不包含 W25Q64 编程或擦除路径。

独立归档包括：

- 完整下位机源码快照和可重复构建脚本：`flash/firmware/`；
- PyQt6/PyUSB/libusb1 上位机和测试：`flash/host/`；
- 冻结的 MCU Intel HEX 与经过回读验证的 8 MiB 镜像：`flash/release/`；
- 镜像固定 ABI：[flash-image-format.md](flash-image-format.md)。

发布 HEX 来源提交为 `84f85172b6477ae47ce1173c70aece1ce620bb79`，SHA-256 为
`DB78754578653C914459BFAA952AE4AB858E69AD52C57E1B668B55C681C50542`。
归档脚本重建出的 HEX 已验证与冻结文件逐字节一致。

## 当前活动固件

- 系统时钟仍为已在板上验证的 96 MHz HSI；
- USART2/COM19 仍提供非阻塞命令行；
- 当前命令表为 `help`、`probe`；本轮没有提前注册 `boot`；
- SPI1 初始化和两颗芯片探测保留；
- 通用存储 API 已与 USB 协议解耦：
  - `SpiStorage_NorRead()`；
  - `SpiStorage_PsramRead()`；
  - `SpiStorage_PsramWrite()`；
- W25Q64 program/erase、Vendor 协议、PMA 端点和 4096 B USB 传输缓冲均不再
  进入活动固件；
- OpenWCH USBD 子模块仍保留，供下一轮 USB CDC 使用。

最终链接容量：

- Flash：`text 13264 + data 160 = 13424 B / 32768 B`；
- RAM：`data 160 + bss 544 + stack 2048 = 2752 B / 10240 B`；
- `_ebss=0x200002C0`，栈从 `0x20002000` 开始，间隔 `7488 B`。

## 真机验证

2026-09-27 将根目录最终构建的
`obj/CH32V203F6P.hex` 经 MounRiver 手动下载并复位后，COM19（115200、
8N1、无流控）在 20 秒监听窗口内输出：

```text
SystemClk:96000000
ChipID:20370500
Product firmware: SPI runtime profile
SPI1 W25Q64 CS0: present=yes id=EF 40 17 expected=yes
SPI1 APS6404L CS1: present=yes mf=0D kgd=5D eid=52D6 expected=yes
```

随后通过同一 COM19 发送 `help`，确认活动命令表仅注册：

```text
Commands:
  help   Show this command table
  probe  Read W25Q64 and APS6404L IDs (resets APS6404L)
```

监听脚本在两条 SPI 探测记录均正确后提前结束。Windows 即时设备枚举仅见
WCH-LinkRV 和 `WCH-Link SERIAL (COM19)`（`1A86:8010`），未见维护固件的
`1209:2036`，与 Product profile 不启动 Vendor USB 的设计一致。

## 下一轮入口

下一轮按以下顺序继续：

1. 基于保留的 OpenWCH USBD/PMA 栈启用 USB CDC，仅作为 Linux 启动输出；
2. USART 命令行继续保留，并在现有表中注册 `boot`；
3. `boot` 先按固定 manifest ABI 校验并定位 W25Q64 中的 vmlinux、DTB、
   initramfs，再把通用 SPI/PSRAM API 接入 MIPS 模拟器引擎；
4. 文件实际长度必须从 manifest 读取，固定的是 slot 地址和容量；vmlinux 必须
   流式读取，不能整体复制到 MCU 内部 RAM。

`flash/` 已同时加入 MounRiver Studio 2 的 `.cproject` 与 `.wvproj` 资源排除项，
避免归档中的第二个 `main.c` 被 Product profile 误编译。`third_party/` 也在这两处
整体排除；下一轮需要的 OpenWCH CDC 源文件由构建配置显式选入，不递归编译整个
参考源码目录。
