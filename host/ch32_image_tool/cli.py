"""Command-line frontend for scripting and recovery."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Callable

from .device import MaintenanceDevice
from .image import IMAGE_SIZE, build_image, inspect_image
from .operations import (
    CancellationToken,
    flash_image,
    readback_image,
    test_chip_integrity,
    verify_image,
)
from .protocol import DEFAULT_PID, DEFAULT_VID
from .transport import UsbBulkTransport, UsbDeviceInfo, enumerate_devices


def _integer(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {text}") from error


class ProgressPrinter:
    def __init__(self) -> None:
        self.last_percent = -1

    def __call__(self, completed: int, total: int, message: str) -> None:
        percent = min(100, (completed * 100 // total) if total else 100)
        if percent != self.last_percent:
            print(f"\r{percent:3d}%  {message:<62}", end="", flush=True)
            self.last_percent = percent

    def finish(self) -> None:
        if self.last_percent >= 0:
            print()


def _select_device(args: argparse.Namespace) -> UsbDeviceInfo:
    devices = enumerate_devices(args.vid, args.pid)
    if args.serial:
        devices = [device for device in devices if device.serial_number == args.serial]
    if not devices:
        serial = f" with serial {args.serial!r}" if args.serial else ""
        raise RuntimeError(
            f"no USB device {args.vid:04X}:{args.pid:04X}{serial} found"
        )
    if len(devices) > 1 and not args.serial:
        raise RuntimeError("multiple devices found; select one with --serial")
    return devices[0]


def _with_device(
    args: argparse.Namespace, action: Callable[[MaintenanceDevice], object]
) -> object:
    info = _select_device(args)
    with UsbBulkTransport.open(
        info, vid=args.vid, pid=args.pid, timeout_ms=args.timeout
    ) as transport:
        device = MaintenanceDevice(transport, timeout_ms=args.timeout)
        return action(device)


def _print_capabilities(device: MaintenanceDevice) -> None:
    info = device.get_info()
    print(f"Firmware:       {info.firmware_version}")
    print(f"W25Q64:         {info.flash_size:,} bytes, ID {info.flash_id_text}")
    print(f"APS6404L:       {info.psram_size:,} bytes, ID {info.psram_id_text}")
    print(f"Erase/page:     {info.erase_size}/{info.page_size} bytes")
    print(f"USB transfer:   {info.max_transfer} bytes")
    print(f"Capabilities:   0x{info.flags:04X}")


def _run_progress(action: Callable[[ProgressPrinter], object]) -> object:
    printer = ProgressPrinter()
    try:
        return action(printer)
    finally:
        printer.finish()


def _cmd_list(args: argparse.Namespace) -> int:
    devices = enumerate_devices(args.vid, args.pid)
    if not devices:
        print(f"No devices matching {args.vid:04X}:{args.pid:04X}.")
        return 1
    for device in devices:
        print(device.label)
    return 0


def _cmd_info(args: argparse.Namespace) -> int:
    _with_device(args, lambda device: _print_capabilities(device))
    return 0


def _cmd_pack(args: argparse.Namespace) -> int:
    manifest = _run_progress(
        lambda progress: build_image(args.resources, args.output, progress)
    )
    print(f"Created {args.output} ({manifest.image_size:,} bytes)")
    for entry in manifest.entries:
        print(
            f"  {entry.name:<10} @ 0x{entry.offset:06X}: "
            f"{entry.data_size:,}/{entry.slot_size:,} bytes, CRC {entry.crc32:08X}"
        )
    print(f"Image CRC32: {manifest.image_crc32:08X}")
    return 0


def _cmd_inspect(args: argparse.Namespace) -> int:
    manifest = inspect_image(args.image)
    print(f"Image: {args.image} ({manifest.image_size:,} bytes)")
    for entry in manifest.entries:
        print(
            f"  {entry.name:<10} @ 0x{entry.offset:06X}: "
            f"{entry.data_size:,}/{entry.slot_size:,} bytes, CRC {entry.crc32:08X}"
        )
    print(f"Manifest CRC32: {manifest.manifest_crc32:08X}")
    print(f"Image CRC32:    {manifest.image_crc32:08X}")
    return 0


def _cmd_flash(args: argparse.Namespace) -> int:
    token = CancellationToken()

    def action(device: MaintenanceDevice) -> object:
        return _run_progress(
            lambda progress: flash_image(
                device,
                args.image,
                verify=not args.no_verify,
                token=token,
                progress=progress,
            )
        )

    result = _with_device(args, action)
    print(
        f"Flashed {result.bytes_processed:,} bytes"
        + (" and verified readback." if not args.no_verify else ".")
    )
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    result = _with_device(
        args,
        lambda device: _run_progress(
            lambda progress: verify_image(device, args.image, progress=progress)
        ),
    )
    print(f"Readback matches all {result.bytes_processed:,} bytes.")
    return 0


def _cmd_readback(args: argparse.Namespace) -> int:
    result = _with_device(
        args,
        lambda device: _run_progress(
            lambda progress: readback_image(device, args.output, progress=progress)
        ),
    )
    print(
        f"Read {result.bytes_processed:,} bytes to {args.output}; "
        f"CRC32 {result.crc32:08X}"
    )
    return 0


def _cmd_test(args: argparse.Namespace) -> int:
    if args.full and args.chip in ("flash", "both") and not args.yes:
        raise RuntimeError(
            "full W25Q64 testing erases and restores every sector; add --yes "
            "to acknowledge flash endurance consumption"
        )
    chips = ("flash", "psram") if args.chip == "both" else (args.chip,)

    def action(device: MaintenanceDevice) -> object:
        results = []
        for chip in chips:
            result = _run_progress(
                lambda progress, selected=chip: test_chip_integrity(
                    device, selected, full=args.full, progress=progress
                )
            )
            results.append((chip, result))
        return results

    for chip, result in _with_device(args, action):
        print(f"{chip}: {result.blocks_tested} blocks passed and were restored.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ch32-image-cli",
        description="CH32V203 USB W25Q64/APS6404L maintenance tool",
    )
    parser.add_argument("--vid", type=_integer, default=DEFAULT_VID)
    parser.add_argument("--pid", type=_integer, default=DEFAULT_PID)
    parser.add_argument("--serial", help="select a device by USB serial number")
    parser.add_argument("--timeout", type=int, default=5_000, help="USB timeout in ms")
    commands = parser.add_subparsers(dest="command", required=True)

    list_parser = commands.add_parser("list", help="list matching USB devices")
    list_parser.set_defaults(handler=_cmd_list)
    info_parser = commands.add_parser("info", help="read firmware/chip information")
    info_parser.set_defaults(handler=_cmd_info)

    pack = commands.add_parser("pack", help="build the fixed 8 MiB image")
    pack.add_argument("--resources", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    pack.set_defaults(handler=_cmd_pack)

    inspect = commands.add_parser("inspect", help="validate and show an image")
    inspect.add_argument("image", type=Path)
    inspect.set_defaults(handler=_cmd_inspect)

    flash = commands.add_parser("flash", help="erase/program/readback an image")
    flash.add_argument("image", type=Path)
    flash.add_argument("--no-verify", action="store_true")
    flash.set_defaults(handler=_cmd_flash)

    verify = commands.add_parser("verify", help="compare flash with an image")
    verify.add_argument("image", type=Path)
    verify.set_defaults(handler=_cmd_verify)

    readback = commands.add_parser("readback", help="save all 8 MiB from W25Q64")
    readback.add_argument("output", type=Path)
    readback.set_defaults(handler=_cmd_readback)

    test = commands.add_parser("test", help="non-destructively test and restore chips")
    test.add_argument("--chip", choices=("flash", "psram", "both"), default="both")
    test.add_argument("--full", action="store_true", help="test every 4 KiB block")
    test.add_argument("--yes", action="store_true", help="acknowledge full NOR wear")
    test.set_defaults(handler=_cmd_test)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
