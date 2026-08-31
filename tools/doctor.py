#!/usr/bin/env python3
"""Check the local toolchain and source layout before a handheld build."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICES = ("tdeck", "tpager", "cardputer")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", choices=DEVICES, default="tdeck")
    parser.add_argument(
        "--source",
        action="store_true",
        help="also require the sibling Lite/full-Rust sources used by protocol tests",
    )
    args = parser.parse_args()

    failures: list[str] = []

    def require_command(name: str) -> None:
        if shutil.which(name) is None:
            failures.append(f"missing command: {name}")

    def require_file(path: Path, label: str) -> None:
        if not path.is_file():
            failures.append(f"missing {label}: {path}")

    require_command("python3")
    require_command("arduino-cli")
    for module in ("platformio", "esptool"):
        result = subprocess.run(
            [sys.executable, "-c", f"import {module}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            failures.append(f"missing Python module: {module}")

    profile = "micro" if args.device == "cardputer" else "small"
    require_file(
        ROOT / "protocol/prebuilt/xtensa-esp32s3" / profile / "libratspeak_protocol.a",
        f"{profile} Rust firmware archive",
    )
    require_file(
        ROOT / "protocol/include/ratspeak_protocol.h",
        "Rust C ABI header",
    )
    require_file(ROOT / "vendor/rnode_firmware/Makefile", "vendored RNode build")

    if args.source:
        require_command("cargo")
        require_command("rustup")
        siblings = {
            "rsReticulumLite": "Cargo.toml",
            "rsLXMFLite": "Cargo.toml",
            "rsReticulum": "Cargo.toml",
            "rsLXMF": "Cargo.toml",
        }
        workspace = ROOT.parent
        for label, relative in siblings.items():
            require_file(workspace / label / relative, label)

    if failures:
        print("Build doctor found problems:")
        for failure in failures:
            print(f"  - {failure}")
        print(f"\nInstall prerequisites, then run: make setup DEVICE={args.device}")
        return 1

    scope = "source + firmware" if args.source else "firmware"
    print(f"Build doctor: PASS ({args.device}, {scope})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
