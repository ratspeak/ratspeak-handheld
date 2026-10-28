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

Download the `.bin` for your device from [Releases](https://github.com/ratspeak/ratspeak-handheld/releases):

- **Full** — Standalone and RNode, with a launcher to choose between them at startup.
- **Standalone** — the on-device messenger, using LoRa or WiFi.
- **RNode** — use the handheld as a radio for Ratspeak, Sideband, or another
  Reticulum client over USB or BLE.

Files are named by device and mode, such as `tdeck-full.bin`,
`pager-standalone.bin` and `cardputer-rnode.bin`. Each is a complete image.
T-Deck, T-Pager and Cardputer also have a `-standalone-launcher.bin` for
bmorcelli's Launcher.

- **Web flasher:** open the [Ratspeak web flasher](https://ratspeak.org/download.html),
  select your device, and choose **Flash in browser**. To install a downloaded
  `.bin`, choose **Flash** under **Build your own** and upload it. Compatible
  updates keep identity, messages and settings.
- **esptool:** write the `.bin` at 0x0, for example
  `python3 -m esptool --chip esp32s3 --port PORT --baud 115200 write-flash 0x0 m9-standalone.bin`.
- **bmorcelli's Launcher:** install the `-standalone-launcher.bin` from the SD
  card or Launcher's web interface. For RNode, install the `-rnode.bin`.

Factory installs with the web flasher or esptool replace the firmware layout, so
back up your identity and messages first. See the
[installation guide](https://docs.ratspeak.org/docs/hardware/flashing-firmware)
for backups, updates and Launcher storage, and the [handheld guide](https://docs.ratspeak.org/docs/hardware/handheld-guide)
for controls and setup.

## Message history

Internal storage keeps the newest messages. When it runs low, the oldest
messages are removed across all conversations; messages still being sent and
drafts are kept. To keep the full history, use an SD card for messages: turn on
**Settings → Storage → SD Message Store**; Cardputer uses an inserted card
automatically. **Settings → Storage** shows internal usage and whether history
is complete on the card or rolling.

## Voice messages

T-Deck, T-Pager, and Cardputer Adv can record and play voice messages up to
15 seconds long. Open **Voice message** in a conversation, record your clip,
then preview and send it. Sending returns to the conversation with inline
playback and delivery status. ThinkNode M9 has no supported audio hardware.

On Pager, move the wheel down from the composer to reach Voice and Send, then
click to select. Cardputer also opens the recorder with **Ctrl+V**.

Unsent clips are removed when you close the conversation. **Settings → Voice**
controls playback volume and **Max voice messages** (1–50, default 2) for
internal storage. Oldest audio is removed first; the message stays in chat.
SD-card audio is limited by available storage instead of the clip count.

Announce from the handheld before recording to it in the Ratspeak app so the
app can choose a compatible compact recording. Existing Opus clips cannot be
played on these handhelds. Live handheld calls and Hub/channel access are not
available in this release.

### Standalone radio defaults

New or reset radio settings use **Medium Fast**: bandwidth **250 kHz**
(`250000` Hz), spreading factor **9**, coding rate **4/5** (`5` in RNode
configuration), TX power **17 dBm**, and preamble **18 symbols**. Existing saved
settings are retained.

The region selection sets the center frequency separately from the link preset:

| Region | Default center frequency | Frequency in Hz |
|---|---|---|
| Americas | 915.000 MHz | `915000000` |
| Europe (EU868) | **868.000 MHz** | `868000000` |
| Australia | 915.000 MHz | `915000000` |
| Asia | 923.000 MHz | `923000000` |

For example, an RNode communicating with a handheld on **Europe + Medium Fast**
needs `frequency = 868000000`, `bandwidth = 250000`, `spreadingfactor = 9`,
and `codingrate = 5`. Match any custom settings saved on the handheld instead
of assuming the defaults. Choosing a region again restores its default frequency;
changing the link preset does not change frequency. TX power need not match.
In **RNode mode**, the connected host client controls the radio settings.

Follow your local radio regulations for frequency, transmit power and duty
cycle. Region and link presets do not limit power or airtime for your region.

## Propagation

In **Settings → Propagation**, turn propagation ON to use an LXMF propagation
node for store-and-forward messaging. It is OFF by default.

- **Node mode AUTO** selects a usable discovered node, preferring WiFi/TCP when
  available. The device keeps up to five candidates for each connection family.
- **Node mode MANUAL** offers address entry or a discovered-node list. The chosen
  address stays pinned while offline and survives AUTO, OFF and restarts. Use
  **Choose / replace node** or **Clear manual node** to change it.
- **Delivery AUTO** tries direct delivery first and falls back after a network
  failure. **ALWAYS** sends through the selected node on every connection. If
  that node is unreachable, the message reports **PROP UNAVAILABLE**.
- **Sync now** retrieves your inbox. Automatic checks run about every five
  minutes on WiFi/TCP or thirty minutes on LoRa, with a small random delay.

**PROPAGATED** means the relay accepted the transfer; it does not mean the
recipient has received the message. Messages created with ALWAYS retain that
policy even if settings change. Turning propagation OFF pauses pending relay
work. Downloaded messages are saved locally before the node is asked to delete
them.

Handheld memory and work limits apply. Stamp costs above 20 are refused, and a
stamp job stops after 30 seconds. An inbox reply that exceeds the supported
Resource size or uses compression reports **REPLY UNSUPPORTED** and leaves
messages on the node.

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
