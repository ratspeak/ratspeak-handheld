#!/usr/bin/env python3
"""Check that launcher, Standalone, and RNode app images fit their OTA slots."""

from __future__ import annotations

import argparse
from pathlib import Path

from release_catalog import APPLICATIONS, BOARDS, ROOT
from release_identity import firmware_version, source_identity
from release_images import verify_component


def image_size(path: Path) -> int:
    if not path.exists():
        raise FileNotFoundError(path)
    return path.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True, choices=BOARDS)
    parser.add_argument("--launcher", type=Path)
    parser.add_argument("--standalone", required=True, type=Path)
    parser.add_argument("--rnode", type=Path)
    args = parser.parse_args()

    failed = False
    revision, dirty = source_identity(ROOT)
    version = firmware_version(ROOT)
    board = BOARDS[args.device]
    if "full" in board.modes:
        if args.launcher is None or args.rnode is None:
            parser.error("dual-boot boards require --launcher and --rnode")
        partitions = board.partitions()
        checks = [(name, getattr(args, name), partitions[name].size)
                  for name in ("launcher", *APPLICATIONS)]
    else:
        if args.launcher is not None or args.rnode is not None:
            parser.error("this board supports only --standalone")
        checks = [("standalone", args.standalone, board.partitions(standalone=True)["app0"].size)]

    for name, path, slot_size in checks:
        size = image_size(path)
        margin = slot_size - size
        print(f"{name}: {size} bytes, slot {slot_size} bytes, margin {margin} bytes")
        try:
            verify_component(path.read_bytes(), args.device, name, version, revision, dirty, slot_size)
        except ValueError as error:
            print(f"error: {error}")
            failed = True

    if failed:
        print("error: at least one application is invalid or exceeds its OTA slot")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
