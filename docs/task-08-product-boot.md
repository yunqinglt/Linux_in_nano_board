# 正式版 USB CDC 启动固件

本轮把原 `User/main.c`、`User/usart_console.c` 和 USART 命令行依赖保留为
`Debug` 配置；正式产品入口位于 `Product/`，不会删除调试实现。

## 构建

默认构建正式版，输出路径仍固定为：

```powershell
.\tools\Build-Firmware.ps1 -Profile Product
# C:\Users\云晴\mounriver-studio-projects\CH32V203F6P\obj\CH32V203F6P.hex
```

调试版可显式构建并恢复 `help/probe/boot` 命令行：

```powershell
.\tools\Build-Firmware.ps1 -Profile Debug
```

正式版排除 `User/main.c`、`User/usart_console.c`、USART2 ISR 和 `lib/` 命令行
实现，入口只初始化 USB CDC；启动时显式禁用、复位并关闭 USART1/2/3 的中断与
外设时钟，USART 不再承担产品日志。

## CDC 启动序列

1. 初始化 USB CDC，并输出时钟、芯片 ID 和两颗 SPI 存储器的探测结果。
2. 只有 W25Q64 `EF 40 17` 与 APS6404L `0D 5D` 精确匹配时才继续；缺失或
   型号异常会输出带接线/电源提示的错误并停留在 CDC 服务循环。
3. 正常探测后等待 3000 ms；等待期间持续调用 `UsbCdc_Service()`。
4. 从固定 manifest 校验 payload CRC32，按 ELF `PT_LOAD` 的 `p_memsz` 将内核段流式
   写入 APS6404L，再加载 DTB 和 initramfs。每约 256 KiB 通过 CDC 报告一次进度。
5. 设置 MIPS 入口和 DTB 参数寄存器，输出 `Linux boot at ...`，随后进入 guest
   时间片循环。guest 的 16550 UART 输出与固件日志共用 CDC EP3，但使用独立队列。

`MIPSEL_IMAGE_SOURCE_IO` 映射为 W25Q64 读取错误，`MIPSEL_IMAGE_MEMORY_IO` 映射为
APS6404L 读写错误，便于区分镜像损坏和硬件故障。

## 手动下载与监听

MounRiver 仍从固定文件 `obj/CH32V203F6P.hex` 下载。下载后复位板卡，并用以下
正式版不再使用 COM19/USART1（也不发送命令），日志只会出现在 USB CDC ACM
枚举出的新串口。手动下载并复位后，请将脚本的 `-Port` 指向该 CDC 端口：

```powershell
.\tools\Read-WchLinkSerial.ps1 -Port COM<CDC> -Command '' -ExpectedPattern 'Linux boot at|ERROR:'
```

调试版仍可使用脚本默认的 `probe` 命令。正式版 HEX 的 SHA-256 为
`BB137D22D717FDD1EECE44B27CD43D3BEE376C2FA5807C522DFB1F47BBA4BC21`。
