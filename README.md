<div align="center">

# Ratspeak Handheld

**Standalone Client for Reticulum & Ratspeak**

[Downloads](https://github.com/ratspeak/ratspeak-handheld/releases) |
[Docs](https://docs.ratspeak.org/) |
[Ratspeak](https://github.com/ratspeak/Ratspeak)

[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

</div>

Ratspeak Handheld is shared firmware for Reticulum/LXMF messaging on the
T-Deck Plus, T-Pager and Cardputer Adv. Its Rust core uses rsReticulumLite and
rsLXMFLite, with shared C++ storage, transport and device services. T-Deck and
T-Pager share the LVGL interface; Cardputer uses a compact Canvas interface.

Version **2.2.1** improves conversations, saved-message reads and delivery recovery. See the
[release notes](https://github.com/ratspeak/ratspeak-handheld/releases/tag/v2.2.1)
for changes and download the package for your device and preferred mode.

## Devices

| Device | Display and controls | Radio |
| --- | --- | --- |
| LilyGO T-Deck Plus | 320×240, keyboard, trackball and touch | Integrated LoRa |
| LilyGO T-Pager (SX1262) | 480×222, keyboard and scroll encoder | Integrated LoRa |
| M5Stack Cardputer Adv | 240×135, keyboard | **Cap LoRa-1262 required for LoRa** |
| Elecrow ThinkNode M9 (development beta) | 320×240, keyboard and D-pad | Integrated LR1110 |

Cardputer support remains beta. An SD card is optional for normal messaging on
the supported boards. Use hardware and an antenna suited to your operating band.

M9 is currently a local **Standalone beta**. RNode and Full packages are coming
soon; the published 2.2.1 downloads cover the three devices above it. The M9's
arrow keys, OK/Enter and Back control the shared LVGL interface. Its additional
icon shortcuts are reserved for a later update.

## Modes

The released T-Deck, T-Pager and Cardputer firmware supports two modes, available at each startup:

- **Standalone** — all-in-one encrypted LXMF messaging over LoRa or Wi-Fi.
- **RNode** — radio for Ratspeak, Sideband, or another Reticulum client.

The launcher starts your last selected mode after seven seconds. Using the
controls stops the countdown; select a mode and confirm with Enter or a click.
On T-Deck, tap the other card to select it; tap the selected card to start. If a mode cannot
start, the launcher shows an error and lets you try again.

## Controls and messaging

On T-Deck, use the trackball or touch. On T-Pager, turn the encoder to move,
click or press Enter to select, and use Backspace to return to tab navigation.
Under **Settings → LoRa**, unlock **Developer Radio Controls** as prompted.
In **Frequency**, **A/D** selects a digit, the encoder tunes it, **Enter** saves
and **Alt+Backspace** cancels. No touch or horizontal encoder is required.
Cardputer uses **Fn+arrows**, **Enter**, **Backspace** to go back outside text entry,
and **Ctrl+H** for help. In text fields, Backspace deletes text; a fresh press on
an empty field goes back. **Fn+Backspace** is forward Delete.
In Cardputer Messages, move down past the last conversation on a page to select
the paging arrows, then use left/right and Enter. **Fn+Left/Right** changes pages
directly; add **Shift** to jump to the first or last page.

A send first saves the message; `sent` means transmission started, while
`delivered` requires a verified delivery proof. A separate `save retry` or
`storage error` can appear even after delivery. See the
[handheld guide](https://docs.ratspeak.org/docs/hardware/handheld-guide) for
controls, history, status labels and recovery.

For M9 development, build with `python3 -m platformio run -e m9`. The resulting
`.pio/build/m9/m9-standalone-factory.bin` is a complete flash image for offset
`0x0`; `.pio/build/m9/firmware.bin` is the application for offset `0x10000`.
Use the USB UART bridge at 115200 baud. Native USB is disabled because its pins
are used by this board's peripherals. RTC/compass features and battery calibration
are deferred; GPS and Wi-Fi provide time through the existing shared services.

## Install

For a fresh installation, use your device's `*-full.zip` package from
[Releases](https://github.com/ratspeak/ratspeak-handheld/releases) or a local
build. Open the [Ratspeak web flasher](https://ratspeak.org/download.html#dl-custom),
choose **Flash** under **Build your own**, and upload the `.zip`. Check the
selected board, source repository and version; automatic download presets may
point to an earlier release.

Full packages include the launcher and both modes; they are not data-preserving
updates. **Back up your identity and data before flashing**; see the
[backup and installation guide](https://docs.ratspeak.org/docs/hardware/flashing-firmware#before-flashing)
if your device already has firmware installed.

Files use the device names `tdeck`, `pager`, and `cardputer`: for example,
`pager-full.zip`, `pager-standalone.zip`, and `pager-rnode.zip`. Standalone and
RNode also have raw application files, `pager-standalone.bin` and `pager-rnode.bin`.
ZIPs install complete firmware layouts; the corresponding raw BIN contains only
that application.

Raw application BINs preserve data only when written to the matching application
slot without erasing flash or changing the partition table. The correct slot
depends on the installed layout; a raw BIN is not a replacement for a full
installation package. Back up first when changing layouts or firmware modes.

On the T-Pager, the buttons are **Reset**, **Boot**, and **Power**, left to right
with the screen facing you. Reset restarts the device; in Standalone, tap Boot
to sleep or wake the screen, or hold it for about a second for the power-off prompt
(Enter confirms). If the screen is dark, tap and release Boot before holding
again; hold Power for about a second to turn the device on.

## Build from source

On Linux or macOS, install Git, Make, Python 3.12, and Arduino CLI 1.4.1, then:

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

Use `DEVICE=tpager` or `DEVICE=cardputer` for the other boards. Packaging builds
the launcher and both modes, validates their identities and installation layout,
and writes the files to `dist/`.

RNode mode automatically refreshes its stored image hash to allow custom builds
and upgrades. That bookkeeping does not authenticate firmware. Check downloaded
packages against the release checksums and use a source you trust.

Normal builds use the included Rust libraries; a Rust toolchain is not needed.
Shared firmware lives in `src/core/`, board support in `src/boards/`, user
interfaces in `src/ui/`, and the Rust protocol bridge in `protocol/`.
Run `make check DEVICE=tdeck` to check a build, or `make check-all` for all boards.

For Rust development, keep `rsReticulumLite`, `rsLXMFLite`, `rsReticulum`, and
`rsLXMF` beside this checkout, using the selected revisions in
[`tools/release_identity.json`](tools/release_identity.json). Run `make protocol-check` for host
and cross-target checks. To rebuild the included libraries, install esp-rs
1.95.0.0 and run `bash protocol/build-xtensa.sh`; its locked dependencies must
already be cached. The script records source revisions and hashes in
[`PROVENANCE.txt`](protocol/prebuilt/xtensa-esp32s3/PROVENANCE.txt).

### T-Deck USB control

Current source builds let you inspect and operate Standalone over USB on macOS
or Linux. Close other serial monitors, then use the T-Deck's serial port:

```sh
python3 tools/handheld-control.py --port /dev/cu.usbmodemXXXX view
python3 tools/handheld-control.py --port /dev/cu.usbmodemXXXX key down
python3 tools/handheld-control.py --port /dev/cu.usbmodemXXXX hold
python3 tools/handheld-control.py --port /dev/cu.usbmodemXXXX text 'Bench message'
```

`view` reads visible text and focus without waking the screen; use its `next`
cursor with `view --offset N` for more text. Password fields are redacted.
Navigation uses the normal keyboard handlers. `hold` performs the trackball's
long-press action, including blanking the screen when no screen action applies.
The first key or hold on a sleeping screen only wakes it; `text` accepts
printable ASCII and never presses Enter.
Use `key enter` to confirm, or `char h --ctrl` for Ctrl+H. If a response is lost,
inspect with `view` before repeating input. The published 2.2.0 images predate
these controls.

## License

The standalone firmware, launcher, and build tools are
[AGPL-3.0-or-later](LICENSE). Bundled RNode firmware retains its GPLv3 license.
See [Third-party notices](THIRD_PARTY_NOTICES.md).
