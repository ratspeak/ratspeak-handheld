#!/usr/bin/env python3
"""Package a merged ESP32-S3 image with a web-flasher manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BOARDS = {
    "tdeck": ("rsdeck", "16MB"),
    "tpager": ("rspager", "16MB"),
    "cardputer": ("rscardputer", "8MB"),
}
PACKAGES = ("full", "standalone", "rnode")
NOTICES = {
    "LICENSE": "LICENSE",
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "vendor/rnode_firmware/LICENSE": "licenses/RNode-GPL-3.0.txt",
    "licenses/THIRD-PARTY.txt": "licenses/THIRD-PARTY.txt",
    "licenses/manifest.json": "licenses/manifest.json",
}


def source_identity(root: Path) -> tuple[str, bool]:
    try:
        top = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=root, text=True, stderr=subprocess.DEVNULL
        ).strip()
        if Path(top).resolve() != root.resolve():
            raise ValueError("not the source repository root")
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    except (subprocess.CalledProcessError, ValueError):
        # Source-release archives have no Git history. Let users build them, but
        # never label a local archive build as an unmodified official checkout.
        metadata = root.parent / "SOURCE.json"
        if not metadata.is_file():
            raise ValueError("source identity requires a Git checkout or the complete release source archive") from None
        revision = json.loads(metadata.read_text()).get("sources", {}).get(root.name, "")
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError("invalid source-archive identity")
        return revision, True
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("packaging requires a Git checkout with a full source commit")
    status = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=root, text=True
    )
    return revision, bool(status.strip())


def firmware_version(root: Path, board: str) -> str:
    config = (root / "src/boards" / board / "config/BoardConfig.h").read_text()
    match = re.search(r'^#define RSDECK_VERSION_STRING\s+"([^"]+)"', config, re.MULTILINE)
    if not match:
        raise ValueError("board firmware version is missing")
    return match.group(1)


def make_manifest(
    image: bytes, name: str, board: str, package: str, flash_size: str,
    version: str, revision: str, dirty: bool,
) -> dict:
    if board not in BOARDS or package not in PACKAGES:
        raise ValueError("unsupported board or package")
    brand, expected_flash = BOARDS[board]
    if name != f"{brand}-{package}" or flash_size != expected_flash:
        raise ValueError("package name/flash size does not match the selected board")
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("invalid firmware version")
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
        "flashMode": "dio",
        "flashFreq": "80m",
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
    write_package(args.output, image, manifest, ROOT)
    print(f"firmware package written to {args.output} (source {revision}, dirty={dirty})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
