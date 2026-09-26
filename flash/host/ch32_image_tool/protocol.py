"""Wire protocol shared by the USB firmware and desktop application.

Every command is one 16-byte bulk OUT transfer followed, when applicable, by
one payload transfer.  Every reply starts with a separate 16-byte bulk IN
transfer followed by a payload transfer.  Keeping the header in a short USB
transfer makes framing unambiguous on a 64-byte full-speed endpoint.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct


PROTOCOL_VERSION = 1
REQUEST_MAGIC = b"MVCH"
RESPONSE_MAGIC = b"MVRS"
REQUEST_STRUCT = struct.Struct("<4sBBHII")
RESPONSE_STRUCT = struct.Struct("<4sBBHII")
HEADER_SIZE = REQUEST_STRUCT.size

# pid.codes' shared testing VID.  Both values remain command-line overridable
# so a locally assigned production VID/PID needs no source change.
DEFAULT_VID = 0x1209
DEFAULT_PID = 0x2036
INTERFACE_NUMBER = 0
ENDPOINT_OUT = 0x01
ENDPOINT_IN = 0x81
USB_PACKET_SIZE = 64


class Opcode(IntEnum):
    INFO = 0x01
    DEVICE_STATUS = 0x02
    FLASH_READ = 0x10
    FLASH_WRITE = 0x11
    FLASH_ERASE = 0x12
    FLASH_CRC32 = 0x13
    PSRAM_READ = 0x20
    PSRAM_WRITE = 0x21
    PSRAM_CRC32 = 0x23
    CANCEL = 0x7F


class Status(IntEnum):
    OK = 0
    INVALID_OPCODE = 1
    INVALID_ARGUMENT = 2
    OUT_OF_RANGE = 3
    BUSY = 4
    IO_ERROR = 5
    VERIFY_FAILED = 6
    CANCELLED = 7
    BAD_FRAME = 8
    INTERNAL_ERROR = 9


STATUS_TEXT = {
    Status.OK: "success",
    Status.INVALID_OPCODE: "unsupported command",
    Status.INVALID_ARGUMENT: "invalid argument",
    Status.OUT_OF_RANGE: "address outside device",
    Status.BUSY: "device busy",
    Status.IO_ERROR: "SPI/USB I/O failure",
    Status.VERIFY_FAILED: "device-side verification failed",
    Status.CANCELLED: "cancelled",
    Status.BAD_FRAME: "malformed USB frame",
    Status.INTERNAL_ERROR: "firmware internal error",
}


class ProtocolError(RuntimeError):
    """Malformed exchange or an error status returned by the firmware."""


@dataclass(frozen=True, slots=True)
class Request:
    opcode: Opcode
    sequence: int
    address: int = 0
    length: int = 0

    def pack(self) -> bytes:
        if not 0 <= self.sequence <= 0xFFFF:
            raise ProtocolError("sequence is outside uint16")
        if not 0 <= self.address <= 0xFFFFFFFF:
            raise ProtocolError("address is outside uint32")
        if not 0 <= self.length <= 0xFFFFFFFF:
            raise ProtocolError("length is outside uint32")
        return REQUEST_STRUCT.pack(
            REQUEST_MAGIC,
            PROTOCOL_VERSION,
            int(self.opcode),
            self.sequence,
            self.address,
            self.length,
        )


@dataclass(frozen=True, slots=True)
class Response:
    status: Status
    sequence: int
    value: int
    payload_length: int

    @classmethod
    def unpack(cls, raw: bytes) -> "Response":
        if len(raw) != RESPONSE_STRUCT.size:
            raise ProtocolError(
                f"reply header is {len(raw)} bytes, expected {RESPONSE_STRUCT.size}"
            )
        magic, version, status_value, sequence, value, length = (
            RESPONSE_STRUCT.unpack(raw)
        )
        if magic != RESPONSE_MAGIC:
            raise ProtocolError(f"invalid reply magic: {magic!r}")
        if version != PROTOCOL_VERSION:
            raise ProtocolError(f"unsupported device protocol version: {version}")
        try:
            status = Status(status_value)
        except ValueError as error:
            raise ProtocolError(f"unknown firmware status: {status_value}") from error
        return cls(status, sequence, value, length)


# INFO payload, protocol version 1.
# Sizes are bytes. IDs are zero-padded bytes in wire order.
INFO_STRUCT = struct.Struct("<IIIIHHHH4s4s")
CAP_FLASH_PRESENT = 1 << 0
CAP_PSRAM_PRESENT = 1 << 1
CAP_FLASH_ERASE = 1 << 2
CAP_CRC32 = 1 << 3


@dataclass(frozen=True, slots=True)
class DeviceCapabilities:
    flash_size: int
    psram_size: int
    erase_size: int
    max_transfer: int
    page_size: int
    flags: int
    firmware_major: int
    firmware_minor: int
    flash_id: bytes
    psram_id: bytes

    @classmethod
    def unpack(cls, payload: bytes) -> "DeviceCapabilities":
        if len(payload) != INFO_STRUCT.size:
            raise ProtocolError(
                f"INFO payload is {len(payload)} bytes, expected {INFO_STRUCT.size}"
            )
        values = INFO_STRUCT.unpack(payload)
        capabilities = cls(*values)
        if capabilities.max_transfer == 0:
            raise ProtocolError("device reported a zero transfer limit")
        return capabilities

    @property
    def firmware_version(self) -> str:
        return f"{self.firmware_major}.{self.firmware_minor}"

    @property
    def flash_id_text(self) -> str:
        return " ".join(f"{byte:02X}" for byte in self.flash_id if byte)

    @property
    def psram_id_text(self) -> str:
        return " ".join(f"{byte:02X}" for byte in self.psram_id if byte)
