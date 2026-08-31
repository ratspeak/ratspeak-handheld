#!/usr/bin/env python3
"""Collect release notices; check the committed bundle without build dependencies."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
import urllib.error
import urllib.parse
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
ALL = ["tdeck", "tpager", "cardputer"]
MODES = ["standalone", "launcher", "rnode"]
LICENSE_NAME = re.compile(r"^(?:licen[cs]e|copying|copyright|notice|unlicense)(?:[._-].*)?$", re.I)
IDF_REF = "38eeba213aa695aabfd6d89aa9f5078dbe5a94c3"
ARDUINO_REFS = {
    "2.0.16": "54927338eb6ae00a67aba53b7f44747cf2dfd0d2",
    "2.0.17": "5e19e086c43d0fa5e5a596497ff8f11a0a43f6c2",
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def input_pins() -> dict:
    configs = {}
    for name in ("platformio.ini", "launcher/platformio.ini"):
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(ROOT / name)
        configs[name] = {
            section: {key: ini[section][key].strip() for key in ("platform", "lib_deps") if key in ini[section]}
            for section in ini.sections()
            if any(key in ini[section] for key in ("platform", "lib_deps"))
        }
    rnode = (ROOT / "vendor/rnode_firmware/Makefile").read_text()
    provenance = (ROOT / "protocol/prebuilt/xtensa-esp32s3/PROVENANCE.txt").read_text()
    workflow = (ROOT / ".github/workflows/build.yml").read_text()
    return {
        "platformio": configs,
        "arduino_core": re.search(r"^ARDUINO_ESP_CORE_VER\s*:=\s*(\S+)", rnode, re.M)[1],
        "arduino_libraries": sorted(set(re.findall(r'arduino-cli lib install "([^"]+)"', rnode))),
        "cargo_lock_sha256": digest((ROOT / "protocol/Cargo.lock").read_bytes()),
        "rust_toolchain": re.search(r"^toolchain: (.+)$", provenance, re.M)[1],
        "lite_commits": {name: re.search(rf"^  {variable}: ([0-9a-f]{{40}})$", workflow, re.M)[1]
                         for name, variable in (("rsReticulumLite", "RNS_LITE_COMMIT"),
                                                ("rsLXMFLite", "LXMF_LITE_COMMIT"))},
        "collector_sha256": digest(Path(__file__).read_bytes()),
    }


class Collector:
    def __init__(self, pio: Path, arduino: Path, rust: Path, arduino_data: Path):
        self.pio = pio
        self.arduino = arduino
        self.rust = rust
        self.arduino_data = arduino_data
        self.components = []

    def component(self, name, version, source, boards=ALL, modes=MODES, note=""):
        component = {"name": name, "version": version, "source": source,
                     "boards": boards, "modes": modes, "notices": []}
        if note:
            component["note"] = note
        self.components.append(component)
        return component

    @staticmethod
    def add(component, label, data, source):
        if not data:
            raise ValueError(f"empty or truncated notice: {component['name']} / {label}")
        # Keep the original bytes, including line endings and copyright wording.
        data.decode("utf-8")
        component["notices"].append({"path": label, "source": source, "data": data})

    def file(self, component, root, name, *, header=False, source=None):
        data = (root / name).read_bytes()
        if header:
            match = re.match(rb"\s*(/\*.*?\*/)", data, re.S)
            if not match:
                raise ValueError(f"missing leading notice: {name}")
            data = match[1] + b"\n"
        if source is None:
            source = component["source"]
            if "/tree/" in source:
                source = source.replace("/tree/", "/blob/") + "/" + urllib.parse.quote(name)
            elif source.startswith("https://crates.io/crates/"):
                source = source.replace("https://crates.io/crates/", "https://docs.rs/crate/") + "/source/" + name
        self.add(component, name + (" (header)" if header else ""), data, source)

    def local_notices(self, component, root, *, extras=()):
        if not root.is_dir():
            raise FileNotFoundError(f"missing dependency: {component['name']}")
        paths = sorted(p for p in root.rglob("*")
                       if p.is_file() and not any(x in p.relative_to(root).parts
                           for x in (".git", "examples", "tests", "test", "target"))
                       and (LICENSE_NAME.match(p.name) or p.name == "IPA_Font_License_Agreement_v1.0.txt"))
        for path in paths:
            self.file(component, root, path.relative_to(root).as_posix())
        for name in extras:
            self.file(component, root, name, header=True)

    def source_notices(self, component, root, *, source_prefix=None):
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root)
            if not path.is_file() or path.suffix not in (".h", ".hpp", ".c", ".cpp", ".cc"):
                continue
            if any(part in (".git", "examples", "tests", "test") for part in relative.parts):
                continue
            data = path.read_bytes()
            for i, match in enumerate(re.finditer(rb"/\*.*?\*/", data, re.S)):
                block = match[0]
                if not re.search(rb"copyright|SPDX-FileCopyrightText|public domain", block, re.I):
                    continue
                if not re.search(rb"licen[cs]e|permission|redistribution|public domain", block, re.I):
                    continue
                label = relative.as_posix() + f" (notice {i + 1})"
                base = source_prefix or component["source"].replace("/tree/", "/blob/") + "/"
                line = data[:match.start()].count(b"\n") + 1
                source = base + relative.as_posix() + f"#L{line}"
                self.add(component, label, block + b"\n", source)

    def remote(self, component, repo, ref, paths):
        for path in paths:
            url = f"https://raw.githubusercontent.com/{repo}/{ref}/{path}"
            request = urllib.request.Request(url, headers={"User-Agent": "ratspeak-license-collector"})
            with urllib.request.urlopen(request, timeout=30) as response:
                data = response.read()
            self.add(component, path, data, url)

    def mit_permission(self, component):
        path = next((ROOT / ".pio/libdeps").glob("*/M5Unified/LICENSE"))
        data = path.read_bytes()
        data = data[data.index(b"Permission is hereby granted"):]
        self.add(component, "MIT permission and disclaimer", data,
            "https://github.com/m5stack/M5Unified/blob/a6256725481f1bc366655fa48cf03b6095e30ad1/LICENSE#L5")

    def pio_library(self, name, version, repo, ref, boards, *, extras=()):
        candidates = [p for p in (ROOT / ".pio/libdeps").glob(f"*/{name}*") if p.is_dir()]
        candidates = [p for p in candidates if p.name == name or p.name.startswith(name + "@")]
        chosen = None
        for path in sorted(candidates):
            if len(ref) == 40:
                if (path / ".git").exists() and run("git", "rev-parse", "HEAD", cwd=path) == ref:
                    chosen = path
                    break
            elif (path / ".piopm").is_file():
                meta = json.loads((path / ".piopm").read_text())
                if meta.get("version") == version:
                    chosen = path
                    break
        if chosen is None:
            raise ValueError(f"missing exact PlatformIO dependency: {name}@{ref}")
        component = self.component(name, version, f"https://github.com/{repo}/tree/{ref}", boards)
        self.local_notices(component, chosen, extras=extras)
        return component, chosen

    def rust_crates(self):
        lock = tomllib.loads((ROOT / "protocol/Cargo.lock").read_text())
        metadata = json.loads(run("cargo", "metadata", "--locked", "--offline", "--format-version", "1",
                                  "--manifest-path", "protocol/Cargo.toml"))
        packages = {(p["name"], p["version"]): p for p in metadata["packages"]}
        for dep in lock["package"]:
            if "source" not in dep:
                continue
            package = packages[(dep["name"], dep["version"])]
            root = Path(package["manifest_path"]).parent
            # registry/src/<index>/<crate> -> registry/cache/<index>/<crate>.crate
            archive = root.parents[2] / "cache" / root.parent.name / (root.name + ".crate")
            if digest(archive.read_bytes()) != dep["checksum"]:
                raise ValueError(f"registry archive checksum mismatch: {root.name}")
            component = self.component(dep["name"], dep["version"],
                f"https://crates.io/crates/{dep['name']}/{dep['version']}", modes=["standalone"],
                note="Cargo.lock dependency; includes build-time and target-conditional crates.")
            component["package_sha256"] = dep["checksum"]
            component["license"] = package.get("license")
            self.local_notices(component, root)
            with tarfile.open(archive) as package_archive:
                for notice in component["notices"]:
                    original = package_archive.extractfile(root.name + "/" + notice["path"])
                    if original is None or original.read() != notice["data"]:
                        raise ValueError(f"registry notice differs from locked archive: {root.name}/{notice['path']}")

    def collect(self):
        framework = self.pio / "packages/framework-arduinoespressif32"
        if json.loads((framework / "package.json").read_text())["version"] != "3.20016.0":
            raise ValueError("PlatformIO Arduino framework version drift")
        if run("rustc", "+esp", "--version") != input_pins()["rust_toolchain"]:
            raise ValueError("esp-rs runtime version drift")
        rnode_core = self.arduino_data / "packages/esp32/hardware/esp32" / input_pins()["arduino_core"]
        if json.loads((rnode_core / "package.json").read_text())["version"] != "2.0.17":
            raise ValueError("RNode Arduino framework version drift")
        self.rust_crates()
        for name, ref in input_pins()["lite_commits"].items():
            component = self.component(name, ref, f"https://github.com/ratspeak/{name}/tree/{ref}", modes=["standalone"])
            self.file(component, ROOT.parent / name, "LICENSE")
        arduino_json, _ = self.pio_library("ArduinoJson", "7.4.3", "bblanchon/ArduinoJson", "v7.4.3", ALL)
        arduino_json["modes"] = ["standalone"]
        graphics, graphics_root = self.pio_library("LovyanGFX", "1.1.16", "lovyan03/LovyanGFX", "1.1.16", ["tdeck", "tpager"])
        self.source_notices(graphics, graphics_root)
        lvgl, lvgl_root = self.pio_library("lvgl", "8.3.11", "lvgl/lvgl", "v8.3.11", ["tdeck", "tpager"])
        lvgl["modes"] = ["standalone"]
        self.file(lvgl, lvgl_root, "src/extra/libs/qrcode/qrcodegen.c", header=True)
        for name, version, repo, ref in (
            ("M5GFX", "0.2.19", "m5stack/M5GFX", "53a7184601f3667b030ba141c58b87ce2acfaa2a"),
            ("M5Unified", "0.2.13", "m5stack/M5Unified", "a6256725481f1bc366655fa48cf03b6095e30ad1"),
            ("M5Cardputer", "1.1.1", "m5stack/M5Cardputer", "e0232804763d3e42d7e9be90e43cf6e1c9e7565b"),
            ("IRremote", "4.6.1", "Arduino-IRremote/Arduino-IRremote", "69d36199d606ab56de1f0f990f2b01f416a9cc9f"),
        ):
            component, path = self.pio_library(name, version, repo, ref, ["cardputer"])
            if name == "M5GFX":
                self.source_notices(component, path)
            if name == "M5Cardputer":
                self.file(component, path, "src/M5Cardputer.h", header=True)
                # This pinned revision has SPDX headers but no root license text.
                self.mit_permission(component)

        for name, repo, ref, paths in (
            ("Montserrat", "JulietaUla/Montserrat", "711e8ae6b4fa9d33d8482c9ec4871d906a345344", ["OFL.txt"]),
            ("Font Awesome 5.9.0", "FortAwesome/Font-Awesome", "ba907eaec40fab01d410c3023a5572b2cb46cea6", ["LICENSE.txt"]),
        ):
            component = self.component(name, ref, f"https://github.com/{repo}/tree/{ref}",
                                       ["tdeck", "tpager"], ["standalone"],
                                       "Font notices for generated LVGL fonts, including custom Montserrat glyphs.")
            self.remote(component, repo, ref, paths)
            if name == "Montserrat":
                ofl = component["notices"][0]["data"]
                ofl = ofl[ofl.index(b"SIL OPEN FONT LICENSE Version 1.1"):]
            else:
                self.add(component, "Font file attribution", b"Copyright (c) Font Awesome\nFont Awesome 5.9.0\n",
                    "https://github.com/lvgl/lvgl/blob/v8.3.11/scripts/built_in_font/FontAwesome5-Solid%2BBrands%2BRegular.woff")
                self.add(component, "SIL Open Font License 1.1", ofl,
                    "https://github.com/JulietaUla/Montserrat/blob/711e8ae6b4fa9d33d8482c9ec4871d906a345344/OFL.txt")

        rnode_repos = {
            "Adafruit BusIO": "adafruit/Adafruit_BusIO", "Adafruit GFX Library": "adafruit/Adafruit-GFX-Library",
            "Adafruit seesaw Library": "adafruit/Adafruit_Seesaw", "Adafruit SSD1306": "adafruit/Adafruit_SSD1306",
            "Adafruit SH110X": "adafruit/Adafruit_SH110X", "Adafruit ST7735 and ST7789 Library": "adafruit/Adafruit-ST7735-Library",
            "Adafruit NeoPixel": "adafruit/Adafruit_NeoPixel", "XPowersLib": "lewisxhe/XPowersLib",
            "Crypto": "OperatorFoundation/Crypto", "LovyanGFX": "lovyan03/LovyanGFX",
            "IRremote": "Arduino-IRremote/Arduino-IRremote", "LibSSH-ESP32": "ewpa/LibSSH-ESP32",
            "M5GFX": "m5stack/M5GFX", "M5Unified": "m5stack/M5Unified", "M5Cardputer": "m5stack/M5Cardputer",
        }
        installed = {}
        index = json.loads((self.arduino_data / "library_index.json").read_text())
        indexed = {(library["name"], library["version"]): library for library in index["libraries"]}
        for prop in self.arduino.glob("*/library.properties"):
            values = dict(line.split("=", 1) for line in prop.read_text().splitlines() if "=" in line and not line.startswith("#"))
            installed[(values.get("name"), values.get("version"))] = prop.parent
        for spec in input_pins()["arduino_libraries"]:
            name, version = spec.rsplit("@", 1)
            path = installed.get((name, version))
            if path is None:
                raise ValueError(f"missing exact Arduino dependency: {spec}")
            package = indexed[(name, version)]
            archive = self.arduino_data / "staging/libraries" / package["archiveFileName"]
            expected_checksum = package["checksum"].removeprefix("SHA-256:")
            if digest(archive.read_bytes()) != expected_checksum:
                raise ValueError(f"Arduino registry archive checksum mismatch: {spec}")
            component = self.component("Arduino: " + name, version,
                package["url"], modes=["rnode"],
                note="Pinned RNode build dependency; board-conditional dependencies are included conservatively.")
            component["source_repository"] = f"https://github.com/{rnode_repos[name]}"
            component["archive_sha256"] = expected_checksum
            if name in ("M5GFX", "M5Unified", "M5Cardputer", "IRremote", "LibSSH-ESP32"):
                component["boards"] = ["cardputer"]
            elif name == "LovyanGFX":
                component["boards"] = ["tdeck", "tpager"]
            self.local_notices(component, path)
            with zipfile.ZipFile(archive) as package_archive:
                for notice in component["notices"]:
                    candidates = [n for n in package_archive.namelist() if "/" in n and n.split("/", 1)[1] == notice["path"]]
                    if len(candidates) != 1 or package_archive.read(candidates[0]) != notice["data"]:
                        raise ValueError(f"Arduino notice differs from registry archive: {spec}/{notice['path']}")
            if name in ("LovyanGFX", "M5GFX"):
                self.source_notices(component, path,
                    source_prefix=f"https://github.com/{rnode_repos[name]}/blob/{version}/")
            if name == "Adafruit seesaw Library":
                self.file(component, path, "Adafruit_seesaw.h", header=True)
                self.file(component, self.arduino / "Adafruit_GFX_Library", "license.txt",
                    source="https://github.com/adafruit/Adafruit-GFX-Library/blob/1.12.5/license.txt")
            if name == "Adafruit ST7735 and ST7789 Library":
                self.file(component, path, "Adafruit_ST77xx.h", header=True)
                self.mit_permission(component)
            if name == "M5Cardputer":
                self.file(component, path, "src/M5Cardputer.h", header=True)
                self.mit_permission(component)

        for version, ref in ARDUINO_REFS.items():
            component = self.component("Arduino-ESP32", version,
                f"https://github.com/espressif/arduino-esp32/tree/{ref}",
                modes=["rnode"] if version == "2.0.17" else ["standalone", "launcher"])
            self.remote(component, "espressif/arduino-esp32", ref,
                        ["LICENSE.md", "cores/esp32/libb64/LICENSE", "libraries/BLE/LICENSE"])
            if version == "2.0.16":
                self.file(component, self.pio / "packages/framework-arduinoespressif32", "cores/esp32/Arduino.h", header=True)

        self.sdk()
        toolchain = self.pio / "packages/toolchain-xtensa-esp32s3"
        if json.loads((toolchain / "package.json").read_text())["version"] != "8.4.0+2021r2-patch5":
            raise ValueError("Xtensa GCC runtime version drift")
        gcc = self.component("Xtensa GCC and newlib runtimes", "8.4.0+2021r2-patch5",
            "https://github.com/espressif/crosstool-NG/releases/tag/esp-2021r2-patch5",
            note="Runtime libraries only; toolchain is not redistributed in firmware packages.")
        for name in ("gcc/COPYING3", "gcc/COPYING3.LIB", "gcc/COPYING.RUNTIME", "newlib/COPYING.NEWLIB", "newlib/COPYING.LIBGLOSS"):
            self.file(gcc, toolchain / "share/licenses", name)
        rust = self.component("Rust core, alloc and compiler-builtins", "esp-rs 1.95.0.0",
            "https://github.com/esp-rs/rust-build/releases/tag/v1.95.0.0", modes=["standalone"],
            note="Full upstream library notice is retained as a conservative superset of core/alloc use.")
        self.file(rust, self.rust, "share/doc/rust/COPYRIGHT-library.html")
        self.file(rust, self.rust, "lib/rustlib/src/rust/library/compiler-builtins/LICENSE.txt")
        radio = self.component("RNode radio drivers", "vendored",
            "https://github.com/ratspeak/ratspeak-handheld/tree/main/vendor/rnode_firmware", modes=["rnode"])
        self.add(radio, "sx126x, sx127x, sx128x copyright", b"Copyright Sandeep Mistry, Mark Qvist and Jacob Eva.\nLicensed under the MIT license.\n",
                 radio["source"] + "/sx126x.cpp")
        self.file(radio, ROOT / "vendor/rnode_firmware", "ST7789.h", header=True)
        # MIT permission/disclaimer text, with the radio copyright retained above.
        self.mit_permission(radio)
        for component in self.components:
            if not component["notices"]:
                raise ValueError(f"no license text found for {component['name']}")
        return sorted(self.components, key=lambda x: (x["name"].lower(), x["version"]))

    def sdk(self):
        sdk = self.component("ESP-IDF SDK", "v4.4.7 (Arduino build)",
            f"https://github.com/espressif/esp-idf/tree/{IDF_REF}",
            note="Arduino packages report v4.4.7-dirty. This identifies upstream notice provenance, not an assertion that packaged SDK binaries equal a clean ESP-IDF build.")
        self.remote(sdk, "espressif/esp-idf", IDF_REF, ["LICENSE", "components/console/argtable3/LICENSE",
            "components/console/linenoise/LICENSE", "components/freertos/LICENSE.md",
            "components/newlib/COPYING.NEWLIB", "components/nghttp/COPYING", "components/nghttp/LICENSE",
            "components/wpa_supplicant/COPYING"])
        self.file(sdk, self.pio / "packages/framework-arduinoespressif32/tools/sdk/esp32s3",
                  "include/fatfs/src/ff.h", header=True,
                  source=f"https://github.com/espressif/esp-idf/blob/{IDF_REF}/components/fatfs/src/ff.h")
        self.source_notices(sdk, self.pio / "packages/framework-arduinoespressif32/tools/sdk/esp32s3/include",
            source_prefix=f"https://github.com/espressif/arduino-esp32/blob/{ARDUINO_REFS['2.0.16']}/tools/sdk/esp32s3/include/")
        for name, repo, ref, paths in (
            ("lwIP", "espressif/esp-lwip", "a45be9e438f6cf9c54ec150581819c3b95d5af6b", ["COPYING"]),
            ("mbedTLS", "espressif/mbedtls", "2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9", ["LICENSE"]),
            ("ESP Wi-Fi binaries", "espressif/esp32-wifi-lib", "f2aae4d44ec7908013066e69d29b9948846c335c", ["LICENSE"]),
            ("ESP PHY binaries", "espressif/esp-phy-lib", "dcfdccf6cc2fc02d0886624b7998c890d1a19b28", ["LICENSE"]),
            ("ESP Bluetooth controller", "espressif/esp32c3-bt-lib", "e5c0f7256ecf5b5f8eb28c1793051a6b88f95124", ["LICENSE"]),
            ("NimBLE", "espressif/esp-nimble", "49d60705f6e085e670b6d425387e75054bf5dffe", ["LICENSE", "NOTICE"]),
            ("TinyUSB", "espressif/tinyusb", "c4badd394eda18199c0196ed0be1e2d635f0a5f6", ["LICENSE"]),
            ("SPIFFS", "pellepl/spiffs", "0dbb3f71c5f6fae3747a9d935372773762baf852", ["LICENSE"]),
            ("nghttp2", "nghttp2/nghttp2", "e2bc59bec9004bca47df961cbbad20664d7e53b2", ["COPYING"]),
            ("cJSON", "DaveGamble/cJSON", "87d8f0961a01bf09bef98ff89bae9fdec42181ee", ["LICENSE"]),
            ("libsodium", "jedisct1/libsodium", "4f5e89fa84ce1d178a6765b8b46f2b6f91216677", ["LICENSE"]),
            ("micro-ecc", "kmackay/micro-ecc", "24c60e243580c7868f4334a1ba3123481fe1aa48", ["LICENSE.txt"]),
            ("esp_littlefs 1.14.1", "joltwallet/esp_littlefs", "41873c20fb5cdbcf28d7d6cc04e4bcb4a1305317", ["LICENSE"]),
            ("littlefs", "littlefs-project/littlefs", "f53a0cc961a8acac85f868b431d2f3e58e447ba3", ["LICENSE.md"]),
            ("protobuf-c", "protobuf-c/protobuf-c", "abc67a11c6db271bedbb9f58be85d6f4e2ea8389", ["LICENSE"]),
        ):
            component = self.component("ESP-IDF: " + name, ref, f"https://github.com/{repo}/tree/{ref}",
                note="Upstream v4.4.7 submodule notice; configuration-dependent components are included conservatively.")
            self.remote(component, repo, ref, paths)


def write_bundle(components, output):
    bundle = bytearray(b"Ratspeak Handheld third-party notices\n\n"
        b"Notices for the pinned build dependencies and embedded runtime components.\n"
        b"Some entries are build-time or board-conditional; this is not a binary SBOM.\n"
        b"Each component retains its own license. See manifest.json for provenance.\n")
    seen = {}
    for component in components:
        bundle.extend(("\n" + "=" * 72 + "\n" + component["name"] + " " + component["version"] + "\n"
            + component["source"] + "\nBoards: " + ", ".join(component["boards"])
            + "\nModes: " + ", ".join(component["modes"]) + "\n").encode())
        for notice in component["notices"]:
            data = notice.pop("data")
            bundle.extend(("\n--- " + notice["path"] + " ---\n").encode())
            notice["length"] = len(data)
            notice["sha256"] = digest(data)
            if notice["sha256"] in seen:
                notice["offset"], original = seen[notice["sha256"]]
                bundle.extend(("The same license text is reproduced above under " + original + ".\n").encode())
            else:
                notice["offset"] = len(bundle)
                seen[notice["sha256"]] = (len(bundle), component["name"] + " / " + notice["path"])
                bundle.extend(data)
                bundle.extend(b"\n")
    manifest = {"schema_version": 1, "inputs": input_pins(), "bundle": "THIRD-PARTY.txt",
                "bundle_sha256": digest(bundle), "components": components}
    output.mkdir(parents=True, exist_ok=True)
    (output / "THIRD-PARTY.txt").write_bytes(bundle)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")


def check_bundle(output=ROOT / "licenses"):
    manifest = json.loads((output / "manifest.json").read_text())
    bundle = (output / "THIRD-PARTY.txt").read_bytes()
    if manifest.get("schema_version") != 1 or manifest.get("inputs") != input_pins():
        raise ValueError("license inventory does not match the pinned build inputs; regenerate and review it")
    if digest(bundle) != manifest["bundle_sha256"]:
        raise ValueError("third-party notice bundle checksum mismatch")
    if not manifest.get("components"):
        raise ValueError("empty license inventory")
    identities = [(c["name"], c["version"]) for c in manifest["components"]]
    if len(identities) != len(set(identities)):
        raise ValueError("duplicate license component")
    names = {c["name"] for c in manifest["components"]}
    required = {"Arduino-ESP32", "ArduinoJson", "LovyanGFX", "lvgl", "M5GFX", "M5Unified", "M5Cardputer",
                "IRremote", "Montserrat", "Font Awesome 5.9.0", "RNode radio drivers", "ESP-IDF SDK",
                "Rust core, alloc and compiler-builtins", "Xtensa GCC and newlib runtimes"}
    required.update("ESP-IDF: " + name for name in ("lwIP", "mbedTLS", "ESP Wi-Fi binaries", "ESP PHY binaries",
        "ESP Bluetooth controller", "NimBLE", "TinyUSB", "SPIFFS", "nghttp2", "cJSON", "libsodium", "micro-ecc",
        "esp_littlefs 1.14.1", "littlefs", "protobuf-c"))
    if not required.issubset(names):
        raise ValueError("missing non-Rust notices: " + ", ".join(sorted(required - names)))
    for spec in manifest["inputs"]["arduino_libraries"]:
        name, version = spec.rsplit("@", 1)
        if ("Arduino: " + name, version) not in identities:
            raise ValueError("missing pinned Arduino notices: " + spec)
    for name, version in manifest["inputs"]["lite_commits"].items():
        if (name, version) not in identities:
            raise ValueError("missing pinned Lite source license: " + name)
    for version in ARDUINO_REFS:
        if ("Arduino-ESP32", version) not in identities:
            raise ValueError("missing Arduino framework license: " + version)
    for component in manifest["components"]:
        if not component.get("notices"):
            raise ValueError(f"missing notices for {component['name']}")
        if not component.get("boards") or set(component["boards"]) - set(ALL):
            raise ValueError("invalid notice board scope")
        if not component.get("modes") or set(component["modes"]) - set(MODES):
            raise ValueError("invalid notice firmware mode scope")
        for notice in component["notices"]:
            start, length = notice["offset"], notice["length"]
            if start < 0 or length <= 0 or start + length > len(bundle) or digest(bundle[start:start + length]) != notice["sha256"]:
                raise ValueError(f"notice checksum mismatch: {component['name']} / {notice['path']}")
            if urllib.parse.urlsplit(notice["source"]).scheme != "https":
                raise ValueError("notice source must use HTTPS")
    lock = tomllib.loads((ROOT / "protocol/Cargo.lock").read_text())
    expected = {(p["name"], p["version"], p["checksum"]) for p in lock["package"] if "source" in p}
    actual = {(c["name"], c["version"], c["package_sha256"]) for c in manifest["components"] if "package_sha256" in c}
    if actual != expected:
        raise ValueError("license bundle is missing a locked Rust dependency")
    if re.search(rb"/(?:Users|home)/[^/\s]+/", bundle) or re.search(r"/(?:Users|home)/[^/\s]+/", json.dumps(manifest)):
        raise ValueError("machine-local path leaked into license bundle")
    return len(manifest["components"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify the checked-in bundle (default; offline)")
    mode.add_argument("--refresh", action="store_true", help="regenerate from installed exact dependencies and upstream notices")
    parser.add_argument("--output", type=Path, default=ROOT / "licenses")
    parser.add_argument("--platformio-home", type=Path, default=Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))))
    parser.add_argument("--arduino-libraries", type=Path, default=Path.home() / "Documents/Arduino/libraries")
    parser.add_argument("--arduino-data", type=Path, default=Path.home() / ("Library/Arduino15" if sys.platform == "darwin" else ".arduino15"))
    parser.add_argument("--rust-toolchain", type=Path, default=Path.home() / ".rustup/toolchains/esp")
    args = parser.parse_args()
    if args.refresh:
        write_bundle(Collector(args.platformio_home, args.arduino_libraries, args.rust_toolchain, args.arduino_data).collect(), args.output)
    count = check_bundle(args.output)
    print(f"license bundle: PASS ({count} components; pinned inputs and notice checksums verified)")


if __name__ == "__main__":
    main()
