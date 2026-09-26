"""High-level cancellable image and memory operations."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from threading import Event
from typing import Callable, Literal
import zlib

from .device import MaintenanceDevice
from .image import (
    ERASE_SIZE,
    IMAGE_SIZE,
    TRANSFER_BUFFER_SIZE,
    ImageFormatError,
    inspect_image,
)
from .protocol import (
    CAP_FLASH_ERASE,
    CAP_FLASH_PRESENT,
    CAP_PSRAM_PRESENT,
    DeviceCapabilities,
)


Progress = Callable[[int, int, str], None]
Chip = Literal["flash", "psram"]


class OperationCancelled(RuntimeError):
    """Raised at a safe block boundary after cancellation was requested."""


class VerificationError(RuntimeError):
    """Readback differs from the requested image or test pattern."""


class IntegrityTestError(RuntimeError):
    """A non-destructive integrity test failed or could not restore data."""


class CancellationToken:
    def __init__(self) -> None:
        self._event = Event()

    def cancel(self) -> None:
        self._event.set()

    @property
    def cancelled(self) -> bool:
        return self._event.is_set()

    def check(self) -> None:
        if self.cancelled:
            raise OperationCancelled("operation cancelled")


@dataclass(frozen=True, slots=True)
class OperationSummary:
    bytes_processed: int
    crc32: int | None = None
    blocks_tested: int | None = None


def _progress(callback: Progress | None, done: int, total: int, text: str) -> None:
    if callback:
        callback(done, total, text)


def _first_difference(expected: bytes, actual: bytes) -> int | None:
    for index, (left, right) in enumerate(zip(expected, actual)):
        if left != right:
            return index
    if len(expected) != len(actual):
        return min(len(expected), len(actual))
    return None


def _require_capabilities(
    device: MaintenanceDevice,
    *,
    flash: bool = False,
    psram: bool = False,
    erase: bool = False,
) -> DeviceCapabilities:
    info = device.get_info()
    if flash and not info.flags & CAP_FLASH_PRESENT:
        raise RuntimeError("firmware reports that W25Q64 is absent")
    if psram and not info.flags & CAP_PSRAM_PRESENT:
        raise RuntimeError("firmware reports that APS6404L is absent")
    if erase and info.erase_size != ERASE_SIZE:
        raise RuntimeError(
            f"unsupported erase geometry {info.erase_size}; expected {ERASE_SIZE}"
        )
    if erase and not info.flags & CAP_FLASH_ERASE:
        raise RuntimeError("firmware does not advertise SPI NOR erase support")
    if flash and info.flash_size < info.erase_size:
        raise RuntimeError("firmware reported an invalid SPI NOR capacity")
    if psram and info.psram_size < 64:
        raise RuntimeError("firmware reported an invalid PSRAM capacity")
    if info.max_transfer < 64:
        raise RuntimeError(f"device transfer limit is too small: {info.max_transfer}")
    return info


def _require_image_geometry(
    device: MaintenanceDevice, *, erase: bool = False
) -> DeviceCapabilities:
    info = _require_capabilities(device, flash=True, erase=erase)
    if info.flash_size < IMAGE_SIZE:
        raise RuntimeError(
            f"SPI NOR is only {info.flash_size:,} bytes; {IMAGE_SIZE:,} required"
        )
    return info


def _chunk_size(info: DeviceCapabilities) -> int:
    return min(TRANSFER_BUFFER_SIZE, info.max_transfer)


def flash_image(
    device: MaintenanceDevice,
    image_path: str | Path,
    *,
    verify: bool = True,
    token: CancellationToken | None = None,
    progress: Progress | None = None,
) -> OperationSummary:
    """Erase, program, and optionally byte-for-byte verify the 8 MiB image."""

    token = token or CancellationToken()
    inspect_image(image_path)
    image = Path(image_path).read_bytes()
    info = _require_image_geometry(device, erase=True)
    chunk_size = _chunk_size(info)
    stages = 3 if verify else 2
    total_work = IMAGE_SIZE * stages

    for address in range(0, IMAGE_SIZE, info.erase_size):
        token.check()
        device.erase_flash(address, info.erase_size)
        _progress(
            progress,
            address + info.erase_size,
            total_work,
            f"Erasing W25Q64 at 0x{address:06X}",
        )

    stage_base = IMAGE_SIZE
    for address in range(0, IMAGE_SIZE, chunk_size):
        token.check()
        payload = image[address : address + chunk_size]
        # NOR is already erased; avoid programming padding while retaining
        # deterministic progress and a full readback in the next phase.
        if any(byte != 0xFF for byte in payload):
            device.write_flash(address, payload)
        _progress(
            progress,
            stage_base + address + len(payload),
            total_work,
            f"Programming W25Q64 at 0x{address:06X}",
        )

    if verify:
        stage_base = IMAGE_SIZE * 2
        for address in range(0, IMAGE_SIZE, chunk_size):
            token.check()
            expected = image[address : address + chunk_size]
            actual = device.read_flash(address, len(expected))
            difference = _first_difference(expected, actual)
            if difference is not None:
                absolute = address + difference
                raise VerificationError(
                    f"readback mismatch at flash 0x{absolute:06X}: "
                    f"expected {expected[difference]:02X}, got {actual[difference]:02X}"
                )
            _progress(
                progress,
                stage_base + address + len(expected),
                total_work,
                f"Verifying W25Q64 at 0x{address:06X}",
            )

    return OperationSummary(IMAGE_SIZE, zlib.crc32(image) & 0xFFFFFFFF)


def verify_image(
    device: MaintenanceDevice,
    image_path: str | Path,
    *,
    token: CancellationToken | None = None,
    progress: Progress | None = None,
) -> OperationSummary:
    """Read every byte and compare it with a locally validated image."""

    token = token or CancellationToken()
    inspect_image(image_path)
    expected_image = Path(image_path).read_bytes()
    info = _require_image_geometry(device)
    chunk_size = _chunk_size(info)
    crc = 0
    for address in range(0, IMAGE_SIZE, chunk_size):
        token.check()
        expected = expected_image[address : address + chunk_size]
        actual = device.read_flash(address, len(expected))
        difference = _first_difference(expected, actual)
        if difference is not None:
            absolute = address + difference
            raise VerificationError(
                f"readback mismatch at flash 0x{absolute:06X}: "
                f"expected {expected[difference]:02X}, got {actual[difference]:02X}"
            )
        crc = zlib.crc32(actual, crc)
        _progress(
            progress,
            address + len(actual),
            IMAGE_SIZE,
            f"Verifying W25Q64 at 0x{address:06X}",
        )
    return OperationSummary(IMAGE_SIZE, crc & 0xFFFFFFFF)


def readback_image(
    device: MaintenanceDevice,
    output_path: str | Path,
    *,
    token: CancellationToken | None = None,
    progress: Progress | None = None,
) -> OperationSummary:
    """Read all W25Q64 bytes into an atomically replaced local file."""

    token = token or CancellationToken()
    info = _require_image_geometry(device)
    chunk_size = _chunk_size(info)
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    crc = 0
    completed = 0
    try:
        with temporary.open("wb") as output:
            for address in range(0, IMAGE_SIZE, chunk_size):
                token.check()
                payload = device.read_flash(
                    address, min(chunk_size, IMAGE_SIZE - address)
                )
                output.write(payload)
                crc = zlib.crc32(payload, crc)
                completed += len(payload)
                _progress(
                    progress,
                    completed,
                    IMAGE_SIZE,
                    f"Reading W25Q64 at 0x{address:06X}",
                )
        temporary.replace(output_path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return OperationSummary(completed, crc & 0xFFFFFFFF)


def _pattern(address: int, length: int, inverse: bool = False) -> bytes:
    # Address-dependent xorshift data catches stuck bits and many address-line
    # aliases while remaining reproducible in tests and diagnostics.
    state = (0x9E3779B9 ^ address) & 0xFFFFFFFF
    result = bytearray(length)
    for index in range(length):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        value = state & 0xFF
        result[index] = value ^ (0xFF if inverse else 0)
    return bytes(result)


def _sample_addresses(size: int, block_size: int, full: bool) -> list[int]:
    if full:
        return list(range(0, size, block_size))
    final = size - block_size
    samples = {0, final, final // 4, final // 2, (final * 3) // 4}
    return sorted((address // block_size) * block_size for address in samples)


def _restore_flash(
    device: MaintenanceDevice, address: int, original: bytes
) -> None:
    device.erase_flash(address, len(original))
    if any(byte != 0xFF for byte in original):
        device.write_flash(address, original)
    restored = device.read_flash(address, len(original))
    if restored != original:
        difference = _first_difference(original, restored) or 0
        raise IntegrityTestError(
            f"could not restore W25Q64 at 0x{address + difference:06X}"
        )


def _test_flash_block(device: MaintenanceDevice, address: int, size: int) -> None:
    original = device.read_flash(address, size)
    failure: BaseException | None = None
    try:
        device.erase_flash(address, size)
        erased = device.read_flash(address, size)
        if erased != b"\xFF" * size:
            difference = _first_difference(b"\xFF" * size, erased) or 0
            raise IntegrityTestError(
                f"W25Q64 erase test failed at 0x{address + difference:06X}"
            )
        pattern = _pattern(address, size)
        device.write_flash(address, pattern)
        actual = device.read_flash(address, size)
        if actual != pattern:
            difference = _first_difference(pattern, actual) or 0
            raise IntegrityTestError(
                f"W25Q64 pattern mismatch at 0x{address + difference:06X}"
            )
    # Ctrl+C 也只能在当前块恢复完成后向上传播，否则完整性测试会破坏原数据。
    except (Exception, KeyboardInterrupt) as error:
        failure = error
    try:
        _restore_flash(device, address, original)
    except Exception as restore_error:
        # 即使恢复失败也保留 Ctrl+C 语义，并通过异常链暴露恢复错误。
        if isinstance(failure, KeyboardInterrupt):
            raise failure from restore_error
        raise IntegrityTestError(
            f"W25Q64 test failed and original data restoration also failed at "
            f"0x{address:06X}: {restore_error}"
        ) from (failure or restore_error)
    if failure:
        raise failure


def _test_psram_block(device: MaintenanceDevice, address: int, size: int) -> None:
    original = device.read_psram(address, size)
    failure: BaseException | None = None
    try:
        for inverse in (False, True):
            pattern = _pattern(address, size, inverse)
            device.write_psram(address, pattern)
            actual = device.read_psram(address, size)
            if actual != pattern:
                difference = _first_difference(pattern, actual) or 0
                raise IntegrityTestError(
                    f"APS6404L pattern mismatch at 0x{address + difference:06X}"
                )
    # KeyboardInterrupt 与普通测试故障一样，先进入统一恢复路径。
    except (Exception, KeyboardInterrupt) as error:
        failure = error
    try:
        device.write_psram(address, original)
        restored = device.read_psram(address, size)
        if restored != original:
            difference = _first_difference(original, restored) or 0
            raise IntegrityTestError(
                f"could not restore APS6404L at 0x{address + difference:06X}"
            )
    except Exception as restore_error:
        # 恢复失败时仍重新抛出原始中断，恢复错误保留为异常原因。
        if isinstance(failure, KeyboardInterrupt):
            raise failure from restore_error
        raise IntegrityTestError(
            f"APS6404L test failed and original data restoration also failed at "
            f"0x{address:06X}: {restore_error}"
        ) from (failure or restore_error)
    if failure:
        raise failure


def test_chip_integrity(
    device: MaintenanceDevice,
    chip: Chip,
    *,
    full: bool = False,
    token: CancellationToken | None = None,
    progress: Progress | None = None,
) -> OperationSummary:
    """Destructively exercise blocks, restoring each block before proceeding.

    Quick mode samples five evenly distributed 4 KiB blocks. Full W25Q64 mode
    performs erase/program cycles on every sector and therefore consumes erase
    endurance; callers should require an explicit user choice before using it.
    """

    token = token or CancellationToken()
    if chip == "flash":
        info = _require_capabilities(device, flash=True, erase=True)
        size = min(info.flash_size, IMAGE_SIZE)
        block_size = info.erase_size
        test_block = _test_flash_block
    elif chip == "psram":
        info = _require_capabilities(device, psram=True)
        size = info.psram_size
        block_size = _chunk_size(info)
        test_block = _test_psram_block
    else:
        raise ValueError(f"unknown chip: {chip}")

    addresses = _sample_addresses(size, block_size, full)
    total = len(addresses)
    for index, address in enumerate(addresses, 1):
        token.check()
        test_block(device, address, min(block_size, size - address))
        _progress(
            progress,
            index,
            total,
            f"Testing {chip} block at 0x{address:06X}",
        )
    return OperationSummary(total * block_size, blocks_tested=total)
