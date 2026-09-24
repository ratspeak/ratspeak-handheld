<div align="center">

# [Ratspeak Handheld](https://ratspeak.org/)

**Standalone messaging and RNode firmware for Reticulum handhelds.**

[![Status](https://img.shields.io/badge/status-beta-yellow.svg)](#devices)
[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

[Ratspeak](https://github.com/ratspeak/Ratspeak) |
[Docs](https://docs.ratspeak.org/docs/hardware/handheld-guide) |
[Downloads](https://github.com/ratspeak/ratspeak-handheld/releases)

</div>

---

Send encrypted LXMF messages over LoRa or WiFi, directly from your handheld.
No account or phone required. Keep contacts, discover peers, and carry your
conversations with you. LoRa messaging works without an internet connection.

This is the shared home for our handheld firmware, bringing rsDeck, rsPager
and rsCardputer into one codebase. The protocol core uses rsReticulumLite and
rsLXMFLite, with device support built around the same messaging services.

## Devices

| Device | Support |
| --- | --- |
| LilyGO T-Deck Plus | Standalone and RNode |
| LilyGO T-Pager (SX1262) | Standalone and RNode |
| M5Stack Cardputer Adv | Standalone and RNode; Cap LoRa-1262 required for LoRa |
| Elecrow ThinkNode M9 | Standalone; RNode coming soon |

An SD card is optional for normal messaging. Downloads are available for all
four devices.

This is beta firmware. If something isn't working, open an issue with your
device, firmware version, and the steps to reproduce it.

## Install

Download the ZIP for your device from [Releases](https://github.com/ratspeak/ratspeak-handheld/releases):

- **Full** — Standalone and RNode, with a launcher to choose between them at startup.
- **Standalone** — the on-device messenger, using LoRa or WiFi.
- **RNode** — use the handheld as a radio for Ratspeak, Sideband, or another
  Reticulum client over USB or BLE.

Files are named by device: `tdeck-full.zip`, `pager-standalone.zip`,
`cardputer-rnode.zip`, and `m9-standalone.zip`. For T-Deck, T-Pager and Cardputer, open the
[Ratspeak web flasher](https://ratspeak.org/download.html#dl-custom), choose
**Flash** under **Build your own**, and upload the ZIP.

For M9, extract `m9-standalone.zip` and flash its enclosed factory image with
esptool: `python3 -m esptool --chip esp32s3 --port PORT --baud 115200 write-flash 0x0 m9-standalone.bin`.
Replace `PORT` with your device's serial port. M9 web flashing is coming later.

ZIPs install a complete firmware layout. Back up your identity and messages
before flashing. The matching `.bin` files contain only the application and
must be written to the correct slot for your installed layout. See the
[installation guide](https://docs.ratspeak.org/docs/hardware/flashing-firmware)
for backups and updates, and the [handheld guide](https://docs.ratspeak.org/docs/hardware/handheld-guide)
for controls and setup.

## Build From Source

On Linux or macOS, install Git, Make, Python 3.12 and Arduino CLI 1.4.1, then:

```bash
git clone https://github.com/ratspeak/ratspeak-handheld
cd ratspeak-handheld
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements-build.txt
make setup DEVICE=tdeck
make doctor DEVICE=tdeck
make package DEVICE=tdeck
```

Use `DEVICE=tpager`, `DEVICE=cardputer` or `DEVICE=m9` for the other devices. Packages
are written to `dist/`. Normal builds use the included Rust libraries; a Rust
toolchain is not required.

M9 builds Standalone only and does not require Arduino CLI.

## License

GNU Affero General Public License v3.0 or later. See [LICENSE](LICENSE).
Bundled RNode firmware retains its [GPLv3 license](vendor/rnode_firmware/LICENSE).
See [Third-party notices](THIRD_PARTY_NOTICES.md).
