#!/usr/bin/env python3
"""Build a merged handheld image with launcher, Standalone, and RNode."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


from release_catalog import APPLICATIONS, BOARDS, ROOT
from release_identity import firmware_version, source_identity
from release_images import verify_component, verify_factory


def require_file(path: Path, label: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{label} not found: {path}")
    if not path.is_file():
        raise FileNotFoundError(f"{label} is not a file: {path}")



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, choices=BOARDS, help="board with full/dual-boot packages; standalone-only boards are unsupported")
    parser.add_argument("--bootloader", required=True, type=Path, help="ESP32-S3 bootloader binary for offset 0x0")
    parser.add_argument("--partitions", required=True, type=Path, help="dual-boot partition-table binary for offset 0x8000")
    parser.add_argument("--boot-app0", required=True, type=Path, help="SDK boot_app0.bin metadata for offset 0xe000")
    parser.add_argument("--launcher", required=True, type=Path, help="launcher application binary")
    parser.add_argument("--standalone", required=True, type=Path, help="Standalone application binary")
    parser.add_argument("--rnode", required=True, type=Path, help="RNode application binary")
    parser.add_argument("--output", required=True, type=Path, help="merged factory binary; existing file is replaced after verification")
    args = parser.parse_args()

    for label, path in (
        ("bootloader", args.bootloader),
        ("partition table", args.partitions),
        ("boot_app0", args.boot_app0),
        ("launcher app", args.launcher),
        ("Standalone app", args.standalone),
        ("RNode app", args.rnode),
    ):
        require_file(path, label)

    board = BOARDS[args.device]
    partitions = board.partitions()
    version = firmware_version(ROOT)
    revision, dirty = source_identity(ROOT)
    for name in ("launcher", *APPLICATIONS):
        verify_component(getattr(args, name).read_bytes(), args.device, name, version, revision, dirty, partitions[name].size)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="handheld-merge-", dir=args.output.parent) as directory:
        temporary = Path(directory) / "merged.bin"
        cmd = [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32s3",
            "merge-bin",
            "--flash-mode",
            "dio",
            "--flash-size",
            board.flash_size,
            "--output",
            str(temporary),
            "0x0000",
            str(args.bootloader),
            "0x8000",
            str(args.partitions),
            "0xe000",
            str(args.boot_app0),
            hex(partitions["launcher"].offset),
            str(args.launcher),
            hex(partitions["standalone"].offset),
            str(args.standalone),
            hex(partitions["rnode"].offset),
            str(args.rnode),
        ]
        subprocess.run(cmd, check=True)
        verify_factory(temporary.read_bytes(), args.device, "full", version, revision, dirty)
        temporary.replace(args.output)
    print(f"dual firmware image written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
