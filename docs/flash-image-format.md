# W25Q64 固定镜像格式

本文档是下位机从 W25Q64 直接读取 MIPS 模拟器资源时使用的格式
合同。格式实现位于 `flash/host/ch32_image_tool/image.py`，本文记录的发布样本为
`flash/host/out/mipsel-emu-8m.img`。经过真机验证的冻结副本位于
`flash/release/mipsel-emu-8m-v1.img`。

> 镜像布局中的 offset 和 slot 是 version 1 的固定 ABI；`data_size`、
> payload CRC32 和 SHA-256 是本次已验证发布镜像的指纹。以后替换
> `res/` 内容时，后三者可随 payload 变化，读取器应从 manifest 取值。

## 镜像几何与字节序

| 属性 | 值 |
|---|---:|
| 总大小 | `8,388,608 B` / `0x00800000` |
| 外部 Flash | W25Q64，镜像偏移 `0` 对应 Flash 地址 `0` |
| 擦除块 | `4,096 B` / `0x1000` |
| 声明的传输缓冲区 | `4,096 B` / `0x1000` |
| 未使用字节 | `0xFF` |
| 整数字节序 | little-endian |
| Manifest 版本 | `1` |
| 文件项数 | `3` |

Manifest 占用第一个 `64 KiB` 槽，范围为
`0x000000..0x00FFFF`。其有效记录共 `0x100` 字节，范围为
`0x000000..0x0000FF`；`0x000100..0x00FFFF` 必须全部为 `0xFF`。

## Manifest 固定头

固定头为 64 字节，等价于 Python `struct` 格式
`<8sHHIIIIII28s`。

| 绝对偏移 | 大小 | 字段 | 格式/当前值 |
|---:|---:|---|---|
| `0x00` | 8 | `magic` | 字节 `4D 49 50 53 49 4D 47 00`，即 `MIPSIMG\0` |
| `0x08` | 2 | `version` | `1` |
| `0x0A` | 2 | `header_size` | `0x0100`；包含固定头和 3 个文件项 |
| `0x0C` | 4 | `image_size` | `0x00800000` |
| `0x10` | 4 | `erase_size` | `0x00001000` |
| `0x14` | 4 | `transfer_size` | `0x00001000` |
| `0x18` | 4 | `entry_count` | `3` |
| `0x1C` | 4 | `manifest_crc32` | `0x6C192C43` |
| `0x20` | 4 | `image_crc32` | `0x4A01555E` |
| `0x24` | 28 | `reserved` | 必须全零 |

## 文件项的二进制布局

每个文件项为 64 字节，等价于 Python `struct` 格式
`<16sIIII32s`。三个项的起始位置分别为 `0x40`、`0x80` 和
`0xC0`。

| 项内偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| `+0x00` | 16 | `name` | ASCII 名称，后面用 NUL 填充；名称最长 15 字节 |
| `+0x10` | 4 | `offset` | payload 相对 W25Q64 起点的字节偏移 |
| `+0x14` | 4 | `slot_size` | 为该文件保留的总容量 |
| `+0x18` | 4 | `data_size` | payload 实际长度 |
| `+0x1C` | 4 | `crc32` | 仅覆盖 `data_size` 字节的 CRC32 |
| `+0x20` | 32 | `sha256` | payload 的 32 字节原始 SHA-256，不是 ASCII hex |

Manifest 内文件项的固定顺序为 `vmlinux`、`dtb`、`initramfs`。
版本 1 读取器应同时核对名称、offset 和 slot_size，不应接受任意
重排或重叠的项。

## 固定文件和已验证发布指纹

| Manifest 名称 | `res/` 源文件 | 文件项位置 | Flash offset | Slot 大小 | Slot 范围（含末字节） |
|---|---|---:|---:|---:|---|
| `vmlinux` | `vmlinux` | `0x40` | `0x010000` | `0x600000` | `0x010000..0x60FFFF` |
| `dtb` | `mipsel-emu-embedded.dtb` | `0x80` | `0x610000` | `0x010000` | `0x610000..0x61FFFF` |
| `initramfs` | `initramfs.cpio.gz` | `0xC0` | `0x620000` | `0x1E0000` | `0x620000..0x7FFFFF` |

| Manifest 名称 | 实际长度 | 数据范围（含末字节） | 填充起点 | 填充长度 | CRC32 |
|---|---:|---|---:|---:|---:|
| `vmlinux` | `2,636,940` / `0x283C8C` | `0x010000..0x293C8B` | `0x293C8C` | `3,654,516` / `0x37C374` | `0xBC9332D5` |
| `dtb` | `1,360` / `0x550` | `0x610000..0x61054F` | `0x610550` | `64,176` / `0xFAB0` | `0xF8F212A3` |
| `initramfs` | `153,051` / `0x255DB` | `0x620000..0x6455DA` | `0x6455DB` | `1,813,029` / `0x1BAA25` | `0xB8623D5D` |

| Manifest 名称 | SHA-256 |
|---|---|
| `vmlinux` | `76F43DC437671C038A244103BA081D9F870652911A690360BD8218D33CC9E43B` |
| `dtb` | `2ACEFC6B03E5E373BBF44C98780B73692EB5B82B896A567C1FDE4844EC26EBA2` |
| `initramfs` | `299765475930B283E6011C4948D4BD977FC9D04DE10CF19E7C28967295FFE248` |

每个文件的 `data_size` 之后直到 slot 结束必须全部为 `0xFF`。
三个 slot 在 manifest 之后连续覆盖 `0x010000..0x7FFFFF`，彼此不
重叠且中间没有空洞。

### 整镜指纹

- 发布镜像 SHA-256：
  `5D5DA3F3445AF5FEEBF968FC9DC7E7E5BF9374326AE3FCB252B7F48805D4400C`；
- 从真机 W25Q64 独立回读的 8 MiB 内容与该镜像逐字节一致；
- 对最终文件直接计算的普通 CRC32 为 `0x413CD9EC`。这个值不是
  manifest 中的 `image_crc32`，不能用来与 `0x4A01555E` 直接比较。

## CRC32 定义与覆盖范围

所有 CRC 使用 Python `zlib.crc32()` 对应的标准反射 CRC-32/ISO-HDLC：

- 正向多项式 `0x04C11DB7`，反射实现多项式 `0xEDB88320`；
- 原始初值 `0xFFFFFFFF`，最终异或 `0xFFFFFFFF`；
- 按字节反射处理，最终保留 32 位无符号值；
- 使用 zlib 的流式 API 时，第一块传入的对外初值为 `0`，后续块
  传入上一块返回值。

### Payload CRC32

从文件项的 `offset` 开始，仅覆盖 `data_size` 字节。不包含
slot 后部的 `0xFF` 填充。

### 整镜 CRC32

覆盖完整 `0x800000` 字节。计算时必须将以下 8 个字节当作零：

- `0x1C..0x1F`：`manifest_crc32`；
- `0x20..0x23`：`image_crc32`。

得到的值为 `0x4A01555E`。镜像中的 manifest、payload 和所有
`0xFF` 填充都参与计算。

### Manifest CRC32

仅覆盖 `header_size`，即 `0x000000..0x0000FF`。计算时仅将
`0x1C..0x1F` 的 `manifest_crc32` 当作零；`0x20..0x23` 保留已写入的
`image_crc32=0x4A01555E`。得到的值为 `0x6C192C43`。Manifest slot
中 `0x100` 之后的填充不参与 manifest CRC。

为解除两个 CRC 字段之间的循环依赖，打包顺序必须是：

1. 先将两个 CRC 字段全部写零，计算整镜 CRC；
2. 写入 `image_crc32`，保持 `manifest_crc32` 为零；
3. 对前 `header_size` 字节计算 manifest CRC；
4. 写入 `manifest_crc32`。

CH32V203 默认硬件 CRC 的位序、字宽和初值未必与 zlib 一致。
移植时应先用本文中的三个 payload CRC 作为测试向量；未验证前不要
直接使用默认硬件 CRC 结果。

## 下一轮下位机读取要求

1. 启动时先从 W25Q64 偏移 `0` 读取前 `0x100` 字节，不需要把
   8 MiB 镜像装入 MCU RAM。
2. 依次校验 magic、version、`header_size`、镜像几何、项数、reserved
   和 manifest CRC，再使用文件项。
3. 使用显式小端读取函数或 `memcpy` 解析多字节整数，不要依赖
   C 编译器的结构体填充、别名或非对齐访问行为。
4. 核对三个项的固定名称、offset 和 slot_size。所有加法前先检查
   `data_size <= slot_size`、`offset <= image_size` 以及
   `slot_size <= image_size - offset`，避免 32 位溢出。
5. 模拟器文件抽象应实现为流式回调：Flash 地址为
   `entry.offset + file_offset`，并严格用 `data_size` 做边界检查。不要把
   `slot_size` 或后面的 `0xFF` 当作文件长度。
6. `vmlinux`、DTB 和 gzip initramfs 都是原文件字节的直接拷贝，
   外层容器不对 payload 做转码、解压或字节序变换。
7. Payload CRC 和整镜 CRC 可按 `0x1000` 字节分块流式计算。
   `transfer_size` 是打包/维护协议几何，普通 SPI 随机读取可以使用更小块。
8. 启动热路径可以只校验 manifest，然后在首次使用某个 payload 时
   流式校验其 CRC。全 8 MiB CRC 适合发布自检或诊断命令，不必在
   每次随机读取前重复计算。
9. 移除刷写功能后，运行时数据路径应只向 W25Q64 发出读取命令；
   文件项和 manifest 不得在运行时被改写。

## 发布保留要求

`flash/host/out/` 被 `flash/host/.gitignore` 忽略，因此其中的工作镜像不会随普通
Git 提交保留。发布时应把已验证的 8 MiB 原始镜像复制到专用的
非忽略发布目录，并与上述整镜 SHA-256 一起保留。

外部 Flash 产物是严格 8 MiB 的原始二进制镜像，不是 Intel HEX。
CH32V203 内部 Flash 使用的 `.hex` 固件是另一个独立发布物，两者应
分别命名和校验，避免在刷写时混淆。
