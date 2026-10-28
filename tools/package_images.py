#!/usr/bin/env python3
"""Verify a board's factory images and write its release .bin files."""

from __future__ import annotations

import argparse
from pathlib import Path

from release_catalog import BOARDS, PACKAGES, ROOT
from release_identity import firmware_version, source_identity
from release_images import launcher_image, verify_factory, verify_launcher


def package(device: str, images: dict[str, bytes], dist: Path, root: Path = ROOT) -> list[Path]:
    board = BOARDS[device]
    if set(images) != set(board.modes):
        raise ValueError(f"{device}: expected images for {', '.join(board.modes)}")
    revision, dirty = source_identity(root)
    version = firmware_version(root, device)
    outputs = {}
    for mode, image in images.items():
        verify_factory(image, device, mode, version, revision, dirty, root)
        outputs[board.image_name(mode)] = image
    if board.launcher_image:
        image = launcher_image(images["standalone"], device, root)
        verify_launcher(image, images["standalone"], device, root)
        outputs[board.launcher_image_name()] = image
    dist.mkdir(parents=True, exist_ok=True)
    written = []
    for name, image in sorted(outputs.items()):
        temporary = dist / f".{name}.tmp"
        temporary.write_bytes(image)
        temporary.replace(dist / name)
        written.append(dist / name)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, choices=BOARDS)
    for mode in PACKAGES:
        parser.add_argument(f"--{mode}", type=Path)
    parser.add_argument("--dist", required=True, type=Path)
    args = parser.parse_args()
    images = {mode: getattr(args, mode).read_bytes() for mode in PACKAGES if getattr(args, mode)}
    for path in package(args.device, images, args.dist):
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
