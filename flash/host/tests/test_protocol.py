from __future__ import annotations

import struct
import zlib

import pytest

from ch32_image_tool.device import MaintenanceDevice
from ch32_image_tool.protocol import (
    DeviceCapabilities,
    INFO_STRUCT,
    Opcode,
    ProtocolError,
    Request,
    RESPONSE_MAGIC,
    RESPONSE_STRUCT,
    Response,
    Status,
)


class FakeTransport:
    def __init__(self, reads: list[bytes]) -> None:
        self.reads = reads
        self.writes: list[bytes] = []

    def write(self, data: bytes, timeout_ms: int | None = None) -> None:
        self.writes.append(bytes(data))

    def read_exact(self, length: int, timeout_ms: int | None = None) -> bytes:
        value = self.reads.pop(0)
        assert len(value) == length
        return value


def _reply(
    sequence: int,
    *,
    status: Status = Status.OK,
    value: int = 0,
    payload: bytes = b"",
) -> list[bytes]:
    header = RESPONSE_STRUCT.pack(
        RESPONSE_MAGIC, 1, int(status), sequence, value, len(payload)
    )
    return [header] + ([payload] if payload else [])


def test_request_wire_layout() -> None:
    packed = Request(Opcode.FLASH_READ, 0x1234, 0x89ABCDEF, 4096).pack()
    assert packed == struct.pack(
        "<4sBBHII", b"MVCH", 1, 0x10, 0x1234, 0x89ABCDEF, 4096
    )


def test_response_rejects_bad_magic_and_version() -> None:
    with pytest.raises(ProtocolError, match="magic"):
        Response.unpack(struct.pack("<4sBBHII", b"nope", 1, 0, 1, 0, 0))
    with pytest.raises(ProtocolError, match="version"):
        Response.unpack(struct.pack("<4sBBHII", b"MVRS", 2, 0, 1, 0, 0))


def test_info_payload() -> None:
    payload = INFO_STRUCT.pack(
        8 << 20,
        8 << 20,
        4096,
        4096,
        256,
        0xF,
        1,
        7,
        b"\xef\x40\x17\0",
        b"\x0d\x5d\x52\xd6",
    )
    transport = FakeTransport(_reply(1, payload=payload))
    device = MaintenanceDevice(transport)
    info = device.get_info()

    assert isinstance(info, DeviceCapabilities)
    assert info.flash_size == 8 << 20
    assert info.max_transfer == 4096
    assert info.firmware_version == "1.7"
    assert transport.writes == [Request(Opcode.INFO, 1).pack()]


def test_read_and_write_crc_are_checked() -> None:
    payload = b"usb-payload"
    reads = _reply(1, value=zlib.crc32(payload) & 0xFFFFFFFF, payload=payload)
    transport = FakeTransport(reads)
    device = MaintenanceDevice(transport)
    device.capabilities = DeviceCapabilities(
        1 << 20, 1 << 20, 4096, 4096, 256, 0xF, 1, 0, b"id\0\0", b"id\0\0"
    )
    assert device.read_flash(0x100, len(payload)) == payload

    transport = FakeTransport(_reply(1, value=zlib.crc32(payload) & 0xFFFFFFFF))
    device = MaintenanceDevice(transport)
    device.capabilities = DeviceCapabilities(
        1 << 20, 1 << 20, 4096, 4096, 256, 0xF, 1, 0, b"id\0\0", b"id\0\0"
    )
    device.write_psram(0x200, payload)
    assert transport.writes[1] == payload


def test_error_status_includes_firmware_detail() -> None:
    transport = FakeTransport(
        _reply(1, status=Status.OUT_OF_RANGE, value=0x800000, payload=b"bad range")
    )
    device = MaintenanceDevice(transport)
    with pytest.raises(ProtocolError, match="bad range"):
        device.command(Opcode.FLASH_READ, address=0x800000, length=1)


def test_crc_mismatch_is_rejected() -> None:
    payload = b"corrupt"
    transport = FakeTransport(_reply(1, value=0, payload=payload))
    device = MaintenanceDevice(transport)
    device.capabilities = DeviceCapabilities(
        1 << 20, 1 << 20, 4096, 4096, 256, 0xF, 1, 0, b"id\0\0", b"id\0\0"
    )
    with pytest.raises(ProtocolError, match="CRC mismatch"):
        device.read_flash(0, len(payload))
