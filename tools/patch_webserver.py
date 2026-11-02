#!/usr/bin/env python3
"""Bound multipart parsing in the selected RNode Arduino 2.0.17 WebServer.

CVE-2026-42854 / Espressif GHSA-8cmm-3887-r32j: RFC 2046 limits a MIME
boundary to 70 characters. Reject invalid boundaries before reading a form,
then use fixed storage for the delimiter. The optional static-file console
can enter this parser even without an application upload handler.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess

from patch_wifi_sockets import prepare_arduino, selected_platform, sha, verify_database

SOURCE_SHA256 = "122de5397729899ac8600d545f7ed4b8a02298351a4f1b0fa5c7fa73f87a14d0"
SOURCE_FILES = ("Parsing.cpp",)
PATCHES = (
    (b'bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){\n',
     b'bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){\n'
     b'  // RFC 2046 / CVE-2026-42854: validate before any form allocation or read.\n'
     b'  if (boundary.length() == 0 || boundary.length() > 70) return false;\n'),
    (b'char fastBoundary[ fastBoundaryLen ];', b'char fastBoundary[4 + 70 + 1];'),
)


def patched_source(source: Path) -> bytes:
    data = source.read_bytes()
    if source.name != "Parsing.cpp" or sha(data) != SOURCE_SHA256:
        raise ValueError(f"unreviewed selected SDK WebServer parser: {source}")
    for before, after in PATCHES:
        if data.count(before) != 1:
            raise ValueError("WebServer backport must select exactly one reviewed boundary")
        data = data.replace(before, after)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-cli", default="arduino-cli", help="Arduino CLI executable used to resolve the selected core (default: arduino-cli)")
    parser.add_argument("--fqbn", help="fully qualified board name; required for preparation")
    parser.add_argument("--sketch", type=Path, help="sketch path; required for preparation")
    parser.add_argument("--build-path", type=Path, help="Arduino build directory; required for preparation")
    parser.add_argument("--output", type=Path, required=True,
                        help="generated library override outside the installed core; preparation replaces an existing managed directory")
    parser.add_argument("--verify-database", type=Path,
                        help="check compile_commands.json against --output without preparing a library; otherwise resolve and copy the selected Arduino 2.0.17 library")
    args = parser.parse_args()
    if args.verify_database:
        verify_database(args.verify_database, args.output, SOURCE_FILES)
        print("WebServer multipart boundary: patched Arduino parser selected exactly once")
        return
    if not all((args.fqbn, args.sketch, args.build_path)):
        parser.error("preparation requires --fqbn, --sketch and --build-path")
    result = subprocess.run(
        [args.arduino_cli, "compile", "--fqbn", args.fqbn, "--build-path",
         str(args.build_path), "--show-properties=expanded", str(args.sketch)],
        capture_output=True, text=True, timeout=120, check=True)
    platform = selected_platform(result.stdout)
    prepare_arduino(platform, args.output, library_name="WebServer",
                    source_files=SOURCE_FILES, patcher=patched_source,
                    patch_name="webserver-multipart-boundary-v1")
    print(f"WebServer multipart boundary: selected Arduino {platform}; override {args.output}")


if __name__ == "__main__":
    main()
