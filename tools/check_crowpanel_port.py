#!/usr/bin/env python3
"""Verify the packaged CrowPanel image against its actual SDK and protocol inputs."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
from release_identity import firmware_version, source_identity
from release_images import verify_factory
from patch_crowpanel_wifi import SOURCE_FILES, patched_source

root = Path(__file__).resolve().parents[1]
build = root / ".pio/build/crowpanel"
packages = root / ".platformio-core/packages"
sdk = root.parent / "CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480/ESP32S3_120M"
framework = packages / "framework-arduinoespressif32"
if json.loads((framework / "package.json").read_text())["version"] != "3.0.2":
    raise SystemExit("Expected Arduino 3.0.2 in the isolated .platformio-core")

# Verify the immutable protocol archive without requiring absent sibling sources.
provenance = (root / "protocol/prebuilt/xtensa-esp32s3/PROVENANCE.txt").read_text()
archive = root / "protocol/prebuilt/xtensa-esp32s3/small/libratspeak_protocol.a"
expected = next(line.split(": ", 1)[1] for line in provenance.splitlines() if line.startswith("small.sha256:"))
assert hashlib.sha256(archive.read_bytes()).hexdigest() == expected, "Protocol archive differs from provenance"

# Every selected SDK repair must come from the reviewed 3.0.2 source, not an old
# build-local output or an accidental selection of the unpatched Arduino files.
for name in SOURCE_FILES:
    assert (build / "sdk-backports" / name).read_text() == patched_source(framework / "libraries/WiFi/src" / name), name
nm = packages / "toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm"
symbols = subprocess.check_output([str(nm), "--defined-only", str(build / "firmware.elf")], text=True)
names = [line.split()[-1] for line in symbols.splitlines() if line.split()]
for name in ("handheld_wifi_scan_busy", "handheld_wifi_scan_cancel", "handheld_wifi_scan_error"):
    assert names.count(name) == 1, f"Missing/duplicate scan provider: {name}"

# Recreate the boot image from the supplied 120 MHz ELF, independently of the
# SCons selection check. The 80 MHz ROM header is intentional (Arduino qio120).
with tempfile.TemporaryDirectory(prefix="crowpanel-boot-") as temp:
    boot = Path(temp) / "boot.bin"
    subprocess.run([str(root / ".venv/bin/python"), str(packages / "tool-esptoolpy/esptool.py"),
                    "--chip", "esp32s3", "elf2image", "--flash_mode", "dio", "--flash_freq", "80m",
                    "--flash_size", "16MB", "-o", str(boot),
                    str(sdk / "esp32s3/bin/bootloader_qio_120m.elf")],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert boot.read_bytes() == (build / "bootloader.bin").read_bytes(), "Wrong bootloader"

image = (root / "dist/crowpanel-standalone.bin").read_bytes()
boot = (build / "bootloader.bin").read_bytes()
assert image[:len(boot)] == boot, "Packaged bootloader differs from verified bootloader"
assert image == (build / "crowpanel-standalone-factory.bin").read_bytes(), "Stale package"
revision, dirty = source_identity(root)
verify_factory(image, "crowpanel", "standalone", firmware_version(root, "crowpanel"), revision, dirty, root)
print("CrowPanel package: 120 MHz bootloader, SDK WiFi providers, Rust archive, image integrity and partitions PASS")
