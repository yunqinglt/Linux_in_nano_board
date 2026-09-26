"""Build and inspect the deterministic 8 MiB W25Q64 boot image."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
from typing import Callable, Iterable
import zlib


IMAGE_SIZE = 8 * 1024 * 1024
ERASE_SIZE = 4 * 1024
TRANSFER_BUFFER_SIZE = 4 * 1024
MANIFEST_SLOT_SIZE = 64 * 1024
MANIFEST_MAGIC = b"MIPSIMG\0"
MANIFEST_VERSION = 1
ERASED_BYTE = 0xFF

_HEADER = struct.Struct("<8sHHIIIIII28s")
_ENTRY = struct.Struct("<16sIIII32s")
_MANIFEST_CRC_OFFSET = 28
_IMAGE_CRC_OFFSET = 32


@dataclass(frozen=True, slots=True)
class ImageSlot:
    name: str
    filename: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


# Fixed W25Q64 layout.  The 64 KiB manifest area is intentionally aligned to
# a block boundary.  Kernel growth is favoured while retaining almost 2 MiB
# for a compressed initramfs.
IMAGE_SLOTS: tuple[ImageSlot, ...] = (
    ImageSlot("vmlinux", "vmlinux", 0x010000, 0x600000),
    ImageSlot("dtb", "mipsel-emu-embedded.dtb", 0x610000, 0x010000),
    ImageSlot("initramfs", "initramfs.cpio.gz", 0x620000, 0x1E0000),
)


@dataclass(frozen=True, slots=True)
class ImageEntry:
    name: str
    offset: int
    slot_size: int
    data_size: int
    crc32: int
    sha256: bytes


@dataclass(frozen=True, slots=True)
class ImageManifest:
    image_size: int
    erase_size: int
    transfer_size: int
    manifest_crc32: int
    image_crc32: int
    entries: tuple[ImageEntry, ...]


class ImageFormatError(ValueError):
    """The source set or packed image violates the fixed layout."""


Progress = Callable[[int, int, str], None]


def _crc32(data: bytes | bytearray | memoryview) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _validate_layout(slots: Iterable[ImageSlot] = IMAGE_SLOTS) -> None:
    ranges = [(0, MANIFEST_SLOT_SIZE, "manifest")]
    for slot in slots:
        if slot.offset % ERASE_SIZE or slot.size % ERASE_SIZE:
            raise ImageFormatError(f"{slot.name}: slot is not erase aligned")
        if slot.offset < MANIFEST_SLOT_SIZE or slot.end > IMAGE_SIZE:
            raise ImageFormatError(f"{slot.name}: slot is outside the image")
        ranges.append((slot.offset, slot.end, slot.name))
    ranges.sort()
    for left, right in zip(ranges, ranges[1:]):
        if left[1] > right[0]:
            raise ImageFormatError(f"slots overlap: {left[2]} and {right[2]}")


def _entry_bytes(entry: ImageEntry) -> bytes:
    encoded_name = entry.name.encode("ascii")
    if len(encoded_name) > 15:
        raise ImageFormatError(f"manifest name too long: {entry.name}")
    return _ENTRY.pack(
        encoded_name.ljust(16, b"\0"),
        entry.offset,
        entry.slot_size,
        entry.data_size,
        entry.crc32,
        entry.sha256,
    )


def _pack_manifest(
    entries: tuple[ImageEntry, ...], manifest_crc32: int, image_crc32: int
) -> bytes:
    header_size = _HEADER.size + len(entries) * _ENTRY.size
    header = _HEADER.pack(
        MANIFEST_MAGIC,
        MANIFEST_VERSION,
        header_size,
        IMAGE_SIZE,
        ERASE_SIZE,
        TRANSFER_BUFFER_SIZE,
        len(entries),
        manifest_crc32,
        image_crc32,
        b"\0" * 28,
    )
    return header + b"".join(_entry_bytes(entry) for entry in entries)


def build_image(
    resource_dir: str | Path,
    output_path: str | Path,
    progress: Progress | None = None,
) -> ImageManifest:
    """Pack the three fixed resources into an exactly 8 MiB image.

    Unused bytes are the NOR erased value (0xff).  The image CRC is computed
    with both CRC fields cleared; the manifest CRC is then computed with only
    its own field cleared.  This removes checksum circularity.
    """

    _validate_layout()
    resource_dir = Path(resource_dir)
    output_path = Path(output_path)
    if not resource_dir.is_dir():
        raise ImageFormatError(f"resource directory does not exist: {resource_dir}")

    image = bytearray([ERASED_BYTE]) * IMAGE_SIZE
    entries: list[ImageEntry] = []
    total = len(IMAGE_SLOTS)
    for index, slot in enumerate(IMAGE_SLOTS, 1):
        source = resource_dir / slot.filename
        if not source.is_file():
            raise ImageFormatError(f"missing resource: {source}")
        payload = source.read_bytes()
        if len(payload) > slot.size:
            raise ImageFormatError(
                f"{slot.filename} is {len(payload):,} bytes; "
                f"slot capacity is {slot.size:,} bytes"
            )
        image[slot.offset : slot.offset + len(payload)] = payload
        entries.append(
            ImageEntry(
                name=slot.name,
                offset=slot.offset,
                slot_size=slot.size,
                data_size=len(payload),
                crc32=_crc32(payload),
                sha256=hashlib.sha256(payload).digest(),
            )
        )
        if progress:
            progress(index, total, f"Packed {slot.filename}")

    packed_entries = tuple(entries)
    manifest = _pack_manifest(packed_entries, 0, 0)
    image[: len(manifest)] = manifest
    image_crc32 = _crc32(image)

    manifest = _pack_manifest(packed_entries, 0, image_crc32)
    image[: len(manifest)] = manifest
    manifest_crc32 = _crc32(image[: len(manifest)])
    manifest = _pack_manifest(packed_entries, manifest_crc32, image_crc32)
    image[: len(manifest)] = manifest

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    try:
        temporary.write_bytes(image)
        temporary.replace(output_path)
    finally:
        if temporary.exists():
            temporary.unlink()

    return ImageManifest(
        image_size=IMAGE_SIZE,
        erase_size=ERASE_SIZE,
        transfer_size=TRANSFER_BUFFER_SIZE,
        manifest_crc32=manifest_crc32,
        image_crc32=image_crc32,
        entries=packed_entries,
    )


def _read_image(source: str | Path | bytes | bytearray | memoryview) -> bytes:
    if isinstance(source, (str, Path)):
        data = Path(source).read_bytes()
    else:
        data = bytes(source)
    if len(data) != IMAGE_SIZE:
        raise ImageFormatError(
            f"image must be exactly {IMAGE_SIZE:,} bytes, got {len(data):,}"
        )
    return data


def inspect_image(
    source: str | Path | bytes | bytearray | memoryview,
    *,
    verify_payloads: bool = True,
) -> ImageManifest:
    """Parse and fully validate a packed image."""

    data = _read_image(source)
    try:
        (
            magic,
            version,
            header_size,
            image_size,
            erase_size,
            transfer_size,
            entry_count,
            manifest_crc32,
            image_crc32,
            reserved,
        ) = _HEADER.unpack_from(data)
    except struct.error as error:
        raise ImageFormatError("truncated manifest") from error

    if magic != MANIFEST_MAGIC:
        raise ImageFormatError("invalid image magic")
    if version != MANIFEST_VERSION:
        raise ImageFormatError(f"unsupported image version: {version}")
    expected_header_size = _HEADER.size + entry_count * _ENTRY.size
    if header_size != expected_header_size or header_size > MANIFEST_SLOT_SIZE:
        raise ImageFormatError("invalid manifest size")
    if (image_size, erase_size, transfer_size) != (
        IMAGE_SIZE,
        ERASE_SIZE,
        TRANSFER_BUFFER_SIZE,
    ):
        raise ImageFormatError("image geometry does not match this tool")
    if entry_count != len(IMAGE_SLOTS):
        raise ImageFormatError(
            f"image contains {entry_count} entries; expected {len(IMAGE_SLOTS)}"
        )
    if reserved != b"\0" * len(reserved):
        raise ImageFormatError("reserved manifest bytes are not zero")

    entries: list[ImageEntry] = []
    cursor = _HEADER.size
    for expected_slot in IMAGE_SLOTS:
        raw_name, offset, slot_size, data_size, crc32, sha256 = _ENTRY.unpack_from(
            data, cursor
        )
        cursor += _ENTRY.size
        try:
            name = raw_name.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as error:
            raise ImageFormatError("entry name is not ASCII") from error
        if not name or data_size > slot_size or offset + slot_size > IMAGE_SIZE:
            raise ImageFormatError(f"invalid manifest entry: {name or '<empty>'}")
        if (name, offset, slot_size) != (
            expected_slot.name,
            expected_slot.offset,
            expected_slot.size,
        ):
            raise ImageFormatError(
                f"manifest entry does not match fixed slot {expected_slot.name}"
            )
        entry = ImageEntry(name, offset, slot_size, data_size, crc32, sha256)
        entries.append(entry)
        if verify_payloads:
            payload = data[offset : offset + data_size]
            if _crc32(payload) != crc32:
                raise ImageFormatError(f"{name}: CRC32 mismatch")
            if hashlib.sha256(payload).digest() != sha256:
                raise ImageFormatError(f"{name}: SHA-256 mismatch")
            padding = data[offset + data_size : offset + slot_size]
            if padding != b"\xFF" * len(padding):
                raise ImageFormatError(f"{name}: slot padding is not erased")

    manifest_padding = data[header_size:MANIFEST_SLOT_SIZE]
    if manifest_padding != b"\xFF" * len(manifest_padding):
        raise ImageFormatError("manifest slot padding is not erased")

    manifest_copy = bytearray(data[:header_size])
    struct.pack_into("<I", manifest_copy, _MANIFEST_CRC_OFFSET, 0)
    if _crc32(manifest_copy) != manifest_crc32:
        raise ImageFormatError("manifest CRC32 mismatch")

    image_copy = bytearray(data)
    struct.pack_into("<I", image_copy, _MANIFEST_CRC_OFFSET, 0)
    struct.pack_into("<I", image_copy, _IMAGE_CRC_OFFSET, 0)
    if _crc32(image_copy) != image_crc32:
        raise ImageFormatError("whole-image CRC32 mismatch")

    return ImageManifest(
        image_size=image_size,
        erase_size=erase_size,
        transfer_size=transfer_size,
        manifest_crc32=manifest_crc32,
        image_crc32=image_crc32,
        entries=tuple(entries),
    )
