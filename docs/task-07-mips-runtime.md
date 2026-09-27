# Task 07: MIPS Runtime on CH32V203F6P6

## APS6404L memory callbacks

The emulator retains its byte-range `platform_memory_ops_t` backend. The six
guest memory helpers are wired through it as follows:

| Guest helper | Bus width | APS6404L backend operation |
| --- | ---: | --- |
| `read8` | 1 byte | `psram_memory_read(pa, ..., 1)` |
| `read16` | 2 bytes | `psram_memory_read(pa, ..., 2)` |
| `read32` | 4 bytes | `psram_memory_read(pa, ..., 4)` |
| `write8` | 1 byte | `psram_memory_write(pa, ..., 1)` |
| `write16` | 2 bytes | `psram_memory_write(pa, ..., 2)` |
| `write32` | 4 bytes | `psram_memory_write(pa, ..., 4)` |

`platform_bus_read()` and `platform_bus_write()` perform the little-endian
width conversion and call the configured raw-memory backend. The board backend
uses `SpiStorage_PsramRead()` / `SpiStorage_PsramWrite()` and also provides a
bounded fill callback for ELF BSS initialization. Non-aligned halfword and
word accesses remain byte-addressed. Reads use four 32-byte cache lines to
coalesce sequential instruction fetches; writes invalidate overlapping lines.
SPI failures latch their physical address and stop the guest after the current
execution slice.

The engine regression `platform_memory_test` covers all six helpers, including
unaligned 16- and 32-bit accesses. Host suite result: 10/10 tests passed.

## Runtime and handoff

- A static `Registers` object backs the emulator context; the runtime keeps its
  pointer for all run slices.
- The runtime validates the fixed 8 MiB W25Q64 image manifest and payload CRC32s,
  loads the MIPS ELF, DTB, and initramfs into APS6404L, patches initramfs FDT
  properties, sets the Linux entry and argument registers, and starts bounded
  guest slices from the main loop.
- Product builds report manifest, CRC, PT_LOAD, DTB and initramfs progress over
  USB CDC; the CDC log queue is separate from the guest 16550 FIFO.
- `Product/main.c` disables and resets USART1/2/3 before USB or storage setup;
  it does not initialize them. The original USART command line remains
  available only in the `Debug` profile; see
  [task-08-product-boot.md](task-08-product-boot.md).
- Build with `tools/Build-Firmware.ps1 -Profile Product`; MounRiver's configured
  download path remains `obj/CH32V203F6P.hex`.

## Verification record

- Product firmware build succeeded: text 27,756 bytes, data 232 bytes, BSS
  7,804 bytes.
- Product HEX SHA-256: `BB137D22D717FDD1EECE44B27CD43D3BEE376C2FA5807C522DFB1F47BBA4BC21`.
- Linker memory: 32 KiB Flash, 10 KiB SRAM, 2 KiB reserved stack.
- Hardware verification requires manual flashing of the generated HEX. To
  listen for the product boot result, run
  `tools/Read-WchLinkSerial.ps1 -Command '' -ExpectedPattern 'Linux boot at|ERROR:'`.
