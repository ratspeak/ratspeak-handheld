#!/usr/bin/env python3
"""Verify committed Xtensa archives against the C header and provenance hashes."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
from pathlib import Path

from source_fingerprint import source_fingerprint


ROOT = Path(__file__).resolve().parents[1]
PREBUILT = ROOT / "protocol/prebuilt/xtensa-esp32s3"
HEADER = ROOT / "protocol/include/ratspeak_protocol.h"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="allow development archives built with uncommitted Lite changes",
    )
    args = parser.parse_args()

    header = HEADER.read_text(encoding="utf-8")
    expected = set(re.findall(r"\b(rs_handheld_[A-Za-z0-9_]+)\s*\(", header))
    provenance = {}
    for line in (PREBUILT / "PROVENANCE.txt").read_text(encoding="utf-8").splitlines():
        if ": " in line:
            key, value = line.split(": ", 1)
            provenance[key] = value

    if len(expected) != 88:
        raise SystemExit(f"C ABI declaration count drifted: expected 88, found {len(expected)}")

    source_roots = {
        "rsReticulumLite": ROOT.parent / "rsReticulumLite",
        "rsLXMFLite": ROOT.parent / "rsLXMFLite",
    }
    for name, source_root in source_roots.items():
        recorded = provenance.get(name, "")
        if not args.allow_dirty and recorded.endswith(" +dirty"):
            raise SystemExit(f"{name} archive provenance is dirty; rebuild after committing")
        recorded_commit = recorded.split(" ", 1)[0]
        current_commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source_root, text=True
        ).strip()
        if recorded_commit != current_commit:
            raise SystemExit(
                f"{name} archive provenance is stale: recorded={recorded_commit}, "
                f"current={current_commit}"
            )
        if not args.allow_dirty and subprocess.check_output(
            ["git", "status", "--porcelain", "--untracked-files=all"], cwd=source_root
        ):
            raise SystemExit(f"{name} source checkout is dirty")

    # A content fingerprint lets source and generated archives share one commit
    # without making the provenance depend on that commit's own identifier.
    if provenance.get("ratspeak-handheld.source-sha256") != source_fingerprint(ROOT):
        raise SystemExit("ratspeak-handheld archive provenance is stale: source fingerprint differs")

    for profile in ("small", "micro"):
        archive = PREBUILT / profile / "libratspeak_protocol.a"
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        recorded = provenance.get(f"{profile}.sha256")
        if digest != recorded:
            raise SystemExit(
                f"{profile} archive hash mismatch: provenance={recorded}, actual={digest}"
            )
        output = subprocess.check_output(["nm", "-g", str(archive)], text=True, errors="replace")
        exported = {name for name in expected if re.search(rf"\b{name}$", output, re.MULTILINE)}
        missing = sorted(expected - exported)
        if missing:
            raise SystemExit(f"{profile} archive is missing C ABI symbols: {', '.join(missing)}")

    qualifier = ", dirty development provenance allowed" if args.allow_dirty else ""
    print(
        f"prebuilt Rust archives: PASS (2 profiles, {len(expected)} ABI symbols{qualifier})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
