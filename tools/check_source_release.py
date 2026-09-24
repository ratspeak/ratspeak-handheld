#!/usr/bin/env python3
"""Validate the public source/release contract for the unified firmware."""

from __future__ import annotations

import configparser
import re
from pathlib import Path

from release_catalog import BOARDS, check_rnode_partition_producers
from release_identity import check_local


ROOT = Path(__file__).resolve().parents[1]
SHA = re.compile(r"^[0-9a-f]{40}$")
IGNORED_PARTS = {".git", ".pio", ".venv", "build", "dist", "target"}


def fail(message: str) -> None:
    raise SystemExit(f"source-release contract failed: {message}")


def main() -> int:
    required = [
        ".github/workflows/build.yml",
        ".github/release-notes.txt",
        "CONTRIBUTORS.md",
        "LICENSE",
        "README.md",
        "SECURITY.md",
        "THIRD_PARTY_NOTICES.md",
        "protocol/rust-toolchain.toml",
        "tools/check_prebuilt.py",
        "tools/source_fingerprint.py",
        "tools/doctor.py",
        "tools/collect_licenses.py",
        "tools/prepare_release.py",
        "tools/release_catalog.py",
        "tools/release_boards.json",
        "tools/release_identity.json",
        "tools/release_identity.py",
        "tools/check_build_identity.py",
        "src/core/config/FirmwareVersion.h",
        "licenses/THIRD-PARTY.txt",
        "licenses/manifest.json",
    ]
    missing = [path for path in required if not (ROOT / path).is_file()]
    if missing:
        fail(f"missing required files: {', '.join(missing)}")

    for path in ROOT.rglob("*"):
        if any(part in IGNORED_PARTS for part in path.parts) or not path.is_file():
            continue
        lowered = path.name.lower()
        if (lowered in {"agents.md", "claude.md", ".cursorrules"} or lowered.startswith("codex")
                or any(part.lower() in {".agents", ".claude", ".codex"} for part in path.relative_to(ROOT).parts)):
            fail(f"internal agent file present: {path.relative_to(ROOT)}")
        if path.suffix.lower() == ".md":
            contents = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"(?:CLAUDE|CODEX[^/\\\s`]*)\.md", contents, re.IGNORECASE):
                fail(f"public documentation references an internal agent file: {path.relative_to(ROOT)}")
            if re.search(r"/(?:Users|home)/[^/\s]+/|docs/(?:active|audits|internal-ai-agent-backups)/", contents):
                fail(f"public documentation references private workspace material: {path.relative_to(ROOT)}")

    allowed_markdown = {
        Path("CONTRIBUTORS.md"),
        Path("README.md"),
        Path("SECURITY.md"),
        Path("THIRD_PARTY_NOTICES.md"),
    }
    markdown = {
        path.relative_to(ROOT)
        for path in ROOT.rglob("*.md")
        if not any(part in IGNORED_PARTS for part in path.parts)
    }
    unexpected_markdown = sorted(markdown - allowed_markdown)
    if unexpected_markdown:
        fail(
            "unexpected Markdown files: "
            + ", ".join(str(path) for path in unexpected_markdown)
        )

    workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
    for action in re.findall(r"^\s*(?:-\s+)?uses:\s+([^\s#]+)", workflow, re.MULTILINE):
        if action.startswith("./"):
            continue
        if "@" not in action or not SHA.fullmatch(action.rsplit("@", 1)[1]):
            fail(f"workflow action is not commit-pinned: {action}")
    package = workflow.split("\n  package:", 1)[1].split("\n  prepare-release:", 1)[0]
    preparation = workflow.split("\n  prepare-release:", 1)[1]
    catalog_job = workflow.split("\n  release-config:", 1)[1].split("\n  package:", 1)[0]
    if "tools/release_catalog.py matrix" not in catalog_job or "tools/release_catalog.py check" not in catalog_job:
        fail("release board matrix must come from the validated catalog")
    if "needs: release-config" not in package or "matrix: ${{ fromJSON(needs.release-config.outputs.matrix) }}" not in package:
        fail("all catalog boards must build complete release packages")
    if "make package DEVICE=${{ matrix.device }}" not in package:
        fail("release board matrix must build all advertised modes")
    if "experimental-cardputer" in workflow:
        fail("Cardputer must qualify in the complete release matrix")
    if "pattern: '*-dist'" not in preparation or "merge-multiple: true" not in preparation:
        fail("candidate must collect board artifacts for exact inventory verification")
    if "contents: write" in workflow or "gh release" in workflow or "gh pr" in workflow:
        fail("CI must build and verify artifacts without changing repository content")
    if "needs: [protocol-quality, package]" not in preparation or "tools/prepare_release.py" not in preparation:
        fail("candidate must depend on complete source/package validation")
    if '--check-tag "$GITHUB_REF_NAME" --source-revision "$GITHUB_SHA"' not in preparation:
        fail("manual tag qualification must verify source and version binding")
    check_local(ROOT)
    if "secrets.PRIVATE_LITE_READ_TOKEN || github.token" not in workflow:
        fail("workflow must support private and public Lite checkouts")
    for checkout in workflow.split("uses: actions/checkout@")[1:]:
        step = re.split(r"\n      - |\n  [a-z]", checkout, maxsplit=1)[0]
        if "persist-credentials: false" not in step:
            fail("checkout must discard credentials before building source")

    for ini_name in ("platformio.ini", "launcher/platformio.ini"):
        ini = (ROOT / ini_name).read_text(encoding="utf-8")
        if re.search(r"@\s*[~^<>=*]", ini):
            fail(f"non-exact dependency constraint in {ini_name}")

    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / "platformio.ini")
    for name, board in BOARDS.items():
        if "full" in board.modes:
            board.partitions(ROOT)
        board.partitions(ROOT, standalone=True)
        flags = config[f"env:{name}"]["build_flags"]
        service = re.findall(r"-DDEVICE_SERVICE_TASK=([01])", flags)
        if service != (["1"] if board.runtime == "dedicated" else []):
            fail(f"{name}: standard execution profile differs from release catalog")
        if "${protocol_" + board.protocol_profile + ".build_flags}" not in flags:
            fail(f"{name}: protocol profile differs from release catalog")
    if "STANDALONE_ENV ?= $(DEVICE)" not in (ROOT / "Makefile").read_text():
        fail("normal packaging must use the standard board environment")
    if "STANDALONE_ENV=" in package:
        fail("release CI must not override the standard board environment")

    if "pre:tools/check_build_identity.py" not in config["common"]["extra_scripts"]:
        fail("normal builds must check the generated product version")

    rnode = (ROOT / "vendor/rnode_firmware/Makefile").read_text(encoding="utf-8")
    check_rnode_partition_producers(ROOT)
    for spec in re.findall(r'arduino-cli lib install "([^"]+)"', rnode):
        if "@" not in spec:
            fail(f"unpinned Arduino library: {spec}")

    for board in BOARDS:
        config = (ROOT / f"src/boards/{board}/config/BoardConfig.h").read_text(
            encoding="utf-8"
        )
        if not re.search(r'^#define\s+BOARD_RELEASE_REPO\s+"ratspeak/ratspeak-handheld"$', config, re.M):
            fail(f"{board} does not use the unified release repository")

    if "TODO(reveal)" in "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in ROOT.rglob("*")
        if path.is_file()
        and not any(part in IGNORED_PARTS for part in path.parts)
        and path.suffix in {".h", ".cpp"}
    ):
        fail("unresolved TODO(reveal) remains")

    print(f"source-release contract: PASS ({len(BOARDS)} release targets, pinned dependencies/actions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
