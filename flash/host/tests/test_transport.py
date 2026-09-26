from __future__ import annotations

from ch32_image_tool.transport import UsbBulkTransport


class FakeUsbDevice:
    def __init__(self) -> None:
        self.writes: list[tuple[int, bytes, int | None]] = []
        self.read_chunks = [b"ab", b"cdef"]

    def write(self, endpoint: int, data: bytes, timeout: int | None = None) -> int:
        self.writes.append((endpoint, bytes(data), timeout))
        return len(data)

    def read(
        self, endpoint: int, size: int, timeout: int | None = None
    ) -> bytes:
        assert endpoint == 0x81
        assert timeout == 321
        chunk = self.read_chunks.pop(0)
        return chunk[:size]


def test_bulk_transport_uses_pyusb_timeout_signature() -> None:
    device = FakeUsbDevice()
    transport = UsbBulkTransport(device, timeout_ms=321)

    transport.write(b"request")
    assert device.writes == [(0x01, b"request", 321)]
    assert transport.read_exact(6) == b"abcdef"
