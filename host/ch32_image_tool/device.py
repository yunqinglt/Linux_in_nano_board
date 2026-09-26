"""Synchronous, testable client for the MCU maintenance protocol."""

from __future__ import annotations

from dataclasses import dataclass
from threading import Lock
from typing import Protocol
import zlib

from .protocol import (
    DeviceCapabilities,
    HEADER_SIZE,
    Opcode,
    ProtocolError,
    Request,
    Response,
    STATUS_TEXT,
    Status,
)


class BulkIo(Protocol):
    def write(self, data: bytes, timeout_ms: int | None = None) -> None: ...

    def read_exact(self, length: int, timeout_ms: int | None = None) -> bytes: ...


@dataclass(frozen=True, slots=True)
class CommandResult:
    value: int
    payload: bytes


class MaintenanceDevice:
    """Serialize request/reply exchanges over a provided bulk transport."""

    def __init__(self, transport: BulkIo, *, timeout_ms: int = 5_000) -> None:
        self.transport = transport
        self.timeout_ms = timeout_ms
        self._sequence = 0
        self._lock = Lock()
        self.capabilities: DeviceCapabilities | None = None

    def _next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFFFF
        if self._sequence == 0:
            self._sequence = 1
        return self._sequence

    def command(
        self,
        opcode: Opcode,
        *,
        address: int = 0,
        length: int = 0,
        payload: bytes = b"",
        timeout_ms: int | None = None,
    ) -> CommandResult:
        if payload and len(payload) != length:
            raise ProtocolError(
                f"payload is {len(payload)} bytes but request length is {length}"
            )
        timeout = timeout_ms if timeout_ms is not None else self.timeout_ms
        with self._lock:
            sequence = self._next_sequence()
            request = Request(opcode, sequence, address, length)
            self.transport.write(request.pack(), timeout)
            if payload:
                self.transport.write(payload, timeout)
            response = Response.unpack(self.transport.read_exact(HEADER_SIZE, timeout))
            if response.sequence != sequence:
                raise ProtocolError(
                    f"reply sequence {response.sequence} does not match {sequence}"
                )
            response_payload = (
                self.transport.read_exact(response.payload_length, timeout)
                if response.payload_length
                else b""
            )
            if response.status is not Status.OK:
                detail = response_payload.rstrip(b"\0").decode("utf-8", "replace")
                message = STATUS_TEXT.get(response.status, response.status.name)
                if detail:
                    message = f"{message}: {detail}"
                raise ProtocolError(
                    f"{opcode.name} failed ({message}, value=0x{response.value:08X})"
                )
            return CommandResult(response.value, response_payload)

    def get_info(self) -> DeviceCapabilities:
        result = self.command(Opcode.INFO)
        self.capabilities = DeviceCapabilities.unpack(result.payload)
        return self.capabilities

    def _limit(self) -> int:
        if self.capabilities is None:
            self.get_info()
        assert self.capabilities is not None
        return self.capabilities.max_transfer

    def _check_transfer(self, address: int, length: int) -> None:
        if length < 0 or length > self._limit():
            raise ValueError(
                f"transfer length {length} exceeds device limit {self._limit()}"
            )
        if address < 0 or address + length > 0x1_0000_0000:
            raise ValueError("transfer address is outside uint32")

    def read_flash(self, address: int, length: int) -> bytes:
        self._check_transfer(address, length)
        result = self.command(Opcode.FLASH_READ, address=address, length=length)
        if len(result.payload) != length:
            raise ProtocolError(
                f"FLASH_READ returned {len(result.payload)} of {length} bytes"
            )
        actual_crc = zlib.crc32(result.payload) & 0xFFFFFFFF
        if result.value != actual_crc:
            raise ProtocolError(
                f"FLASH_READ CRC mismatch: device {result.value:08X}, "
                f"host {actual_crc:08X}"
            )
        return result.payload

    def write_flash(self, address: int, payload: bytes) -> None:
        self._check_transfer(address, len(payload))
        result = self.command(
            Opcode.FLASH_WRITE,
            address=address,
            length=len(payload),
            payload=payload,
        )
        expected_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if result.value != expected_crc:
            raise ProtocolError(
                f"FLASH_WRITE CRC mismatch: device {result.value:08X}, "
                f"host {expected_crc:08X}"
            )

    def erase_flash(self, address: int, length: int) -> None:
        result = self.command(
            Opcode.FLASH_ERASE,
            address=address,
            length=length,
            timeout_ms=max(self.timeout_ms, 20_000),
        )
        if result.value not in (0, length):
            raise ProtocolError(
                f"FLASH_ERASE acknowledged {result.value} of {length} bytes"
            )

    def flash_crc32(self, address: int, length: int) -> int:
        return self.command(
            Opcode.FLASH_CRC32,
            address=address,
            length=length,
            timeout_ms=max(self.timeout_ms, 30_000),
        ).value

    def read_psram(self, address: int, length: int) -> bytes:
        self._check_transfer(address, length)
        result = self.command(Opcode.PSRAM_READ, address=address, length=length)
        if len(result.payload) != length:
            raise ProtocolError(
                f"PSRAM_READ returned {len(result.payload)} of {length} bytes"
            )
        actual_crc = zlib.crc32(result.payload) & 0xFFFFFFFF
        if result.value != actual_crc:
            raise ProtocolError(
                f"PSRAM_READ CRC mismatch: device {result.value:08X}, "
                f"host {actual_crc:08X}"
            )
        return result.payload

    def write_psram(self, address: int, payload: bytes) -> None:
        self._check_transfer(address, len(payload))
        result = self.command(
            Opcode.PSRAM_WRITE,
            address=address,
            length=len(payload),
            payload=payload,
        )
        expected_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if result.value != expected_crc:
            raise ProtocolError(
                f"PSRAM_WRITE CRC mismatch: device {result.value:08X}, "
                f"host {expected_crc:08X}"
            )

    def psram_crc32(self, address: int, length: int) -> int:
        return self.command(
            Opcode.PSRAM_CRC32,
            address=address,
            length=length,
            timeout_ms=max(self.timeout_ms, 30_000),
        ).value

    def cancel(self) -> None:
        self.command(Opcode.CANCEL, timeout_ms=1_000)
