#!/usr/bin/env python3
"""Package a merged ESP32-S3 image with a web-flasher manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tempfile
import zipfile
from pathlib import Path


from release_catalog import BOARDS, PACKAGES, ROOT
from release_identity import firmware_version, validate_version, source_identity
from release_images import flash_settings, verify_factory


NOTICES = {
    "LICENSE": "LICENSE",
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "vendor/rnode_firmware/LICENSE": "licenses/RNode-GPL-3.0.txt",
    "licenses/THIRD-PARTY.txt": "licenses/THIRD-PARTY.txt",
    "licenses/manifest.json": "licenses/manifest.json",
}


def make_manifest(
    image: bytes, name: str, board: str, package: str, flash_size: str,
    version: str, revision: str, dirty: bool,
) -> dict:
    if board not in BOARDS or package not in BOARDS[board].modes:
        raise ValueError("unsupported board or package")
    capability = BOARDS[board]
    brand, expected_flash = capability.artifact_prefix, capability.flash_size
    if name != f"{brand}-{package}" or flash_size != expected_flash:
        raise ValueError("package name/flash size does not match the selected board")
    validate_version(version)
    if not re.fullmatch(r"[0-9a-f]{40}", revision) or not isinstance(dirty, bool):
        raise ValueError("invalid source identity")
    capacity = int(expected_flash.removesuffix("MB")) * 1024 * 1024
    if len(image) < 0x10000 or len(image) > capacity or image[0] != 0xE9:
        raise ValueError("merged image lacks an ESP header or is outside the board flash bounds")
    return {
        "schemaVersion": 1,
        "product": "ratspeak-handheld",
        "board": board,
        "package": package,
        "installMode": "factory",
        "version": version,
        "sourceRevision": revision,
        "sourceDirty": dirty,
        "chipFamily": "ESP32-S3",
        "flashSize": flash_size,
        **flash_settings(image, board),
        "parts": [{
            "path": f"{name}.bin", "offset": "0x0000", "size": len(image),
            "sha256": hashlib.sha256(image).hexdigest(),
        }],
    }


def write_package(output: Path, image: bytes, manifest: dict, root: Path) -> None:
    entries = {manifest["parts"][0]["path"]: image}
    entries["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    for source, destination in NOTICES.items():
        data = (root / source).read_bytes()
        if not data.strip():
            raise ValueError(f"empty release notice: {source}")
        entries[destination] = data
    output.parent.mkdir(parents=True, exist_ok=True)
    # Fixed metadata makes repeated packaging of identical inputs reproducible.
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".zip", delete=False) as temp:
        temporary = Path(temp.name)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(entries.items()):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--device", required=True, choices=BOARDS)
    parser.add_argument("--package", required=True, choices=PACKAGES)
    parser.add_argument("--flash-size", required=True, choices=("8MB", "16MB"))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    revision, dirty = source_identity(ROOT)
    image = args.image.read_bytes()
    manifest = make_manifest(
        image, args.name, args.device, args.package, args.flash_size,
        firmware_version(ROOT, args.device), revision, dirty,
    )
    verify_factory(image, args.device, args.package, manifest["version"], revision, dirty, ROOT)
    write_package(args.output, image, manifest, ROOT)
    print(f"firmware package written to {args.output} (source {revision}, dirty={dirty})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
