# CH32V203F6P6 独立刷写产品

本目录冻结已经过真机验证的 USB Vendor 刷写产品。根目录下的活动固件不再链接
Vendor 刷写协议，以便下一轮加入 USB CDC、USART `boot` 命令和 MIPS 模拟器；
这里的源码、上位机和发布文件仍完整保留，随时可以恢复或独立重建。

## 目录

- `release/CH32V203F6P-usb-flasher-v1.0.0.hex`：写入 MCU 内部 Flash 的
  MounRiver/WCH-Link 发布固件；
- `release/mipsel-emu-8m-v1.img`：已经写入 W25Q64 并逐字节回读验证的原始
  8 MiB 镜像；它不是 Intel HEX；
- `release/SHA256SUMS`、`release/release.json`：冻结产物校验与真机验证元数据；
- `firmware/User/`：生成上述 HEX 时的完整 `User` 源码快照；
- `firmware/Build-Flasher.ps1`：只从归档源码构建到 `flash/build/`，不会覆盖
  根目录 `obj/` 或冻结的 `release/`；
- `host/`：PyQt6、PyUSB、libusb1 上位机、CLI 与离线测试；
- `image-layout.md`：下一轮 loader 使用的固定镜像 ABI、地址和校验规则。

## 重建刷写固件

```powershell
cd C:\Users\云晴\mounriver-studio-projects\CH32V203F6P
.\flash\firmware\Build-Flasher.ps1
Get-FileHash .\flash\build\CH32V203F6P.hex -Algorithm SHA256
```

重建结果必须为：

`DB78754578653C914459BFAA952AE4AB858E69AD52C57E1B668B55C681C50542`

## 使用上位机

```powershell
cd C:\Users\云晴\mounriver-studio-projects\CH32V203F6P\flash\host
python -m pytest
python -m ch32_image_tool.cli list
python -m ch32_image_tool.cli info
python -m ch32_image_tool.cli pack `
  --resources ..\..\res `
  --output .\out\mipsel-emu-8m.img
```

需要重新进入维护/刷写模式时，先用 MounRiver Studio 手动下载
`release/CH32V203F6P-usb-flasher-v1.0.0.hex`，再运行这里的上位机。只有这个
固件枚举 `1209:2036` Vendor 接口；根目录的活动固件不再包含该接口。
