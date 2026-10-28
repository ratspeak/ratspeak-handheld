# Third-party notices

The standalone firmware, launcher, and build tools use the
[GNU Affero General Public License v3.0 or later](LICENSE). Third-party code and
assets retain their own licenses.

RNode-mode firmware under `vendor/rnode_firmware/` is derived from RNode
Firmware by Mark Qvist and contributors, with handheld-specific changes. It uses
the [GNU General Public License v3.0 or later](vendor/rnode_firmware/LICENSE).
The included Semtech radio drivers retain the MIT notices of Sandeep Mistry,
Mark Qvist, and Jacob Eva; other file-level notices remain in place.

The [notice bundle](licenses/THIRD-PARTY.txt) contains the license texts and
copyright notices for the pinned Rust dependencies, Arduino/ESP-IDF components,
graphics libraries, fonts, and compiler runtimes. The
[inventory](licenses/manifest.json) records their versions, sources, board/mode
scope, and notice checksums. It includes build-time and conditional dependencies;
it is not a claim that every listed component appears in every image.

In particular, the generated `lv_font_rsdeck_*` fonts are derived from Montserrat
by the Montserrat Project Authors, and LVGL's built-in symbol fonts use Font
Awesome. Their font licenses are included in the bundle. Graphics notices also
retain attribution to Adafruit, Bodmer, lovyan03, and M5Stack.

Redistributions must retain the applicable notices and provide the corresponding
source required by each license, including changes and build scripts. The Lite
protocol sources identified in
[`PROVENANCE.txt`](protocol/prebuilt/xtensa-esp32s3/PROVENANCE.txt) are part of the
standalone firmware's source; the prebuilt Rust archives are not a substitute.

Release downloads include `ratspeak-handheld-source.tar.gz`,
`ratspeak-handheld-notices.zip`, and `SHA256SUMS`. Keep the notices with
redistributed binaries. The source archive's `SOURCE.json` identifies the source
revisions needed to rebuild them.

Maintainers can verify the inventory offline with
`python3 tools/collect_licenses.py --check`. After a dependency change, regenerate
it with `--refresh` using the installed build dependencies and review the result.

## ESP-IDF compatibility fixes

Sources under `vendor/esp_idf_compat/` provide narrow backports for the pinned
ESP-IDF 4.4.7 ESP32-S3 SDK. Paths in this section are relative to that source
directory unless they begin with `src/` or `tools/`.
They retain Espressif's public interfaces and original copyright notices.
`SdkVersion.h` rejects a different SDK version or chip so that an upgrade
requires reviewing and removing backports supplied by the new SDK.

### DHCP option bounds

`dhcpserver.c` is Espressif's Apache-2.0 implementation from
[`38eeba213aa695aabfd6d89aa9f5078dbe5a94c3`](https://github.com/espressif/esp-idf/blob/38eeba213aa695aabfd6d89aa9f5078dbe5a94c3/components/lwip/apps/dhcpserver/dhcpserver.c),
with the option bounds checks from
[`8b4b5d53`](https://github.com/espressif/esp-idf/commit/8b4b5d53)
backported for the pinned ESP32-S3 SDK. The original source SHA-256 is
`a7ff751cf191de080f60bf592de26a3b8768785c774e4ce9bd1b824b4cb7c8c4`.

The installed Arduino 2.0.16 and 2.0.17 SDKs contain the same affected DHCP
archive member. The advisory is
[CVE-2026-45160](https://github.com/espressif/esp-idf/security/advisories/GHSA-g764-gwc3-75m5).
Standalone firmware and RNode both use this file. Its public symbols replace
the SDK's `dhcpserver.c.obj` during linking; no second DHCP service is started.

PAD and END options are handled separately. Other options require a length
byte and a complete payload before field access. Message type and requested
address retain the upstream minimum lengths. The backport uses remaining-byte
subtraction for its bounds comparison and rejects nonpositive input lengths
before pointer arithmetic. All other DHCP behavior and public interfaces are
retained.

### Deep-sleep modem power sequence

`rtc_init.c` and `rtc_sleep.c` come from the same source revision, under
`components/esp_hw_support/port/esp32s3/`. Their original SHA-256 values are:

| File | Original SHA-256 |
| --- | --- |
| `rtc_init.c` | `9f13e0aaa8ed5219c8123da0f539feee0b64e3ca13d29c160012c2d549cb8abb` |
| `rtc_sleep.c` | `04f4902afba374ce799b7f0b73196dd28fb84efc78d6a933c8eaffacd06c382e` |

The ESP32-S3 changes from Espressif's
[`5017ec08`](https://github.com/espressif/esp-idf/commit/5017ec08)
release the retained Wi-Fi power-down and isolation overrides before
startup accesses modem registers, and preserve isolation during the sleep
power transition. These are the three register-operation changes addressing
AR2024-004, a rare Wi-Fi initialization watchdog failure after deep sleep.

Standalone firmware, RNode, and the launcher all compile these application-side
functions. The pinned SDK's bootloaders do not contain the affected functions.
The selected definitions replace the two SDK archive members without changing
the bootloader or the firmware's sleep policy.

### TLS Finished errors

`mbedtls/UPSTREAM.json` identifies the exact Espressif Mbed TLS 2.28.7 sources
and local changes. The Deck/Pager application compiles `ssl_tls.c` through
`src/core/compat/Tls.c`. Its private Finished callback returns status so failed
hash/PRF calculations cannot continue through Finished processing. The matching
private header stays local to this translation unit; public SDK layouts and
symbols remain unchanged. Version/configuration guards require re-review when
the SDK changes. Original notices are retained in all three source inputs.

The application release checker separately uses the SDK HTTPS client with its
built-in certificate bundle. It checks release information and does not install
firmware.

### Optional Cardputer web console

`tools/patch_webserver.py` generates the Cardputer RNode WebServer override from
the checksum-verified Arduino source. It rejects empty or over-70-byte multipart
boundaries before parsing and uses a fixed buffer. The existing generated-library
selection checks ensure that RNode compiles the selected implementation once.

### Build validation

`tools/check_sdk_backports.py` checks the application linker map for exactly
one selected provider of the required DHCP and RTC entry points and rejects
the replaced SDK archive members. Launcher builds require only the RTC
correction because they do not start a DHCP service. PlatformIO post-actions
and the RNode build targets run these checks before accepting their outputs.
When TLS entry points are linked, the same checker requires the corrected
Finished providers and rejects live code from the original `ssl_tls.c.obj`
archive member. Discarded zero-address sections do not supply firmware code.

Native DHCP parser tests and SDK compilation validate the software changes.
Device wake and access-point checks remain part of hardware release testing.
