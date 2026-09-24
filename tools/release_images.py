"""Validate complete ESP32-S3 components and the catalog's factory layouts."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from release_catalog import BOARDS, PACKAGES, ROOT, read_partitions

COMPONENT_PREFIX = b"RATSPEAK-HANDHELD-COMPONENT:v1:"
# Identical SDK boot_app0.bin in pinned Arduino-ESP32 2.0.16 and 2.0.17.
BOOT_APP0_SHA256 = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"


def complete_image(data: bytes, *, exact: bool = True):
    import esptool
    from esptool.bin_image import LoadFirmwareImage

    if esptool.__version__ != "5.2.0":
        raise ValueError("release image validation requires esptool 5.2.0")
    try:
        image = LoadFirmwareImage("esp32s3", data)
        if not image.segments or image.chip_id != image.ROM_LOADER.IMAGE_CHIP_ID:
            raise ValueError("not an ESP32-S3 application")
        if image.checksum != image.calculate_checksum():
            raise ValueError("application checksum mismatch")
        if not image.append_digest or image.stored_digest != image.calc_digest:
            raise ValueError("missing or invalid application SHA-256")
        size = image.data_length + 32
        if size > len(data) or (exact and size != len(data)):
            raise ValueError("truncated application or unexpected trailing data")
        return image, size
    except Exception as error:
        raise ValueError(f"invalid complete application image: {error}") from error


def verify_application(data: bytes) -> None:
    complete_image(data)


def component_id(board: str, role: str, version: str, revision: str, dirty: bool) -> bytes:
    return COMPONENT_PREFIX + f"{board}:{role}:{version}:{revision}:{int(dirty)}".encode() + b"\0"


def verify_component(data: bytes, board: str, role: str, version: str, revision: str,
                     dirty: bool, capacity: int) -> None:
    if not 24 <= len(data) <= capacity:
        raise ValueError(f"{board} {role}: empty application or slot overflow")
    image, _ = complete_image(data)
    payload = b"".join(segment.data for segment in image.segments)
    expected = component_id(board, role, version, revision, dirty)
    if payload.count(COMPONENT_PREFIX) != 1 or expected not in payload:
        raise ValueError(f"{board} {role}: component board/role/version/source identity mismatch")


def flash_settings(data: bytes, board: str) -> dict[str, str]:
    image, _ = complete_image(data[:0x8000], exact=False)
    expected_size = {"8MB": 3, "16MB": 4}[BOARDS[board].flash_size]
    if image.flash_mode != 2 or image.flash_size_freq >> 4 != expected_size:
        raise ValueError(f"{board}: bootloader flash mode/capacity mismatch")
    frequencies = {0: "40m", 1: "26m", 2: "20m", 15: "80m"}
    frequency = frequencies.get(image.flash_size_freq & 15)
    if frequency not in ("40m", "80m"):
        raise ValueError(f"{board}: unsupported bootloader flash frequency")
    return {"flashMode": "dio", "flashFreq": frequency}


def factory_partitions(board: str, mode: str, root: Path = ROOT):
    if mode not in BOARDS[board].modes:
        raise ValueError(f"unsupported factory mode: {mode}")
    capability = BOARDS[board]
    if mode == "rnode":
        return read_partitions(root / capability.rnode_partition_csv, capability.capacity)
    return capability.partitions(root, standalone=mode == "standalone")


def partition_binary(entries) -> bytes:
    """Encode only supported explicit catalog rows in Espressif's MD5 format.

    This expected-byte check is cross-checked against the pinned SDK generator;
    it does not interpret or trust geometry from an incoming package.
    """
    types = {"app": 0, "data": 1}
    subtypes = {"app": {f"ota_{i}": 0x10 + i for i in range(16)},
                "data": {"ota": 0, "nvs": 2, "coredump": 3, "spiffs": 0x82}}
    records = bytearray()
    for name, entry in entries.items():
        if len(name.encode()) > 15:
            raise ValueError("partition label exceeds 15 bytes")
        try:
            records += struct.pack("<2sBBLL16sL", b"\xaa\x50", types[entry.kind],
                                   subtypes[entry.kind][entry.subtype], entry.offset,
                                   entry.size, name.encode(), 0)
        except KeyError as error:
            raise ValueError("unsupported catalog partition type/subtype") from error
    records += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(records).digest()
    if len(records) > 0xC00:
        raise ValueError("catalog partition table exceeds its sector")
    return bytes(records).ljust(0xC00, b"\xff")


def verify_factory(data: bytes, board: str, mode: str, version: str, revision: str,
                   dirty: bool, root: Path = ROOT) -> dict[str, bytes]:
    capability = BOARDS[board]
    if not 0x10000 <= len(data) <= capability.capacity:
        raise ValueError(f"{board} {mode}: factory image outside flash bounds")
    settings = flash_settings(data, board)
    if mode == "standalone" and capability.standalone_flash_freq is not None:
        if settings["flashFreq"] != capability.standalone_flash_freq:
            raise ValueError(f"{board}: standalone flash frequency differs from producer")
    boot, boot_size = complete_image(data[:0x8000], exact=False)
    if any(COMPONENT_PREFIX in segment.data for segment in boot.segments):
        raise ValueError(f"{board}: application substituted for bootloader")
    entries = factory_partitions(board, mode, root)
    if data[0x8000:0x8C00] != partition_binary(entries):
        raise ValueError(f"{board} {mode}: partition table differs from expected layout")
    if hashlib.sha256(data[0xE000:0x10000]).hexdigest() != BOOT_APP0_SHA256:
        raise ValueError(f"{board} {mode}: invalid factory boot metadata")
    occupied = [(0, boot_size), (0x8000, 0x8C00), (0xE000, 0x10000)]
    roles = {name: name for name in ("launcher", "standalone", "rnode")} if mode == "full" else {mode: "app0"}
    components = {}
    for role, name in roles.items():
        entry = entries[name]
        _, size = complete_image(data[entry.offset:entry.offset + entry.size], exact=False)
        component = data[entry.offset:entry.offset + size]
        verify_component(component, board, role, version, revision, dirty, entry.size)
        components[role] = component
        occupied.append((entry.offset, entry.offset + size))
    end = 0
    for start, finish in sorted(occupied):
        if start < end or data[end:start].strip(b"\xff"):
            raise ValueError(f"{board} {mode}: unexplained bytes outside factory components")
        end = finish
    if end != len(data):
        raise ValueError(f"{board} {mode}: unexpected trailing factory data")
    return components
