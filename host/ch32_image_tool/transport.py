"""Explicit PyUSB/libusb-1.0 transport implementation."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
from threading import Lock
from typing import Any, Iterable

from .protocol import (
    DEFAULT_PID,
    DEFAULT_VID,
    ENDPOINT_IN,
    ENDPOINT_OUT,
    INTERFACE_NUMBER,
)


class UsbTransportError(RuntimeError):
    """A host USB stack, permission, disconnection, or timeout failure."""


@dataclass(frozen=True, slots=True)
class UsbDeviceInfo:
    vid: int
    pid: int
    bus: int | None
    address: int | None
    serial_number: str | None
    manufacturer: str | None
    product: str | None

    @property
    def key(self) -> tuple[int | None, int | None, str | None]:
        return self.bus, self.address, self.serial_number

    @property
    def label(self) -> str:
        description = self.product or "CH32V203 maintenance device"
        location = (
            f"bus {self.bus}, address {self.address}"
            if self.bus is not None and self.address is not None
            else "USB"
        )
        serial = f", S/N {self.serial_number}" if self.serial_number else ""
        return f"{description} ({self.vid:04X}:{self.pid:04X}, {location}{serial})"


def _usb_modules() -> tuple[Any, Any]:
    try:
        import usb.core
        import usb.util
    except ImportError as error:
        raise UsbTransportError(
            "PyUSB is not installed; run 'python -m pip install -r requirements.txt'"
        ) from error
    return usb.core, usb.util


def make_libusb_backend() -> Any:
    """Return an explicit libusb-1.0 backend, including the bundled DLL."""

    try:
        import usb1
        import usb.backend.libusb1

        configured_dll = os.environ.get("LIBUSB_LIBRARY_PATH")
        bundled_dll = Path(usb1.__file__).resolve().parent / "libusb-1.0.dll"
        dll = configured_dll or (str(bundled_dll) if bundled_dll.is_file() else None)
        backend = usb.backend.libusb1.get_backend(
            find_library=(lambda _name: dll) if dll else None
        )
    except (ImportError, OSError) as error:
        raise UsbTransportError(f"cannot load libusb-1.0: {error}") from error
    if backend is None:
        raise UsbTransportError(
            "libusb-1.0 backend was not found; install libusb1 or set "
            "LIBUSB_LIBRARY_PATH to libusb-1.0.dll"
        )
    return backend


def _safe_string(device: Any, index: int) -> str | None:
    if not index:
        return None
    try:
        _core, util = _usb_modules()
        return util.get_string(device, index)
    except Exception:
        # Enumeration remains useful even when Windows has not bound WinUSB.
        return None


def enumerate_devices(
    vid: int = DEFAULT_VID,
    pid: int = DEFAULT_PID,
    *,
    backend: Any | None = None,
) -> list[UsbDeviceInfo]:
    core, _util = _usb_modules()
    backend = backend or make_libusb_backend()
    try:
        devices: Iterable[Any] = core.find(
            find_all=True, idVendor=vid, idProduct=pid, backend=backend
        ) or ()
        result = [
            UsbDeviceInfo(
                vid=device.idVendor,
                pid=device.idProduct,
                bus=getattr(device, "bus", None),
                address=getattr(device, "address", None),
                serial_number=_safe_string(device, device.iSerialNumber),
                manufacturer=_safe_string(device, device.iManufacturer),
                product=_safe_string(device, device.iProduct),
            )
            for device in devices
        ]
    except Exception as error:
        raise UsbTransportError(f"USB enumeration failed: {error}") from error
    return result


class UsbBulkTransport:
    """One claimed vendor interface with paired bulk endpoints."""

    def __init__(
        self,
        device: Any,
        *,
        interface: int = INTERFACE_NUMBER,
        endpoint_out: int = ENDPOINT_OUT,
        endpoint_in: int = ENDPOINT_IN,
        timeout_ms: int = 5_000,
    ) -> None:
        self._device = device
        self.interface = interface
        self.endpoint_out = endpoint_out
        self.endpoint_in = endpoint_in
        self.timeout_ms = timeout_ms
        self._claimed = False
        self._lock = Lock()

    @classmethod
    def open(
        cls,
        info: UsbDeviceInfo | None = None,
        *,
        vid: int = DEFAULT_VID,
        pid: int = DEFAULT_PID,
        backend: Any | None = None,
        timeout_ms: int = 5_000,
    ) -> "UsbBulkTransport":
        core, util = _usb_modules()
        backend = backend or make_libusb_backend()
        try:
            candidates = core.find(
                find_all=True, idVendor=vid, idProduct=pid, backend=backend
            ) or ()
            device = None
            for candidate in candidates:
                if info is None:
                    device = candidate
                    break
                candidate_key = (
                    getattr(candidate, "bus", None),
                    getattr(candidate, "address", None),
                    _safe_string(candidate, candidate.iSerialNumber),
                )
                if candidate_key == info.key:
                    device = candidate
                    break
            if device is None:
                raise UsbTransportError(
                    f"device {vid:04X}:{pid:04X} is not connected"
                )

            try:
                device.set_configuration()
            except Exception as error:
                # A configured device often reports BUSY on repeated selection.
                try:
                    device.get_active_configuration()
                except Exception:
                    raise error

            transport = cls(device, timeout_ms=timeout_ms)
            try:
                if hasattr(device, "is_kernel_driver_active") and (
                    device.is_kernel_driver_active(INTERFACE_NUMBER)
                ):
                    device.detach_kernel_driver(INTERFACE_NUMBER)
            except (NotImplementedError, AttributeError):
                pass
            util.claim_interface(device, INTERFACE_NUMBER)
            transport._claimed = True
            return transport
        except UsbTransportError:
            raise
        except Exception as error:
            raise UsbTransportError(
                f"cannot open {vid:04X}:{pid:04X}: {error}. On Windows, bind "
                "the device interface to WinUSB/libusbK with Zadig."
            ) from error

    def write(self, data: bytes, timeout_ms: int | None = None) -> None:
        if not data:
            return
        timeout = timeout_ms if timeout_ms is not None else self.timeout_ms
        try:
            with self._lock:
                written = self._device.write(
                    self.endpoint_out, data, timeout=timeout
                )
        except Exception as error:
            raise UsbTransportError(f"USB write failed: {error}") from error
        if written != len(data):
            raise UsbTransportError(
                f"short USB write: {written} of {len(data)} bytes"
            )

    def read_exact(self, length: int, timeout_ms: int | None = None) -> bytes:
        if length < 0:
            raise ValueError("negative USB read length")
        timeout = timeout_ms if timeout_ms is not None else self.timeout_ms
        result = bytearray()
        try:
            with self._lock:
                while len(result) < length:
                    packet = self._device.read(
                        self.endpoint_in,
                        length - len(result),
                        timeout=timeout,
                    )
                    if not packet:
                        raise UsbTransportError("zero-length USB read")
                    result.extend(packet)
        except UsbTransportError:
            raise
        except Exception as error:
            raise UsbTransportError(f"USB read failed: {error}") from error
        return bytes(result)

    def close(self) -> None:
        if self._device is None:
            return
        _core, util = _usb_modules()
        try:
            if self._claimed:
                util.release_interface(self._device, self.interface)
        except Exception:
            pass
        finally:
            try:
                util.dispose_resources(self._device)
            finally:
                self._claimed = False
                self._device = None

    def __enter__(self) -> "UsbBulkTransport":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()
