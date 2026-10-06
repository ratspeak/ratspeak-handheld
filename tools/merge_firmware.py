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


def merge_command():
    """esptool 5 renamed merge_bin and its flags to dashes; PlatformIO's
    bundled esptool is still 4.x, which only knows the underscore spelling."""
    try:
        import esptool
        major = int(esptool.__version__.split(".")[0])
    except (ImportError, AttributeError, ValueError):
        major = 5
    if major >= 5:
        return "merge-bin", "--flash-mode", "--flash-freq", "--flash-size"
    return "merge_bin", "--flash_mode", "--flash_freq", "--flash_size"


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    verify_sdk_map(Path(build_dir) / "firmware.map")

    # boot_app0.bin lives in the Arduino framework tools
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    command, mode_opt, freq_opt, size_opt = merge_command()
    _, board = board_for_environment(env["PIOENV"])
    flash_freq = f"{freq_opt} {board.standalone_flash_freq} " if board.standalone_flash_freq else ""
    slot = board.partitions(Path(env.subst("$PROJECT_DIR")), standalone=True)["app0"]
    firmware = Path(build_dir) / "firmware.bin"
    if not 0 < firmware.stat().st_size <= slot.size:
        raise ValueError(f"{env['PIOENV']}: application is empty or exceeds the standalone slot ({slot.size} bytes)")
    output = os.path.join(build_dir, board.post_build_image)

    python = env.subst("$PYTHONEXE")
    result = env.Execute(
        f"{shlex.quote(python)} -m esptool --chip esp32s3 {command} "
        f"{mode_opt} dio {flash_freq}{size_opt} {board.flash_size} "
        f"-o {shlex.quote(output)} "
        f"0x0000 {shlex.quote(os.path.join(build_dir, 'bootloader.bin'))} "
        f"0x8000 {shlex.quote(os.path.join(build_dir, 'partitions.bin'))} "
        f"0xe000 {shlex.quote(boot_app0)} "
        f"{hex(slot.offset)} {shlex.quote(str(firmware))}"
    )
    if result:
        return result
    print(f"\n** Merged firmware written to: {output}")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
