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
- `boot` validates the fixed 8 MiB W25Q64 image manifest and payload CRC32s,
  loads the MIPS ELF, DTB, and initramfs into APS6404L, patches initramfs FDT
  properties, sets the Linux entry and argument registers, and starts bounded
  guest slices from the main loop.
- USB CDC ACM is serviced between guest slices. USART2 remains the interactive
  command line.
- Build with `tools/Build-Firmware.ps1`; MounRiver's configured download path
  remains `obj/CH32V203F6P.hex`.

## Verification record

- Firmware build succeeded: text 30,812 bytes, data 240 bytes, BSS 6,232 bytes.
- Linker memory: 32 KiB Flash, 10 KiB SRAM, 2 KiB reserved stack.
- Hex SHA-256: `875483EF30FFD4D4911F3A5F221F707B08C2FC58FA9A55CDB6513AB2BC787F59`.
- Hardware execution of `boot` and guest Linux remains pending manual flashing.
- To flash and wait for the boot banner, run
  `tools/Read-WchLinkSerial.ps1 -Command boot -ExpectedPattern '^Linux boot at'`.
