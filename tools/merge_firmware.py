"""Post-build script: merge bootloader + partitions + boot_app0 + firmware
into a single .bin for M5Burner and one-step flashing."""

Import("env")

import os
import shlex


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    # boot_app0.bin lives in the Arduino framework tools
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    # Preserve the established per-device image names and flash settings.
    pioenv = env["PIOENV"]
    tpager = pioenv.startswith("tpager")
    cardputer = pioenv.startswith("cardputer")
    if tpager:
        output_name = "rspager-standalone-factory.bin"
    elif cardputer:
        output_name = "rscardputer-standalone-factory.bin"
    else:
        output_name = "rsdeck-merged.bin"
    flash_freq = "--flash-freq 80m " if tpager else ""
    flash_size = "8MB" if cardputer else "16MB"
    output = os.path.join(build_dir, output_name)

    python = env.subst("$PYTHONEXE")
    result = env.Execute(
        f"{shlex.quote(python)} -m esptool --chip esp32s3 merge-bin "
        f"--flash-mode dio {flash_freq}--flash-size {flash_size} "
        f"-o {shlex.quote(output)} "
        f"0x0000 {shlex.quote(os.path.join(build_dir, 'bootloader.bin'))} "
        f"0x8000 {shlex.quote(os.path.join(build_dir, 'partitions.bin'))} "
        f"0xe000 {shlex.quote(boot_app0)} "
        f"0x10000 {shlex.quote(os.path.join(build_dir, 'firmware.bin'))}"
    )
    if result:
        return result
    print(f"\n** Merged firmware written to: {output}")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
