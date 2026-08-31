#!/usr/bin/env python3
"""Validate the public source/release contract for the unified firmware."""

from __future__ import annotations

import configparser
import re
from pathlib import Path


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
    for device in ("tdeck", "tpager", "cardputer"):
        if device not in workflow:
            fail(f"workflow does not cover {device}")
    package = workflow.split("\n  package:", 1)[1].split("\n  experimental-cardputer:", 1)[0]
    preparation = workflow.split("\n  prepare-release:", 1)[1]
    if "device: [tdeck, tpager]" not in package or "cardputer" in package:
        fail("release package matrix must contain only T-Deck and T-Pager")
    if "name: tdeck-dist" not in preparation or "name: tpager-dist" not in preparation or "pattern:" in preparation:
        fail("release downloads must explicitly allowlist T-Deck and T-Pager")
    if "experimental-cardputer" in re.search(r"needs:.*", preparation).group(0):
        fail("experimental Cardputer must not gate release-board publication")
    if "contents: write" in workflow or "gh release" in workflow or "gh pr" in workflow:
        fail("CI must build and verify artifacts without changing repository content")
    if "needs: [protocol-quality, package]" not in preparation or "tools/prepare_release.py" not in preparation:
        fail("candidate must depend on complete source/package validation")
    if '--check-tag "$GITHUB_REF_NAME" --source-revision "$GITHUB_SHA"' not in preparation:
        fail("manual tag qualification must verify source and version binding")
    for variable, expected in (
        ("RNS_LITE_COMMIT", "25842ef187e2e29819c73121b90e57f5e367b419"),
        ("LXMF_LITE_COMMIT", "f4c16b87b595f4f484fa33fe3984cbafbdce714b"),
        ("RNS_FULL_COMMIT", "3b3eb29c16c9dbc114c9b31d9f912a7b1a68652c"),
        ("LXMF_FULL_COMMIT", "e210e0c244c76532faae99696f83c94d44c27dc6"),
    ):
        if f"{variable}: {expected}" not in workflow:
            fail(f"workflow does not use the reviewed {variable} pin")
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
    for board in ("tdeck", "tpager"):
        flags = config[f"env:{board}"]["build_flags"]
        if re.findall(r"-DDEVICE_SERVICE_TASK=([01])", flags) != ["1"]:
            fail(f"{board}: normal builds must enable the dedicated service task")
    if "STANDALONE_ENV ?= $(DEVICE)" not in (ROOT / "Makefile").read_text():
        fail("normal packaging must use the standard board environment")
    if "STANDALONE_ENV=" in package:
        fail("release CI must not override the standard board environment")

    rnode = (ROOT / "vendor/rnode_firmware/Makefile").read_text(encoding="utf-8")
    for spec in re.findall(r'arduino-cli lib install "([^"]+)"', rnode):
        if "@" not in spec:
            fail(f"unpinned Arduino library: {spec}")

    for board in ("tdeck", "tpager", "cardputer"):
        config = (ROOT / f"src/boards/{board}/config/BoardConfig.h").read_text(
            encoding="utf-8"
        )
        if '#define RSDECK_VERSION_STRING "2.1.0"' not in config:
            fail(f"{board} is not on unified firmware version 2.1.0")
        if '#define BOARD_RELEASE_REPO    "ratspeak/ratspeak-handheld"' not in config:
            fail(f"{board} does not use the unified release repository")

    if "TODO(reveal)" in "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in ROOT.rglob("*")
        if path.is_file()
        and not any(part in IGNORED_PARTS for part in path.parts)
        and path.suffix in {".h", ".cpp"}
    ):
        fail("unresolved TODO(reveal) remains")

    print("source-release contract: PASS (3 build targets, 2 release targets, pinned dependencies/actions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
