# Task 04: USART command line

The command-line library in `lib/` is integrated with the CH32V203 firmware.
USART2 uses PA2 (TX) and PA3 (RX) at 115200 baud, 8 data bits, no parity, one
stop bit, and no flow control.

## Registered commands

- `help`: print the mounted command table.
- `probe`: reset/read the APS6404L ID and read the W25Q64 JEDEC ID.

`probe` retains the output format established by task 03 so the serial helper
can recognize both devices. The APS6404L reset sequence can destroy active
PSRAM contents, so this diagnostic command must not be used while that memory
holds simulator state.

## Serial verification

The fixed programming image is:

`C:\Users\云晴\mounriver-studio-projects\CH32V203F6P\obj\CH32V203F6P.hex`

The MounRiver project configuration includes `lib/`. A command-line rebuild
using the same WCH GCC target flags is also available:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-Firmware.ps1
```

Start the 20-second receiver before programming the board manually:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Read-WchLinkSerial.ps1 -Command probe
```

The receiver waits for the `ch32> ` prompt, transmits `probe`, and exits early
after both SPI ID lines are received.

## Hardware result

Verified on COM19 at 115200 baud on 2026-09-26. The command was received over
PA3 and produced:

```text
ch32> probe
probe: APS6404L is reset before its ID is read
SPI1 W25Q64 CS0: transfer=ok present=yes id=EF 40 17 expected=yes
SPI1 APS6404L CS1: transfer=ok present=yes mf=0D kgd=5D eid=52D606646B55 expected=yes
```

`help`, excess-argument rejection, and unknown-command reporting were also
verified interactively.

## QingKe interrupt-port note

CH32V20x startup returns into `main()` in user mode. Reading machine CSR
`mstatus` from the original library therefore raised illegal-instruction
exception 2 at run time. The port saves and restores interrupt state through
WCH user CSR `0x800`, matching the vendor `__enable_irq()` and
`__disable_irq()` implementation.
