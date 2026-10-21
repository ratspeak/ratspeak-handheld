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
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True, choices=BOARDS)
    parser.add_argument("--bootloader", required=True, type=Path)
    parser.add_argument("--partitions", required=True, type=Path)
    parser.add_argument("--boot-app0", required=True, type=Path)
    parser.add_argument("--launcher", required=True, type=Path)
    parser.add_argument("--standalone", required=True, type=Path)
    parser.add_argument("--rnode", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
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
        positional = [
            "0x0000", str(args.bootloader),
            "0x8000", str(args.partitions),
            "0xe000", str(args.boot_app0),
            hex(partitions["launcher"].offset), str(args.launcher),
            hex(partitions["standalone"].offset), str(args.standalone),
            hex(partitions["rnode"].offset), str(args.rnode),
        ]
        # esptool's merge subcommand/flag spelling has shifted release to release --
        # merge_bin/--flash_mode (older, underscore) vs merge-bin/--flash-mode (newer,
        # hyphenated). PlatformIO's own tool-esptoolpy resolution has drifted under the
        # same declared version range before (confirmed on 2026-10-19), so this project
        # can't assume which style a fresh install actually has -- try the older
        # spelling first (seen working on every version encountered so far), fall back
        # to the newer one.
        legacy = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
                  "--flash_mode", "dio", "--flash_size", board.flash_size,
                  "--output", str(temporary), *positional]
        modern = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin",
                  "--flash-mode", "dio", "--flash-size", board.flash_size,
                  "--output", str(temporary), *positional]
        if subprocess.run(legacy).returncode != 0:
            subprocess.run(modern, check=True)
        verify_factory(temporary.read_bytes(), args.device, "full", version, revision, dirty)
        temporary.replace(args.output)
    print(f"dual firmware image written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
