#!/usr/bin/env bash
# Rebuild prebuilt/xtensa-esp32s3/{small,micro}/libratspeak_protocol.a — offline, pinned
# toolchain, ONE artifact per table profile (a runtime-profile artifact would keep
# both LiteNode monomorphization chains reachable on every board).
#
# Toolchain: espup `esp` channel, exact compiler in tools/release_identity.json.
# -Zbuild-std=core,alloc is REQUIRED
# (no prebuilt xtensa std; rust-src is in-tree so --offline works). Profile `xtensa`
# (Cargo.toml): opt-level=s, fat LTO, codegen-units=1, panic=abort. no_std build
# (--no-default-features --features profile-<x>): abort panic handler + C-malloc global
# allocator (ffi_rt).
set -euo pipefail
cd "$(dirname "$0")"

# Development rebuilds may use dirty source, but never a different selected HEAD.
# Release qualification separately requires clean source and strict provenance.
python3 ../tools/release_identity.py check-workspace --allow-dirty
archive_inputs="$(python3 ../tools/release_identity.py archive-inputs)"

ver="$(rustc +esp --version)"
test "$ver" = "$(python3 ../tools/release_identity.py toolchain)" || {
    echo "ERROR: esp toolchain differs from release identity: $ver" >&2
    exit 1
}

source_fingerprint="$(python3 ../tools/source_fingerprint.py)"

for profile in small micro; do
    cargo +esp build -p ratspeak-handheld-protocol \
        --profile xtensa \
        --target xtensa-esp32s3-none-elf \
        --no-default-features \
        --features "profile-$profile" \
        -Zbuild-std=core,alloc \
        --locked --offline
    mkdir -p "prebuilt/xtensa-esp32s3/$profile"
    cp target/xtensa-esp32s3-none-elf/xtensa/libratspeak_protocol.a \
        "prebuilt/xtensa-esp32s3/$profile/"
done

test "$source_fingerprint" = "$(python3 ../tools/source_fingerprint.py)" || {
    echo "ERROR: source changed while rebuilding archives" >&2
    exit 1
}
test "$archive_inputs" = "$(python3 ../tools/release_identity.py archive-inputs)" || {
    echo "ERROR: selected Lite source changed while rebuilding archives" >&2
    exit 1
}

# Provenance manifest: the source commits + toolchain the committed .a were built from
# (the artifact<->source correspondence is otherwise only co-commit convention).
{
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "toolchain: $ver"
    echo "ratspeak-handheld.source-sha256: $source_fingerprint"
    echo "source-graph-sha256: $archive_inputs"
    echo "rsReticulumLite: $(git -C ../../rsReticulumLite rev-parse HEAD)$( { git -C ../../rsReticulumLite diff --quiet HEAD && test -z "$(git -C ../../rsReticulumLite ls-files --others --exclude-standard 2>/dev/null)"; } 2>/dev/null || echo ' +dirty')"
    echo "rsLXMFLite: $(git -C ../../rsLXMFLite rev-parse HEAD)$( { git -C ../../rsLXMFLite diff --quiet HEAD && test -z "$(git -C ../../rsLXMFLite ls-files --others --exclude-standard 2>/dev/null)"; } 2>/dev/null || echo ' +dirty')"
    echo "rsLXST: $(git -C ../../rsLXST rev-parse HEAD)$( { git -C ../../rsLXST diff --quiet HEAD && test -z "$(git -C ../../rsLXST ls-files --others --exclude-standard 2>/dev/null)"; } 2>/dev/null || echo ' +dirty')"
    for profile in small micro; do
        echo "$profile.sha256: $(shasum -a 256 "prebuilt/xtensa-esp32s3/$profile/libratspeak_protocol.a" | cut -d' ' -f1)"
    done
} > prebuilt/xtensa-esp32s3/PROVENANCE.txt
cat prebuilt/xtensa-esp32s3/PROVENANCE.txt
ls -l prebuilt/xtensa-esp32s3/small/libratspeak_protocol.a \
      prebuilt/xtensa-esp32s3/micro/libratspeak_protocol.a
