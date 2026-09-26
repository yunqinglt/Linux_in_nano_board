# Task 3: SPI device probe

The board map in `pin-configuration.txt` is:

| Signal | MCU pin | Function |
| --- | --- | --- |
| CS0 | PA0 | W25Q64JVXGIQ |
| CS1 | PA1 | APS6404L-3QSR-ZR |
| SCK | PA5 | SPI1 clock |
| MISO | PA6 | SPI1 input |
| MOSI | PA7 | SPI1 output |
| TX | PA2 | USART2 TX to WCH-Link COM19 |

The probe is intentionally conservative: SPI1 mode 0, MSB first, software chip
selects, and the `/256` prescaler. Internal software NSS is held high for SPI
master mode, while PA0/PA1 provide the external chip-selects. It only sends
reset/read-ID commands and does not write or erase either memory.

Expected responses:

- W25Q64: `0x9F` followed by `EF 40 17`.
- APS6404L: `0x9F`, three zero address bytes, then `MF=0D` and `KGD=5D`.
  `KGD=55` is the documented default/fail value. Six additional bytes are
  printed as a raw EID sample.

The current entry point prints two lines similar to:

```text
SPI1 W25Q64 CS0: transfer=ok present=yes id=EF 40 17 expected=yes
SPI1 APS6404L CS1: transfer=ok present=yes mf=0D kgd=5D eid=... expected=yes
```

Hardware validation on 2026-09-26 succeeded after manually programming
`obj/CH32V203F6P.hex` through MounRiver. The board reported:

```text
SystemClk:8000000
ChipID:20370500
SPI1 W25Q64 CS0: transfer=ok present=yes id=EF 40 17 expected=yes
SPI1 APS6404L CS1: transfer=ok present=yes mf=0D kgd=5D eid=52D606646B55 expected=yes
```

This confirms both chip-select paths, the shared SPI1 wiring, and USART2 TX
output through WCH-Link `COM19`.

`tools/Read-WchLinkSerial.ps1` opens COM19 as 115200 8N1 with no flow control,
starts a 20-second timeout, and stops the timeout after both probe lines are
received. Start it before manually programming the generated HEX file:

```powershell
.\tools\Read-WchLinkSerial.ps1
```

The existing `pin-configuration.txt` is a local hardware note and remains
untracked; it is deliberately not included in the task commit.
