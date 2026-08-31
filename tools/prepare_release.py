#!/usr/bin/env python3
"""Validate release-board artifacts and collect their source and notices."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import re
import subprocess
import tarfile
import zipfile
from pathlib import Path

from package_merged_zip import BOARDS, NOTICES, PACKAGES, ROOT, firmware_version, make_manifest, source_identity


RELEASE_BOARDS = ("tdeck", "tpager")
SOURCE_NAME = "ratspeak-handheld-source.tar.gz"
NOTICE_NAME = "ratspeak-handheld-notices.zip"
METADATA_NAME = "release-manifest.json"
CHECKSUM_NAME = "SHA256SUMS"
SERVICE_MARKER = b"[SERVICE] Dedicated network/storage task started\x00"
GENERATED = {SOURCE_NAME, NOTICE_NAME, METADATA_NAME, CHECKSUM_NAME}


def firmware_assets() -> set[str]:
    names = set()
    for board in RELEASE_BOARDS:
        brand = BOARDS[board][0]
        names.update(f"{brand}-{mode}.zip" for mode in PACKAGES)
        suffix = "app" if board == "tpager" else "m5launcher"
        names.update(f"{brand}-{mode}-{suffix}.bin" for mode in ("standalone", "rnode"))
    return names


def verify_application(data: bytes) -> None:
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
        if image.data_length + 32 != len(data):
            raise ValueError("truncated application or unexpected trailing data")
    except Exception as error:
        raise ValueError(f"invalid complete application image: {error}") from error


def verify_tag(tag: str, version: str) -> None:
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("release manifest has an invalid firmware version")
    pattern = re.escape("v" + version)
    if "-" not in version:
        pattern += r"(?:-[0-9A-Za-z.-]+)?"
    if not re.fullmatch(pattern, tag):
        raise ValueError(f"release tag {tag!r} does not match firmware {version}")


def verify_package(path: Path, board: str, mode: str, revision: str, root: Path) -> tuple[dict, bytes]:
    brand, flash = BOARDS[board]
    name = f"{brand}-{mode}"
    allowed = {f"{name}.bin", "manifest.json", *NOTICES.values()}
    with zipfile.ZipFile(path) as archive:
        members = archive.infolist()
        if len(members) != len(allowed) or {item.filename for item in members} != allowed:
            raise ValueError(f"{path.name}: unexpected, missing or duplicate ZIP members")
        # Refuse oversized expanded data before allocating it.
        if any(item.file_size > 16 * 1024 * 1024 for item in members):
            raise ValueError(f"{path.name}: ZIP member exceeds the release limit")
        if any((item.external_attr >> 16) & 0o170000 not in (0, 0o100000) for item in members):
            raise ValueError(f"{path.name}: ZIP contains links or special files")
        manifest = json.loads(archive.read("manifest.json"))
        image = archive.read(f"{name}.bin")
        expected = make_manifest(image, name, board, mode, flash, firmware_version(root, board), revision, False)
        if manifest != expected:
            raise ValueError(f"{path.name}: manifest, image hash or clean-source identity mismatch")
        if len(image) < 24 or image[0] != 0xE9:
            raise ValueError(f"{path.name}: missing ESP image header")
        for source, destination in NOTICES.items():
            if archive.read(destination) != (root / source).read_bytes():
                raise ValueError(f"{path.name}: notice differs from the release source: {destination}")
    return manifest, image


def verify_firmware(dist: Path, revision: str, root: Path) -> list[dict]:
    actual = {path.name for path in dist.iterdir()}
    expected = firmware_assets()
    if actual != expected:
        raise ValueError(f"release asset mismatch; missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}")
    manifests = []
    for board in RELEASE_BOARDS:
        brand = BOARDS[board][0]
        images = {}
        for mode in PACKAGES:
            manifest, images[mode] = verify_package(dist / f"{brand}-{mode}.zip", board, mode, revision, root)
            manifests.append(manifest)
        suffix = "app" if board == "tpager" else "m5launcher"
        for mode, slot, offset in (("standalone", 0x400000, 0x110000), ("rnode", 0x300000, 0x510000)):
            app = (dist / f"{brand}-{mode}-{suffix}.bin").read_bytes()
            if not 24 <= len(app) <= slot or app[0] != 0xE9:
                raise ValueError(f"{board} {mode}: invalid application image or slot overflow")
            verify_application(app)
            if mode == "standalone" and SERVICE_MARKER not in app:
                raise ValueError(f"{board}: release application does not use the dedicated service task")
            if images[mode][0x10000:0x10000 + len(app)] != app:
                raise ValueError(f"{board} {mode}: application differs from factory package")
            if images["full"][offset:offset + len(app)] != app:
                raise ValueError(f"{board} {mode}: application differs from dual-boot package")
    if len({manifest["version"] for manifest in manifests}) != 1:
        raise ValueError("release boards/packages disagree on firmware version")
    return manifests


def source_refs(root: Path) -> dict[str, str]:
    workflow = (root / ".github/workflows/build.yml").read_text()
    refs = {"ratspeak-handheld": source_identity(root)[0]}
    for repo, variable in (
        ("rsReticulumLite", "RNS_LITE_COMMIT"), ("rsLXMFLite", "LXMF_LITE_COMMIT"),
        ("rsReticulum", "RNS_FULL_COMMIT"), ("rsLXMF", "LXMF_FULL_COMMIT"),
    ):
        match = re.search(rf"^  {variable}: ([0-9a-f]{{40}})$", workflow, re.MULTILINE)
        if not match:
            raise ValueError(f"missing reviewed source pin: {variable}")
        refs[repo] = match.group(1)
    return refs


def write_sources(destination: Path, workspace: Path, refs: dict[str, str]) -> None:
    """Include committed source, never working files, .git history or credentials."""
    with destination.open("wb") as output, gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|") as bundle:
            for repo, revision in refs.items():
                data = subprocess.check_output(["git", "archive", "--format=tar", revision], cwd=workspace / repo)
                with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
                    for member in archive:
                        if not (member.isfile() or member.isdir()):
                            raise ValueError(f"source bundle refuses links/special files: {repo}/{member.name}")
                        member.name = f"{repo}/{member.name}"
                        member.uid = member.gid = member.mtime = 0
                        member.uname = member.gname = ""
                        member.pax_headers = {}
                        bundle.addfile(member, archive.extractfile(member) if member.isfile() else None)
            data = (json.dumps({"schemaVersion": 1, "sources": refs}, indent=2) + "\n").encode()
            info = tarfile.TarInfo("SOURCE.json")
            info.size = len(data)
            info.mode = 0o644
            bundle.addfile(info, io.BytesIO(data))


def write_notices(destination: Path, root: Path) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for source, name in sorted(NOTICES.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, (root / source).read_bytes())


def prepare(dist: Path, root: Path) -> None:
    from collect_licenses import check_bundle

    check_bundle(root / "licenses")
    revision, dirty = source_identity(root)
    if dirty:
        raise ValueError("official release preparation requires a clean source checkout")
    for name in GENERATED:
        if (dist / name).exists():
            raise ValueError(f"refusing to overwrite release output: {name}; use a fresh artifact directory")
    manifests = verify_firmware(dist, revision, root)
    refs = source_refs(root)
    for repo, expected in refs.items():
        actual, dirty = source_identity(root.parent / repo)
        if actual != expected or dirty:
            raise ValueError(f"{repo}: source checkout must be clean at {expected}")
    write_sources(dist / SOURCE_NAME, root.parent, refs)
    write_notices(dist / NOTICE_NAME, root)
    assets = {}
    for name in sorted(firmware_assets() | {SOURCE_NAME, NOTICE_NAME}):
        data = (dist / name).read_bytes()
        assets[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    metadata = {
        "schemaVersion": 1, "product": "ratspeak-handheld", "version": manifests[0]["version"],
        "sources": refs, "boards": list(RELEASE_BOARDS), "installMode": "factory", "assets": assets,
    }
    (dist / METADATA_NAME).write_text(json.dumps(metadata, indent=2) + "\n")
    checksums = []
    for name in sorted(firmware_assets() | {SOURCE_NAME, NOTICE_NAME, METADATA_NAME}):
        checksums.append(f"{hashlib.sha256((dist / name).read_bytes()).hexdigest()}  {name}\n")
    (dist / CHECKSUM_NAME).write_text("".join(checksums))
    print(f"release assets verified: 2 boards, 6 factory ZIPs, 4 app images; source {revision}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    parser.add_argument("--check-tag", help="check a prepared release's tag/version binding only")
    parser.add_argument("--source-revision", help="expected source commit when checking a release tag")
    args = parser.parse_args()
    if args.check_tag:
        metadata = json.loads((args.dist / METADATA_NAME).read_text())
        verify_tag(args.check_tag, metadata["version"])
        if args.source_revision and metadata["sources"]["ratspeak-handheld"] != args.source_revision:
            raise ValueError("release manifest does not match the tagged source commit")
        print(f"release tag verified: {args.check_tag}")
        return 0
    prepare(args.dist, ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
