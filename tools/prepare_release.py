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

from release_catalog import APPLICATIONS, BOARDS, ROOT, firmware_assets
from release_identity import check_workspace, firmware_version, load_identity, source_identity, source_pins, validate_version
from release_images import COMPONENT_PREFIX, verify_factory, verify_launcher


RELEASE_BOARDS = tuple(BOARDS)
SOURCE_NAME = "ratspeak-handheld-source.tar.gz"
NOTICE_NAME = "ratspeak-handheld-notices.zip"
CHECKSUM_NAME = "SHA256SUMS"
SERVICE_MARKER = b"[SERVICE] Dedicated network/storage task started\x00"
GENERATED = {SOURCE_NAME, NOTICE_NAME, CHECKSUM_NAME}
NOTICES = {
    "LICENSE": "LICENSE",
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "vendor/rnode_firmware/LICENSE": "licenses/RNode-GPL-3.0.txt",
    "licenses/THIRD-PARTY.txt": "licenses/THIRD-PARTY.txt",
    "licenses/manifest.json": "licenses/manifest.json",
}



def verify_tag(tag: str, version: str) -> None:
    validate_version(version)
    pattern = re.escape("v" + version)
    if "-" not in version:
        pattern += r"(?:-[0-9A-Za-z.-]+)?"
    if not re.fullmatch(pattern, tag):
        raise ValueError(f"release tag {tag!r} does not match firmware {version}")


def verify_firmware(dist: Path, revision: str, root: Path) -> str:
    actual = {path.name for path in dist.iterdir()} - GENERATED
    expected = firmware_assets()
    if actual != expected:
        raise ValueError(f"release asset mismatch; missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}")
    versions = set()
    for board in RELEASE_BOARDS:
        capability = BOARDS[board]
        version = firmware_version(root, board)
        versions.add(version)
        partitions = capability.partitions(root) if "full" in capability.modes else None
        images = {mode: (dist / capability.image_name(mode)).read_bytes() for mode in capability.modes}
        components = {mode: verify_factory(image, board, mode, version, revision, False, root)
                      for mode, image in images.items()}
        for mode in APPLICATIONS:
            if mode not in capability.modes:
                continue
            app = components[mode][mode]
            if mode == "standalone" and capability.runtime == "dedicated" and SERVICE_MARKER not in app:
                raise ValueError(f"{board}: release application does not use the dedicated service task")
            if partitions and images["full"][partitions[mode].offset:partitions[mode].offset + len(app)] != app:
                raise ValueError(f"{board} {mode}: application differs from dual-boot package")
        if capability.launcher_image:
            verify_launcher((dist / capability.launcher_image_name()).read_bytes(), images["standalone"], board, root)
    if len(versions) != 1:
        raise ValueError("release boards disagree on firmware version")
    return versions.pop()


def image_identities(dist: Path) -> set[tuple[str, str]]:
    """(version, revision) of every component embedded in the release images."""
    found = set()
    for name in sorted(firmware_assets()):
        for match in re.finditer(re.escape(COMPONENT_PREFIX) + rb"([^\0]*)\0", (dist / name).read_bytes()):
            board, role, version, revision, dirty = match.group(1).decode().split(":")
            found.add((version, revision))
    return found


def source_refs(root: Path) -> dict[str, str]:
    return {"ratspeak-handheld": source_identity(root)[0], **source_pins(root)}


def write_sources(destination: Path, workspace: Path, refs: dict[str, str], roles: dict[str, str] | None = None) -> None:
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
            metadata = {"schemaVersion": 1, "sources": refs}
            if roles is not None:
                if set(roles) != set(refs):
                    raise ValueError("source roles do not cover the source archive")
                metadata["sourceRoles"] = roles
            data = (json.dumps(metadata, indent=2) + "\n").encode()
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
    check_workspace(root)
    version = verify_firmware(dist, revision, root)
    refs = source_refs(root)
    for repo, expected in refs.items():
        actual, dirty = source_identity(root.parent / repo)
        if actual != expected or dirty:
            raise ValueError(f"{repo}: source checkout must be clean at {expected}")
    roles = {"ratspeak-handheld": "firmware",
             **{name: source["role"] for name, source in load_identity(root)["sources"].items()}}
    write_sources(dist / SOURCE_NAME, root.parent, refs, roles)
    write_notices(dist / NOTICE_NAME, root)
    checksums = []
    for name in sorted(firmware_assets() | {SOURCE_NAME, NOTICE_NAME}):
        checksums.append(f"{hashlib.sha256((dist / name).read_bytes()).hexdigest()}  {name}\n")
    (dist / CHECKSUM_NAME).write_text("".join(checksums))
    print(f"release assets verified: {version}, {len(BOARDS)} boards, "
          f"{len(firmware_assets())} firmware images; source {revision}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    parser.add_argument("--check-tag", help="check a prepared release's tag/version binding only")
    parser.add_argument("--source-revision", help="expected source commit when checking a release tag")
    args = parser.parse_args()
    if args.check_tag:
        identities = image_identities(args.dist)
        if len(identities) != 1:
            raise ValueError("release images disagree on version or source revision")
        version, revision = identities.pop()
        verify_tag(args.check_tag, version)
        if args.source_revision and revision != args.source_revision:
            raise ValueError("release images do not match the tagged source commit")
        print(f"release tag verified: {args.check_tag}")
        return 0
    prepare(args.dist, ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
