#!/usr/bin/env python3
"""Hash tracked source contents independently of Git history and generated archives."""

from __future__ import annotations

import hashlib
from pathlib import Path
import stat
import subprocess


ROOT = Path(__file__).resolve().parents[1]
GENERATED = "protocol/prebuilt/xtensa-esp32s3/"
# The committed archives build from these paths and the selected Lite sources
# alone, so notes, docs, CI and firmware sources never stale their provenance.
ARCHIVE_SOURCES = ("protocol/",)


def source_fingerprint(root: Path = ROOT, scope: tuple[str, ...] = ()) -> str:
    def git(*args: str) -> bytes:
        return subprocess.check_output(["git", *args], cwd=root)

    def included(relative: str) -> bool:
        return not relative.startswith(GENERATED) and (not scope or relative.startswith(scope))

    untracked = git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    if any(path and included(path.decode()) for path in untracked):
        raise ValueError("untracked source files; add intended build inputs to Git first")
    records = git("ls-files", "--stage", "-z").split(b"\0")
    digest = hashlib.sha256(b"ratspeak-handheld-source-v1\0")
    if scope:
        digest.update(b"scope\0" + "\0".join(scope).encode() + b"\0")
    count = 0
    for record in sorted((record for record in records if record), key=lambda row: row.split(b"\t", 1)[1]):
        metadata, name = record.split(b"\t", 1)
        mode, _object, stage = metadata.split()
        relative = name.decode()
        if not included(relative):
            continue
        path = root / relative
        if stage != b"0" or mode not in (b"100644", b"100755"):
            raise ValueError(f"unsupported or unmerged source entry: {relative}")
        attributes = path.lstat()
        if not stat.S_ISREG(attributes.st_mode):
            raise ValueError(f"source is not a regular file: {relative}")
        actual_mode = b"100755" if attributes.st_mode & stat.S_IXUSR else b"100644"
        if actual_mode != mode:
            raise ValueError(f"source executable mode differs from Git: {relative}")
        data = path.read_bytes()
        digest.update(mode + b"\0" + name + b"\0")
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(hashlib.sha256(data).digest())
        count += 1
    if count == 0:
        raise ValueError("no tracked build sources")
    return digest.hexdigest()


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", action="store_true", help="only the committed archives' inputs")
    print(source_fingerprint(scope=ARCHIVE_SOURCES if parser.parse_args().archive else ()))
