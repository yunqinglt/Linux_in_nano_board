# Linux in Nano Board

Run a mainline Linux kernel and BusyBox on a nano board measuring only
0.47 x 1 inch.

The board is built around the WCH CH32V203F6P6, a QingKe V4 / RV32IMAC
microcontroller. The MCU provides the boot and emulation runtime; an external
W25Q64 NOR flash stores the fixed Linux image and an APS6404L PSRAM provides the
guest memory backend.

## Hardware

| Function | Connection |
| --- | --- |
| W25Q64 chip select | PA0 |
| APS6404L chip select | PA1 |
| SPI1 SCK / MISO / MOSI | PA5 / PA6 / PA7 |
| USB D- / D+ | PA11 / PA12 |
| WCH-Link SDI | PA13 / PA14 |
| Debug USART (Debug profile only) | PA2 / PA3, 115200 8N1 |
| BOOT0 | PB8 switch |

The Product firmware deliberately stops USART1/2/3 before initialization. Its
diagnostics and the emulated guest 16550 UART are exposed through USB CDC ACM.
The historical Debug firmware keeps the USART2 command line for board bring-up.

## Repository Layout

- `Product/`: release boot path; USB CDC diagnostics, storage checks and Linux
  handoff.
- `User/`: shared storage and MIPS runtime code, plus the preserved Debug entry.
- `Debug/` and `lib/`: USART command-line implementation used by the Debug
  profile.
- `flash/`: archived USB Vendor flasher, PyQt6/PyUSB/libusb1 host utility,
  fixed image layout and hardware-verified release image.
- `release/firmware/`: the three retained MCU HEX variants.
- `third_party/`: Git submodules for the MIPS engine, OpenWCH CH32 support and
  CherryUSB.
- `docs/`: hardware evidence, protocol notes, image ABI and task records.
- `res/`: Linux kernel, device tree and initramfs inputs used by the image
  packer.

## Firmware Variants

| Variant | File | Purpose |
| --- | --- | --- |
| Product | `release/firmware/CH32V203F6P-product.hex` | USB CDC boot product; also copied to the fixed MounRiver path `obj/CH32V203F6P.hex` |
| Debug | `release/firmware/CH32V203F6P-debug.hex` | USART2 command-line bring-up and SPI probe |
| USB flasher | `release/firmware/CH32V203F6P-usb-flasher-v1.0.0.hex` | Archived `1209:2036` Vendor maintenance firmware |

SHA-256 values are recorded in `release/firmware/SHA256SUMS`. The Product HEX
is the default artifact and is the one MounRiver Studio is configured to load.

## Building

The build script uses the RISC-V toolchain shipped with MounRiver Studio 2 and
keeps the output path expected by the IDE:

```powershell
.\tools\Build-Firmware.ps1 -Profile Product
.\tools\Build-Firmware.ps1 -Profile Debug
```

The Product build excludes the USART command-line sources. The Debug build
includes them and restores the `help`/`probe` bring-up workflow. The generated
Product image is written to `obj/CH32V203F6P.hex`; copy it to a board with
MounRiver Studio and reset the board.

## Product Boot Flow

1. Stop and reset all on-chip USART peripherals.
2. Start USB CDC and print the chip ID and external storage IDs.
3. Require W25Q64 `EF 40 17` and APS6404L manufacturer/device IDs `0D 5D`.
4. Wait briefly while servicing USB, then validate the fixed 8 MiB image
   manifest and CRC32 values.
5. Stream ELF `PT_LOAD` segments, the DTB and initramfs from W25Q64 into
   APS6404L.
6. Set the MIPS entry and boot arguments, print `Linux boot at ...`, and run
   bounded guest slices while forwarding the emulated 16550 UART over CDC.

Product logs are not sent to WCH-Link COM19. After flashing, select the new USB
CDC ACM COM port and listen with:

```powershell
.\tools\Read-WchLinkSerial.ps1 -Port COM<CDC> -Command '' `
  -ExpectedPattern 'Linux boot at|ERROR:'
```

## Image and Host Tool

The image is exactly 8 MiB with a 64 KiB manifest, fixed slots for the Linux
ELF, DTB and initramfs, per-file CRC/SHA-256 values and a whole-image CRC. The
layout is documented in `docs/flash-image-format.md` and implemented by
`flash/host/ch32_image_tool`.

For the archived maintenance firmware:

```powershell
cd .\flash\host
python -m pip install -r requirements-dev.txt
python -m pytest
python -m ch32_image_tool.cli inspect ..\..\flash\release\mipsel-emu-8m-v1.img
```

The Vendor flasher is intentionally kept separate from the Product firmware.
Only the archived flasher enumerates USB VID:PID `1209:2036`.

## Submodules

Initialize the source dependencies after cloning:

```powershell
git submodule update --init --recursive
```

The project records the exact submodule revisions used for the MIPS engine and
WCH USB/MCU support. The locally installed MounRiver OpenOCD remains the
preferred WCH-Link backend; see `docs/task-01-wch-link-openocd.md`.

## Status

The storage probe has been exercised on hardware with W25Q64 `EF 40 17`,
APS6404L `0D 5D 52D6` and chip ID `20370500`. Product and Debug firmware builds
are reproducible from the checked-in script. End-to-end Linux execution still
requires manual board flashing and USB CDC observation because the board's
programming step is currently performed through MounRiver Studio.

## License and Upstream Sources

This repository contains board-specific glue and build records. Third-party
components retain their upstream licenses; consult each submodule and archived
source tree before redistribution.
