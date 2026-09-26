# CH32V203 USB image tool

本目录是 CH32V203F6P6 固件的配套上位机。GUI 使用 PyQt6，USB 层使用
PyUSB，并显式把 `libusb1` Python 包附带的 `libusb-1.0.dll` 交给 PyUSB 的
libusb-1.0 backend；因此不会依赖 PyUSB 在 Windows 上不稳定的自动 DLL 搜索。

它提供以下操作：

- 枚举并打开 MCU 自身枚举的 `1209:2036` Vendor 设备；
- 对 W25Q64 NOR Flash 和 APS6404L PSRAM 做可恢复的数据完整性测试；
- 将 `res/` 的三个文件打成严格 `8,388,608` 字节的 W25Q64 镜像；
- 全片擦除、分块刷写、逐字节回读验证，以及单独保存回读镜像；
- GUI 后台执行、进度、块边界取消和清晰的 USB/协议错误报告；
- 同一核心的 CLI，便于自动化和无 GUI 恢复。

## 安装与启动

建议使用 64 位 Python 3.10 或更新版本：

```powershell
cd C:\Users\云晴\mounriver-studio-projects\CH32V203F6P\flash\host
py -m venv .venv
.\.venv\Scripts\python -m pip install -r requirements.txt
.\.venv\Scripts\python -m ch32_image_tool
```

也可以安装为带入口点的本地包：

```powershell
.\.venv\Scripts\python -m pip install -e .
ch32-image-tool
ch32-image-cli --help
```

Windows 第一次连接 MCU 的 Vendor interface 时，需要用 Zadig **只替换新出现的
`1209:2036` 接口**为 WinUSB 或 libusbK。不要替换 WCH-Link 的 `1A86:8010`
调试/串口接口。

若要使用系统中另一份 DLL，可以设置
`LIBUSB_LIBRARY_PATH=C:\path\to\libusb-1.0.dll`。

## 固定镜像布局

所有未使用字节均填充为 NOR 擦除值 `0xFF`。

| 区域 | 起始地址 | 槽大小 | 当前文件 |
|---|---:|---:|---|
| 二进制 manifest | `0x000000` | 64 KiB | magic、几何、CRC、文件表 |
| Linux ELF | `0x010000` | 6 MiB | `vmlinux` |
| Device Tree | `0x610000` | 64 KiB | `mipsel-emu-embedded.dtb` |
| 根文件系统 | `0x620000` | 1920 KiB | `initramfs.cpio.gz` |

Manifest 固定头为 64 字节，每个文件项为 64 字节。文件项含固定地址、槽大小、
实际长度、CRC32 和 SHA-256。头部还保存全镜像 CRC32、擦除块 `4096` 和 USB
传输缓冲 `4096`。这里的 USB 缓冲与模拟器 loader 的
`MIPSEL_EMU_IMAGE_CHUNK_SIZE=128` 是不同层次的常量。

头部 little-endian 字段为
`<8s magic, u16 version, u16 header_size, u32 image_size, u32 erase_size,`
`u32 transfer_size, u32 entry_count, u32 manifest_crc, u32 image_crc, 28 reserved>`；
文件项为 `<16s name, u32 offset, u32 slot_size, u32 data_size, u32 crc32,`
`32s sha256>`。计算 `image_crc` 时两个 CRC 字段均视作零；计算
`manifest_crc` 时只把自身字段视作零，并包含已经写入的 `image_crc`。

打包过程先写同目录临时文件，成功后原子替换目标。已有镜像在刷写前会再次验证
长度、manifest、每项散列和全镜像 CRC。

```powershell
python -m ch32_image_tool.cli pack `
  --resources ..\..\res `
  --output .\out\mipsel-emu-8m.img
python -m ch32_image_tool.cli inspect .\out\mipsel-emu-8m.img
```

## USB 协议 v1

- Device/interface：`1209:2036`，Vendor class/subclass/protocol `FF/53/01`。
- EP1 OUT `0x01`、EP1 IN `0x81`，Full-Speed bulk，最大包 64 字节。
- 最大命令数据为 4096 字节；同一时刻只允许一个请求等待一个响应。
- 请求头必须作为独立的 16 字节 short transfer 发送，WRITE payload 在下一次
  transfer 发送。响应也必须先独立发送 16 字节 short transfer，再发送 payload。

请求头：`<4sBBHII>` little-endian，字段为 `MVCH`、版本、opcode、sequence、
address、length。响应头同布局，字段为 `MVRS`、版本、status、sequence、value、
payload length。

| Opcode | 值 | 行为 |
|---|---:|---|
| INFO | `0x01` | 返回容量、几何、固件版本、芯片 ID |
| STATUS | `0x02` | 返回内部状态和已处理字节数 |
| FLASH_READ/WRITE/ERASE/CRC32 | `0x10..0x13` | W25Q64 操作 |
| PSRAM_READ/WRITE/CRC32 | `0x20/0x21/0x23` | APS6404L 操作 |
| CANCEL | `0x7F` | 取消固件内的长操作 |

READ/WRITE 的 `value` 是 payload CRC32；ERASE 的 `value` 是完成字节数；CRC32
命令直接在 `value` 返回结果。状态码 `0..9` 依次为 OK、未知命令、参数错误、
越界、忙、I/O 错误、校验失败、已取消、坏帧、内部错误。

## CLI 示例

```powershell
python -m ch32_image_tool.cli list
python -m ch32_image_tool.cli info
python -m ch32_image_tool.cli test --chip both
python -m ch32_image_tool.cli flash .\out\mipsel-emu-8m.img
python -m ch32_image_tool.cli verify .\out\mipsel-emu-8m.img
python -m ch32_image_tool.cli readback .\out\w25q64-readback.img
```

`test` 默认均匀抽样 5 个 4 KiB 块。每块都会先备份、写测试模式、回读，再通过
统一恢复路径写回并复核原数据。取消只在已恢复当前块之后生效。`--full` 会覆盖
全芯片；对 W25Q64 使用它会消耗每个扇区的擦写寿命，CLI 因此还要求显式给出
`--yes`，GUI 也会再次确认。断电或 USB 脱落仍可能让正在测试的当前块无法恢复。
CLI 在当前块操作期间收到 Ctrl+C 时也会先尝试恢复该块，再重新抛出中断。
刷写过程的取消不具备回滚能力；取消或断电后的 W25Q64 应视为不完整镜像并重新刷写。

## 测试

测试不需要连接硬件：

```powershell
python -m pip install -r requirements-dev.txt
python -m pytest
```

测试覆盖固定布局、manifest/镜像损坏检测、协议精确字节布局、序列号/状态/CRC
检查，以及双芯片测试后的数据恢复。
