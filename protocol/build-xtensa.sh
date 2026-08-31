#!/usr/bin/env bash
# Rebuild prebuilt/xtensa-esp32s3/{small,micro}/libratspeak_protocol.a — offline, pinned
# toolchain, ONE artifact per table profile (a runtime-profile artifact would keep
# both LiteNode monomorphization chains reachable on every board).
#
# Toolchain pin: espup `esp` channel, esp-rs release 1.95.0.0 (rustc 1.95.0-nightly
# 95e5bda86, LLVM 21). -Zbuild-std=core,alloc is REQUIRED
# (no prebuilt xtensa std; rust-src is in-tree so --offline works). Profile `xtensa`
# (Cargo.toml): opt-level=s, fat LTO, codegen-units=1, panic=abort. no_std build
# (--no-default-features --features profile-<x>): abort panic handler + C-malloc global
# allocator (ffi_rt).
set -euo pipefail
cd "$(dirname "$0")"

ver="$(rustc +esp --version)"
case "$ver" in
    *"1.95.0-nightly"*"(1.95.0.0)"*) ;;
    *) echo "ERROR: esp toolchain drifted: $ver (pin: esp-rs 1.95.0.0)" >&2; exit 1 ;;
esac

source_fingerprint="$(python3 ../tools/source_fingerprint.py)"

for profile in small micro; do
    cargo +esp build -p ratspeak-handheld-protocol \
        --profile xtensa \
        --target xtensa-esp32s3-none-elf \
        --no-default-features \
        --features "profile-$profile" \
        -Zbuild-std=core,alloc \
        --offline
    mkdir -p "prebuilt/xtensa-esp32s3/$profile"
    cp target/xtensa-esp32s3-none-elf/xtensa/libratspeak_protocol.a \
        "prebuilt/xtensa-esp32s3/$profile/"
done

test "$source_fingerprint" = "$(python3 ../tools/source_fingerprint.py)" || {
    echo "ERROR: source changed while rebuilding archives" >&2
    exit 1
}

# Provenance manifest: the source commits + toolchain the committed .a were built from
# (the artifact<->source correspondence is otherwise only co-commit convention).
{
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "toolchain: $ver"
    echo "ratspeak-handheld.source-sha256: $source_fingerprint"
    echo "rsReticulumLite: $(git -C ../../rsReticulumLite rev-parse HEAD)$( { git -C ../../rsReticulumLite diff --quiet HEAD && test -z "$(git -C ../../rsReticulumLite ls-files --others --exclude-standard 2>/dev/null)"; } 2>/dev/null || echo ' +dirty')"
    echo "rsLXMFLite: $(git -C ../../rsLXMFLite rev-parse HEAD)$( { git -C ../../rsLXMFLite diff --quiet HEAD && test -z "$(git -C ../../rsLXMFLite ls-files --others --exclude-standard 2>/dev/null)"; } 2>/dev/null || echo ' +dirty')"
    for profile in small micro; do
        echo "$profile.sha256: $(shasum -a 256 "prebuilt/xtensa-esp32s3/$profile/libratspeak_protocol.a" | cut -d' ' -f1)"
    done
} > prebuilt/xtensa-esp32s3/PROVENANCE.txt
cat prebuilt/xtensa-esp32s3/PROVENANCE.txt
ls -l prebuilt/xtensa-esp32s3/small/libratspeak_protocol.a \
      prebuilt/xtensa-esp32s3/micro/libratspeak_protocol.a
