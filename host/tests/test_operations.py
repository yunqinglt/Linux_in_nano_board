from __future__ import annotations

from dataclasses import replace

import pytest

from ch32_image_tool.operations import (
    CancellationToken,
    OperationCancelled,
    test_chip_integrity as run_chip_test,
)
from ch32_image_tool.protocol import DeviceCapabilities


class MemoryDevice:
    def __init__(self) -> None:
        self.info = DeviceCapabilities(
            flash_size=32 * 1024,
            psram_size=32 * 1024,
            erase_size=4096,
            max_transfer=4096,
            page_size=256,
            flags=0xF,
            firmware_major=1,
            firmware_minor=0,
            flash_id=b"\xef\x40\x17\0",
            psram_id=b"\x0d\x5d\x52\xd6",
        )
        self.flash = bytearray((index * 13) & 0xFF for index in range(32 * 1024))
        self.psram = bytearray((index * 29) & 0xFF for index in range(32 * 1024))

    def get_info(self) -> DeviceCapabilities:
        return self.info

    def read_flash(self, address: int, length: int) -> bytes:
        return bytes(self.flash[address : address + length])

    def write_flash(self, address: int, payload: bytes) -> None:
        # NOR can only clear bits without an erase.
        for index, value in enumerate(payload):
            self.flash[address + index] &= value

    def erase_flash(self, address: int, length: int) -> None:
        self.flash[address : address + length] = b"\xff" * length

    def read_psram(self, address: int, length: int) -> bytes:
        return bytes(self.psram[address : address + length])

    def write_psram(self, address: int, payload: bytes) -> None:
        self.psram[address : address + len(payload)] = payload


class InterruptingFlashDevice(MemoryDevice):
    """在首次 Flash 写入已经生效后模拟用户按下 Ctrl+C。"""

    def __init__(self) -> None:
        super().__init__()
        self.interrupt_flash_write = True

    def write_flash(self, address: int, payload: bytes) -> None:
        super().write_flash(address, payload)
        if self.interrupt_flash_write:
            self.interrupt_flash_write = False
            raise KeyboardInterrupt


class InterruptingPsramDevice(MemoryDevice):
    """在首次 PSRAM 写入已经生效后模拟用户按下 Ctrl+C。"""

    def __init__(self) -> None:
        super().__init__()
        self.interrupt_psram_write = True

    def write_psram(self, address: int, payload: bytes) -> None:
        super().write_psram(address, payload)
        if self.interrupt_psram_write:
            self.interrupt_psram_write = False
            raise KeyboardInterrupt


def test_quick_integrity_tests_restore_both_chips() -> None:
    device = MemoryDevice()
    original_flash = bytes(device.flash)
    original_psram = bytes(device.psram)

    flash_result = run_chip_test(device, "flash")  # type: ignore[arg-type]
    psram_result = run_chip_test(device, "psram")  # type: ignore[arg-type]

    assert flash_result.blocks_tested == 5
    assert psram_result.blocks_tested == 5
    assert bytes(device.flash) == original_flash
    assert bytes(device.psram) == original_psram


def test_cancel_before_first_block_does_not_modify_memory() -> None:
    device = MemoryDevice()
    original = bytes(device.flash)
    token = CancellationToken()
    token.cancel()

    with pytest.raises(OperationCancelled):
        run_chip_test(device, "flash", token=token)  # type: ignore[arg-type]
    assert bytes(device.flash) == original


def test_keyboard_interrupt_restores_current_flash_block() -> None:
    device = InterruptingFlashDevice()
    original = bytes(device.flash)

    with pytest.raises(KeyboardInterrupt):
        run_chip_test(device, "flash")  # type: ignore[arg-type]

    assert bytes(device.flash) == original


def test_keyboard_interrupt_restores_current_psram_block() -> None:
    device = InterruptingPsramDevice()
    original = bytes(device.psram)

    with pytest.raises(KeyboardInterrupt):
        run_chip_test(device, "psram")  # type: ignore[arg-type]

    assert bytes(device.psram) == original


def test_each_chip_can_be_tested_when_the_other_is_absent() -> None:
    flash_only = MemoryDevice()
    flash_only.info = replace(flash_only.info, flags=0x5)
    run_chip_test(flash_only, "flash")  # type: ignore[arg-type]

    psram_only = MemoryDevice()
    psram_only.info = replace(psram_only.info, flags=0x2)
    run_chip_test(psram_only, "psram")  # type: ignore[arg-type]
