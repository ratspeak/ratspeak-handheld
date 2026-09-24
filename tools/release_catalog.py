#!/usr/bin/env python3
"""Shared release capabilities; partition CSVs remain the layout authority."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import io
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ("full", "standalone", "rnode")
APPLICATIONS = ("standalone", "rnode")


@dataclass(frozen=True)
class Partition:
    kind: str
    subtype: str
    offset: int
    size: int


@dataclass(frozen=True)
class Board:
    artifact_prefix: str
    flash_size: str
    partition_csv: str | None
    standalone_partition_csv: str
    rnode_target: str | None
    rnode_prep_target: str | None
    renderer: str
    protocol_profile: str
    runtime: str
    app_environments: tuple[str, ...]
    device_aliases: tuple[str, ...]
    post_build_image: str
    standalone_flash_freq: str | None
    rnode_partition_scheme: str | None
    rnode_partition_csv: str | None

    development: bool = False

    @property
    def modes(self) -> tuple[str, ...]:
        return ("standalone",) if self.development else PACKAGES

    @property
    def capacity(self) -> int:
        return int(self.flash_size.removesuffix("MB")) * 1024 * 1024

    def package_name(self, mode: str) -> str:
        if mode not in self.modes:
            raise ValueError(f"unsupported package: {mode}")
        return f"{self.artifact_prefix}-{mode}"

    def app_name(self, mode: str) -> str:
        if mode not in APPLICATIONS:
            raise ValueError(f"unsupported application: {mode}")
        return f"{self.package_name(mode)}.bin"

    def partitions(self, root: Path = ROOT, *, standalone: bool = False) -> dict[str, Partition]:
        path = self.standalone_partition_csv if standalone else self.partition_csv
        if path is None:
            raise ValueError("this board supports only the standalone layout")
        entries = read_partitions(root / path, self.capacity)
        roles = (("app0", "ota_0"), ("app1", "ota_1")) if standalone else (
            ("launcher", "ota_0"), ("standalone", "ota_1"), ("rnode", "ota_2"))
        for name, subtype in roles:
            entry = entries.get(name)
            if entry is None or (entry.kind, entry.subtype) != ("app", subtype):
                raise ValueError(f"{path}: missing or invalid {name} application partition")
        return entries

    def factory_app_offset(self, mode: str, root: Path = ROOT) -> int:
        if mode not in self.modes:
            raise ValueError(f"unsupported factory application: {mode}")
        if mode == "standalone":
            return self.partitions(root, standalone=True)["app0"].offset
        if mode == "rnode":
            entries = read_partitions(root / self.rnode_partition_csv, self.capacity)
            app = entries.get("app0")
            if app is None or (app.kind, app.subtype) != ("app", "ota_0"):
                raise ValueError("RNode factory layout requires app0/ota_0")
            return app.offset
        raise ValueError(f"unsupported factory application: {mode}")


def read_partitions(path: Path, capacity: int) -> dict[str, Partition]:
    """Read explicit CSV geometry, rejecting ambiguous or unsafe layouts."""
    lines = "\n".join(line.split("#", 1)[0] for line in path.read_text().splitlines())
    entries: dict[str, Partition] = {}
    for row in csv.reader(io.StringIO(lines), skipinitialspace=True):
        if not row or not any(value.strip() for value in row):
            continue
        if not 5 <= len(row) <= 6 or (len(row) == 6 and row[5].strip()):
            raise ValueError(f"{path}: incomplete partition row")
        name, kind, subtype, offset, size = (value.strip() for value in row[:5])
        if not name or name in entries:
            raise ValueError(f"{path}: missing or duplicate partition name: {name}")
        try:
            entry = Partition(kind, subtype, int(offset, 0), int(size, 0))
        except ValueError as error:
            raise ValueError(f"{path}: explicit numeric offset and size required for {name}") from error
        if entry.offset < 0x9000 or entry.size <= 0 or entry.offset + entry.size > capacity:
            raise ValueError(f"{path}: {name} outside flash bounds")
        if entry.offset % (0x10000 if kind == "app" else 0x1000) or entry.size % 0x1000:
            raise ValueError(f"{path}: {name} is not partition-aligned")
        entries[name] = entry
    ordered = sorted(entries.items(), key=lambda item: item[1].offset)
    if not ordered:
        raise ValueError(f"{path}: empty partition table")
    for (name, entry), (next_name, following) in zip(ordered, ordered[1:]):
        if entry.offset + entry.size > following.offset:
            raise ValueError(f"{path}: overlapping partitions {name} and {next_name}")
    return entries


def load_boards(path: Path = ROOT / "tools/release_boards.json") -> dict[str, Board]:
    entries = json.loads(path.read_text())
    if not isinstance(entries, dict) or not entries:
        raise ValueError("release catalog must contain boards")
    boards = {}
    assets: set[str] = set()
    environments: set[str] = set()
    aliases: set[str] = set()
    for name, fields in entries.items():
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError(f"invalid release board: {name}")
        fields = dict(fields)
        for key, used in (("app_environments", environments), ("device_aliases", aliases)):
            values = fields[key]
            if not isinstance(values, list):
                raise ValueError(f"{name}: {key} must be a list")
            for value in values:
                if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", value) or value in used:
                    raise ValueError(f"{name}: invalid or duplicate {key}: {value}")
                if key == "device_aliases" and value in entries:
                    raise ValueError(f"{name}: alias shadows a canonical board: {value}")
                used.add(value)
            fields[key] = tuple(values)
        board = Board(**fields)
        if name not in board.app_environments:
            raise ValueError(f"{name}: normal application environment is missing")
        for key, value in fields.items():
            if key in ("app_environments", "device_aliases", "standalone_flash_freq", "development"):
                continue
            if board.development and value is None and key in ("partition_csv", "rnode_target", "rnode_prep_target", "rnode_partition_scheme", "rnode_partition_csv"):
                continue
            if not isinstance(value, str) or not re.fullmatch(r"[a-zA-Z0-9_./-]+", value):
                raise ValueError(f"{name}: invalid catalog value")
        if not isinstance(board.development, bool):
            raise ValueError(f"{name}: development must be boolean")
        if board.development and any(getattr(board, key) is not None for key in ("partition_csv", "rnode_target", "rnode_prep_target", "rnode_partition_scheme", "rnode_partition_csv")):
            raise ValueError(f"{name}: development board must be standalone only")
        for value in (board.artifact_prefix, board.rnode_target, board.rnode_prep_target):
            if value is None and board.development:
                continue
            if not re.fullmatch(r"[a-z][a-z0-9_-]*", value):
                raise ValueError(f"{name}: invalid artifact name or build target")
        if not re.fullmatch(r"[a-z][a-z0-9_-]*\.bin", board.post_build_image):
            raise ValueError(f"{name}: invalid post-build image name")
        if board.standalone_flash_freq not in (None, "80m", "40m"):
            raise ValueError(f"{name}: unsupported standalone merge flash frequency")
        if not board.development and board.rnode_partition_scheme not in ("no_ota", "default_8MB"):
            raise ValueError(f"{name}: unsupported RNode partition scheme")
        if board.flash_size not in ("8MB", "16MB"):
            raise ValueError(f"{name}: unsupported flash capacity")
        if (board.renderer, board.protocol_profile, board.runtime) not in (
            ("lvgl", "small", "dedicated"), ("canvas", "micro", "cooperative")
        ):
            raise ValueError(f"{name}: unsupported renderer/protocol/runtime combination")
        for relative in (board.partition_csv, board.standalone_partition_csv, board.rnode_partition_csv):
            if relative is None and board.development:
                continue
            if Path(relative).is_absolute() or ".." in Path(relative).parts:
                raise ValueError(f"{name}: partition path must be source-relative")
        for asset in board_assets(board):
            if asset in assets:
                raise ValueError(f"duplicate release asset: {asset}")
            assets.add(asset)
        boards[name] = board
    return boards


def board_assets(board: Board) -> set[str]:
    return {f"{board.package_name(mode)}.zip" for mode in board.modes} | {
        board.app_name(mode) for mode in APPLICATIONS if mode in board.modes
    }


# Unreleased development targets participate in local builds, not public assets.
ALL_BOARDS = load_boards()
BOARDS = {name: board for name, board in ALL_BOARDS.items() if not board.development}


def board_for_environment(environment: str) -> tuple[str, Board]:
    for name, board in ALL_BOARDS.items():
        if environment in board.app_environments:
            return name, board
    supported = ", ".join(env for board in ALL_BOARDS.values() for env in board.app_environments)
    raise ValueError(f"Unknown handheld app environment {environment!r}; supported environments: {supported}")


def site_contract() -> dict:
    """Capabilities and aliases only; deployment activation/tag is a site policy."""
    return {"schemaVersion": 1, "packages": list(PACKAGES), "boards": {
        name: {"artifactPrefix": board.artifact_prefix, "flashSize": board.flash_size,
               "capacity": board.capacity, "aliases": list(board.device_aliases)}
        for name, board in BOARDS.items()
    }}


def check_rnode_partition_producers(root: Path = ROOT) -> None:
    """Check the source producer; binary partition correspondence is a later gate."""
    makefile = (root / "vendor/rnode_firmware/Makefile").read_text()
    for name, board in BOARDS.items():
        variable = board.rnode_target.upper() + "_FQBN"
        match = re.search(rf"^{variable}\s*:=\s*(\S+)\s*$", makefile, re.MULTILINE)
        if not match:
            raise ValueError(f"{name}: missing explicit RNode FQBN producer {variable}")
        fqbn = match.group(1).split(":", 3)
        if len(fqbn) != 4 or fqbn[:3] != ["esp32", "esp32", "esp32s3"]:
            raise ValueError(f"{name}: RNode producer must target ESP32-S3")
        options = dict(item.split("=", 1) for item in fqbn[3].split(","))
        if (options.get("FlashSize") != board.flash_size.removesuffix("B") or
                options.get("PartitionScheme") != board.rnode_partition_scheme):
            raise ValueError(f"{name}: RNode flash/partition producer differs from the catalog")


def firmware_assets() -> set[str]:
    return set().union(*(board_assets(board) for board in BOARDS.values()))


def make_settings(device: str) -> dict[str, str]:
    board = BOARDS[device]
    partitions = board.partitions()
    settings = {
        "BRAND": board.artifact_prefix,
        "PARTITION_CSV": board.partition_csv,
        "PARTITIONS_BASENAME": f"partitions-{board.flash_size.lower()}.bin",
        "FLASH_SIZE": board.flash_size,
        "RNODE_TARGET": board.rnode_target,
        "RNODE_PREP_TARGET": board.rnode_prep_target,
        "APP_STANDALONE_NAME": board.app_name("standalone").removesuffix(".bin"),
        "APP_RNODE_NAME": board.app_name("rnode").removesuffix(".bin"),
        "POST_BUILD_IMAGE": board.post_build_image,
        "STANDALONE_FLASH_FREQ": board.standalone_flash_freq or "",
        "STANDALONE_FACTORY_OFFSET": hex(board.factory_app_offset("standalone")),
        "STANDALONE_FACTORY_SLOT_SIZE": hex(board.partitions(standalone=True)["app0"].size),
        "RNODE_FACTORY_OFFSET": hex(board.factory_app_offset("rnode")),
    }
    for name in ("launcher", *APPLICATIONS):
        settings[f"{name.upper()}_SLOT_SIZE"] = hex(partitions[name].size)
        settings[f"{name.upper()}_OFFSET"] = hex(partitions[name].offset)
    return settings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("devices", "matrix", "make", "site-contract", "check"))
    parser.add_argument("--device", choices=BOARDS)
    args = parser.parse_args()
    if args.command == "devices":
        print(" ".join(BOARDS))
    elif args.command == "matrix":
        print(json.dumps({"device": list(BOARDS)}, separators=(",", ":")))
    elif args.command == "site-contract":
        print(json.dumps(site_contract(), indent=2))
    elif args.command == "make":
        if not args.device:
            parser.error("make requires --device")
        print(" ".join(f"{name}={value}" for name, value in make_settings(args.device).items()))
    else:
        for board in BOARDS.values():
            board.partitions()
            board.partitions(standalone=True)
        check_rnode_partition_producers()
        print(f"release catalog: PASS ({len(BOARDS)} boards, {len(firmware_assets())} firmware assets)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
