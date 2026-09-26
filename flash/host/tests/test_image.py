from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from ch32_image_tool.image import (
    IMAGE_SIZE,
    IMAGE_SLOTS,
    MANIFEST_SLOT_SIZE,
    ImageFormatError,
    build_image,
    inspect_image,
)


def _resources(root: Path) -> dict[str, bytes]:
    root.mkdir()
    payloads = {
        "vmlinux": bytes(range(251)) * 7,
        "mipsel-emu-embedded.dtb": b"\xd0\x0d\xfe\xed" + b"dtb" * 19,
        "initramfs.cpio.gz": b"\x1f\x8b" + b"rootfs" * 31,
    }
    for name, payload in payloads.items():
        (root / name).write_bytes(payload)
    return payloads


def test_build_is_exact_deterministic_and_inspectable(tmp_path: Path) -> None:
    payloads = _resources(tmp_path / "res")
    first = tmp_path / "first.img"
    second = tmp_path / "second.img"

    built = build_image(tmp_path / "res", first)
    build_image(tmp_path / "res", second)
    parsed = inspect_image(first)

    assert first.stat().st_size == IMAGE_SIZE
    assert first.read_bytes() == second.read_bytes()
    assert parsed == built
    data = first.read_bytes()
    assert data[256:MANIFEST_SLOT_SIZE] == b"\xff" * (MANIFEST_SLOT_SIZE - 256)
    for slot, entry in zip(IMAGE_SLOTS, parsed.entries, strict=True):
        payload = payloads[slot.filename]
        assert entry.offset == slot.offset
        assert entry.slot_size == slot.size
        assert entry.data_size == len(payload)
        assert entry.sha256 == hashlib.sha256(payload).digest()
        assert data[slot.offset : slot.offset + len(payload)] == payload
        assert data[slot.offset + len(payload) : slot.end] == b"\xff" * (
            slot.size - len(payload)
        )


def test_corruption_is_detected(tmp_path: Path) -> None:
    _resources(tmp_path / "res")
    image = tmp_path / "image.img"
    build_image(tmp_path / "res", image)
    data = bytearray(image.read_bytes())
    data[IMAGE_SLOTS[0].offset + 3] ^= 0x80

    with pytest.raises(ImageFormatError, match="CRC32 mismatch"):
        inspect_image(data)


def test_wrong_size_is_rejected() -> None:
    with pytest.raises(ImageFormatError, match="exactly"):
        inspect_image(b"too short")


def test_missing_and_oversized_resources_are_rejected(tmp_path: Path) -> None:
    resources = tmp_path / "missing"
    resources.mkdir()
    with pytest.raises(ImageFormatError, match="missing resource"):
        build_image(resources, tmp_path / "missing.img")

    resources = tmp_path / "large"
    _resources(resources)
    dtb_slot = next(slot for slot in IMAGE_SLOTS if slot.name == "dtb")
    (resources / dtb_slot.filename).write_bytes(b"x" * (dtb_slot.size + 1))
    with pytest.raises(ImageFormatError, match="slot capacity"):
        build_image(resources, tmp_path / "large.img")
