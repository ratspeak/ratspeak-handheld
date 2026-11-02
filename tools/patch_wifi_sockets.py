#!/usr/bin/env python3
"""Build-local Arduino 2.0.16/2.0.17 WiFi socket FD-cleanup backport.

Both reviewed cores ship the identical source below. Keep its LGPL notice and
normal behavior while containing descriptor-option and owner-allocation failure.
Never modify an installed framework or select an arbitrary installed version.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE_SHA256 = "87f2786b1d7c4617eb4de7e0ada8da1da7c684c35da4ace39b3d051bfaf7494a"
PATCH_FROM = b'errno, strerror(errno)); return 0; }}'
PATCH_TO = b'errno, strerror(errno)); close(sockfd); return 0; }}'
MANIFEST = "ratspeak-wifi-backport.json"
SOURCE_FILES = {"WiFiClient.cpp": (SOURCE_SHA256, PATCH_FROM, PATCH_TO),
                "WiFiServer.cpp": ('d2778de30e02c0f4f7a2d7d880197d0b5662fecdc540979b535cd21e57139efa', b'    }\n  }\n  return WiFiClient();\n}\n\nvoid WiFiServer::begin', b'    }\n    lwip_close(client_sock);\n  }\n  return WiFiClient();\n}\n\nvoid WiFiServer::begin')}

# Keep the existing two independent shared_ptr control blocks and object layout.
# A shared_ptr constructor deletes its raw pointer if control allocation throws;
# relinquish the stack FD guard only once that raw handle exists.
OWNER_HELPER = b'''#include <new>

namespace {
class WiFiClientPendingFd {
    int value;
public:
    explicit WiFiClientPendingFd(int fd) noexcept : value(fd) {}
    ~WiFiClientPendingFd() { if (value >= 0) close(value); }
    WiFiClientPendingFd(const WiFiClientPendingFd&) = delete;
    WiFiClientPendingFd& operator=(const WiFiClientPendingFd&) = delete;
    void release() noexcept { value = -1; }
};

bool prepareWiFiClientOwners(int fd,
        std::shared_ptr<WiFiClientSocketHandle>& socketOwner,
        std::shared_ptr<WiFiClientRxBuffer>& rxOwner) noexcept
{
    WiFiClientPendingFd pending(fd);
    try {
        auto* rawHandle = new WiFiClientSocketHandle(fd);
        pending.release();
        std::shared_ptr<WiFiClientSocketHandle> stagedSocket(rawHandle);
        std::shared_ptr<WiFiClientRxBuffer> stagedRx(new WiFiClientRxBuffer(fd));
        socketOwner.swap(stagedSocket);
        rxOwner.swap(stagedRx);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
} // namespace

'''
ALLOCATION_PATCHES = [
    (b'WiFiClient::WiFiClient():', OWNER_HELPER + b'WiFiClient::WiFiClient():'),
    (b'''WiFiClient::WiFiClient(int fd):_connected(true),_timeout(WIFI_CLIENT_DEF_CONN_TIMEOUT_MS),next(NULL)
{
    clientSocketHandle.reset(new WiFiClientSocketHandle(fd));
    _rxBuffer.reset(new WiFiClientRxBuffer(fd));
}''', b'''WiFiClient::WiFiClient(int fd):_connected(false),_timeout(WIFI_CLIENT_DEF_CONN_TIMEOUT_MS),next(NULL)
{
    _connected = prepareWiFiClientOwners(fd, clientSocketHandle, _rxBuffer);
}'''),
    (b'''    clientSocketHandle.reset(new WiFiClientSocketHandle(sockfd));
    _rxBuffer.reset(new WiFiClientRxBuffer(sockfd));''', b'''    if (!prepareWiFiClientOwners(sockfd, clientSocketHandle, _rxBuffer)) {
        return 0;
    }'''),
]



def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def patched_source(source: Path) -> bytes:
    original = source.read_bytes()
    if source.name not in SOURCE_FILES:
        raise ValueError(f"unexpected WiFi SDK source: {source}")
    expected, before, after = SOURCE_FILES[source.name]
    if sha(original) != expected:
        raise ValueError(f"unreviewed selected SDK WiFi socket source: {source}")
    if original.count(before) != 1:
        raise ValueError("WiFi socket cleanup patch must select exactly one error macro")
    patched = original.replace(before, after)
    if source.name == "WiFiClient.cpp":
        for before, after in ALLOCATION_PATCHES:
            if patched.count(before) != 1:
                raise ValueError("WiFi owner patch must select exactly one reviewed boundary")
            patched = patched.replace(before, after)
    return patched


def write_patched(source: Path, output: Path) -> dict:
    source, output = source.resolve(), output.resolve()
    if output == source or output.is_relative_to(source.parent):
        raise ValueError("patched output must be outside the selected SDK source directory")
    data = patched_source(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != data:
        output.write_bytes(data)
    result = {"patch": "wifi-socket-owner-failure-v2",
              "source": str(source), "source_sha256": SOURCE_FILES[source.name][0],
              "output_sha256": sha(data)}
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def selected_platform(properties: str) -> Path:
    values = [line.split("=", 1)[1] for line in properties.splitlines()
              if line.startswith("runtime.platform.path=")]
    if len(values) != 1 or not values[0].strip():
        raise ValueError("Arduino must report exactly one selected runtime.platform.path")
    platform = Path(values[0]).resolve()
    if not (platform / "platform.txt").is_file():
        raise ValueError(f"selected Arduino platform is unavailable: {platform}")
    return platform


def prepare_arduino(platform: Path, output: Path, *, library_name: str = "WiFi",
                    source_files=None, patcher=None,
                    patch_name: str = "wifi-socket-owner-failure-v2") -> dict:
    # The same staged, checksum-guarded library selection is also used by the
    # optional RNode console's WebServer backport. WiFi remains the default.
    source_files = SOURCE_FILES if source_files is None else source_files
    patcher = patched_source if patcher is None else patcher
    library = (platform / "libraries" / library_name).resolve()
    output = output.resolve()
    if output == library or output.is_relative_to(platform.resolve()) or platform.resolve().is_relative_to(output):
        raise ValueError("library override must be outside the selected installed platform")
    # Check both before touching a previous generated copy.
    patched = {name: patcher(library / "src" / name) for name in source_files}
    version = [line.split("=", 1)[1] for line in (platform / "platform.txt").read_text().splitlines()
               if line.startswith("version=")]
    if version != ["2.0.17"]:
        raise ValueError(f"RNode requires the selected Arduino core 2.0.17, got {version}")
    if output.exists() and not (output / MANIFEST).is_file():
        raise ValueError(f"refusing to replace an unmanaged SDK library directory: {output}")
    entries = {str(p.relative_to(library)): sha(p.read_bytes())
               for p in sorted(library.rglob("*")) if p.is_file()}
    result = {"patch": patch_name, "library": library_name, "platform": str(platform),
              "version": version[0], "inputs": entries, "patched_sha256": {name: sha(data) for name, data in patched.items()}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sdk-library-backport-", dir=output.parent) as temporary:
        staged = Path(temporary) / library_name
        shutil.copytree(library, staged)
        for name, data in patched.items():
            (staged / "src" / name).write_bytes(data)
        (staged / MANIFEST).write_text(json.dumps(result, indent=2) + "\n")
        if output.exists():
            shutil.rmtree(output)
        os.replace(staged, output)
    return result


def verify_database(database: Path, library: Path, source_files=None) -> None:
    manifest = json.loads((library / MANIFEST).read_text())
    rows = json.loads(database.read_text())
    for name in (SOURCE_FILES if source_files is None else source_files):
        expected = (library / "src" / name).resolve()
        if sha(expected.read_bytes()) != manifest["patched_sha256"][name]:
            raise ValueError(f"generated {name} no longer matches its patch manifest")
        selected = [row for row in rows if Path(row["file"]).name == name]
        if len(selected) != 1:
            raise ValueError(f"compile database must select exactly one {name} source")
        row = selected[0]
        if (Path(row["directory"]) / row["file"]).resolve() != expected:
            raise ValueError(f"compile database selected an unpatched {name} source")


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
        verify_database(args.verify_database, args.output)
        print("WiFi socket FD cleanup: both patched Arduino sources selected exactly once")
        return
    if not all((args.fqbn, args.sketch, args.build_path)):
        parser.error("preparation requires --fqbn, --sketch and --build-path")
    command = [args.arduino_cli, "compile", "--fqbn", args.fqbn,
               "--build-path", str(args.build_path), "--show-properties=expanded", str(args.sketch)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=True)
    platform = selected_platform(result.stdout)
    prepare_arduino(platform, args.output)
    print(f"WiFi socket FD cleanup: selected Arduino {platform}; override {args.output}")


if __name__ == "__main__":
    main()
