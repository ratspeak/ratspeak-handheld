"""Post-build script: merge bootloader + partitions + boot_app0 + firmware
into a single .bin for M5Burner and one-step flashing."""

Import("env")

import os
import shlex
import sys
from pathlib import Path

# PlatformIO executes extra scripts without adding their directory to sys.path.
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "tools"))
from release_catalog import board_for_environment
from check_sdk_backports import verify_sdk_map


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    verify_sdk_map(Path(build_dir) / "firmware.map")

    # boot_app0.bin lives in the Arduino framework tools
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    _, board = board_for_environment(env["PIOENV"])
    flash_freq = f"--flash-freq {board.standalone_flash_freq} " if board.standalone_flash_freq else ""
    slot = board.partitions(Path(env.subst("$PROJECT_DIR")), standalone=True)["app0"]
    firmware = Path(build_dir) / "firmware.bin"
    if not 0 < firmware.stat().st_size <= slot.size:
        raise ValueError(f"{env['PIOENV']}: application is empty or exceeds the standalone slot ({slot.size} bytes)")
    output = os.path.join(build_dir, board.post_build_image)

    python = env.subst("$PYTHONEXE")
    positional = (
        f"0x0000 {shlex.quote(os.path.join(build_dir, 'bootloader.bin'))} "
        f"0x8000 {shlex.quote(os.path.join(build_dir, 'partitions.bin'))} "
        f"0xe000 {shlex.quote(boot_app0)} "
        f"{hex(slot.offset)} {shlex.quote(str(firmware))}"
    )
    # esptool's merge subcommand/flag spelling has shifted release to release --
    # merge_bin/--flash_mode/--output (older, underscore, long --output only) vs
    # merge-bin/--flash-mode/-o (newer, hyphenated, short -o too). PlatformIO's own
    # tool-esptoolpy resolution has drifted under the same declared version range
    # before (confirmed on 2026-10-19), so this project can't assume which style a
    # fresh install actually has -- try the older spelling first (seen working on
    # every version encountered so far), fall back to the newer one.
    legacy = (
        f"--chip esp32s3 merge_bin "
        f"--flash_mode dio {flash_freq.replace('--flash-freq', '--flash_freq')}--flash_size {board.flash_size} "
        f"--output {shlex.quote(output)} {positional}"
    )
    modern = (
        f"--chip esp32s3 merge-bin "
        f"--flash-mode dio {flash_freq}--flash-size {board.flash_size} "
        f"-o {shlex.quote(output)} {positional}"
    )
    result = env.Execute(f"{shlex.quote(python)} -m esptool {legacy}")
    if result:
        result = env.Execute(f"{shlex.quote(python)} -m esptool {modern}")
    if result:
        return result
    print(f"\n** Merged firmware written to: {output}")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
