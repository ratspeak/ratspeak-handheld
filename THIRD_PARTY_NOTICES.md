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
`ratspeak-handheld-notices.zip`, `release-manifest.json`, and `SHA256SUMS`.
Keep the notices with redistributed binaries. The source archive and manifest
identify the source revisions and build inputs needed to rebuild them.

Maintainers can verify the inventory offline with
`python3 tools/collect_licenses.py --check`. After a dependency change, regenerate
it with `--refresh` using the installed build dependencies and review the result.
