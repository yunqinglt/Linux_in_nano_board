# Retained Firmware HEX Files

These are the three compiled MCU firmware variants retained for the project:

| File | Role |
| --- | --- |
| `CH32V203F6P-product.hex` | Current Product build. The same bytes are copied to `obj/CH32V203F6P.hex` for MounRiver Studio. |
| `CH32V203F6P-debug.hex` | Debug build with USART2 command-line support. |
| `CH32V203F6P-usb-flasher-v1.0.0.hex` | Archived USB Vendor maintenance build. |

The USB flasher is also retained at `flash/release/` with its original release
metadata. SHA-256 values for all three files are in `SHA256SUMS`.

Product firmware uses USB CDC for output and does not use COM19/USART1. Debug
firmware is the compatibility image for the earlier serial command-line
workflow.
