<div align="center">

# Ratspeak Handheld

**Standalone Client for Reticulum & Ratspeak**

[Downloads](https://github.com/ratspeak/ratspeak-handheld/releases) |
[Docs](https://docs.ratspeak.org/) |
[Ratspeak](https://github.com/ratspeak/Ratspeak)

[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

</div>

Ratspeak Handheld combines [rsDeck](https://github.com/ratspeak/rsDeck),
[rsPager](https://github.com/ratspeak/rsPager), and
[rsCardputer](https://github.com/ratspeak/rsCardputer) in one repo.
The Reticulum and LXMF core is now written in Rust, replacing
our half-baked microReticulum fork. Over time, this repo will evolve to support more handheld devices, but is intentionally slim during the first beta rollout.

## Devices

| Device | Release |
| --- |  --- |
| LilyGo T-Deck Plus | Beta |
| LilyGo T-Pager | Beta |
| M5Stack Cardputer Adv* | In Testing |

*Cardputer Adv requires the Cap LoRa-1262 for LoRa connectivity.

## Modes

The firmware supports two different modes, available at each startup:

- **Standalone** — all-in-one encrypted LXMF messaging over LoRa or Wi-Fi.
- **RNode** — radio for Ratspeak, Sideband, or another Reticulum client.



## Install

For a fresh installation, use your device's `*-full.zip` package from
[Releases](https://github.com/ratspeak/ratspeak-handheld/releases) or a local
build. Open the [Ratspeak web flasher](https://ratspeak.org/download.html#dl-custom),
choose **Flash** under **Build your own**, and upload the `.zip`.

Full packages include the launcher and both modes; they are not data-preserving
updates. **Back up your identity and data before flashing**; see the
[backup and installation guide](https://docs.ratspeak.org/docs/hardware/flashing-firmware#before-flashing)
if your device already has firmware installed.

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
the launcher and both modes, checks image sizes, and writes the files to `dist/`.

Normal builds use the included Rust libraries; a Rust toolchain is not needed.
Shared firmware lives in `src/core/`, board support in `src/boards/`, user
interfaces in `src/ui/`, and the Rust protocol bridge in `protocol/`.
Run `make check DEVICE=tdeck` to check a build, or `make check-all` for all boards.

For Rust development, keep `rsReticulumLite`, `rsLXMFLite`, `rsReticulum`, and
`rsLXMF` beside this checkout, using the revisions in the
[build workflow](.github/workflows/build.yml). Run `make protocol-check` for host
and cross-target checks. To rebuild the included libraries, install esp-rs
1.95.0.0 and run `bash protocol/build-xtensa.sh`; its locked dependencies must
already be cached. The script records source revisions and hashes in
[`PROVENANCE.txt`](protocol/prebuilt/xtensa-esp32s3/PROVENANCE.txt).

## License

The standalone firmware, launcher, and build tools are
[AGPL-3.0-or-later](LICENSE). Bundled RNode firmware retains its GPLv3 license.
See [Third-party notices](THIRD_PARTY_NOTICES.md).
