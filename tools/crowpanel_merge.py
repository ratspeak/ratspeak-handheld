"""Package the exact boot images selected by the Arduino 3 builder."""
from pathlib import Path
import subprocess

Import("env")


def merge(source, target, env):
    build = Path(env.subst("$BUILD_DIR"))
    app = build / "firmware.bin"
    if not 0 < app.stat().st_size <= 0x400000:
        raise RuntimeError("CrowPanel application exceeds its 4 MiB OTA slot")
    images = [(int(str(offset), 0), Path(env.subst(str(path))))
              for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
    if not any(offset == 0 for offset, _ in images):
        raise RuntimeError("No bootloader selected by the SDK builder")
    sdk = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"))
    expected_boot = (sdk / "esp32s3/bin/bootloader_qio_120m.elf").resolve()
    selected_boot = [Path(node.get_abspath()).resolve()
                     for node in env.File(str(build / "bootloader.bin")).sources]
    if expected_boot not in selected_boot:
        raise RuntimeError("The build did not select Elecrow bootloader_qio_120m.elf")
    images.append((0x10000, app))
    command = [env.subst("$PYTHONEXE"), env.subst("$UPLOADER"), "--chip", "esp32s3",
               "merge_bin", "--flash_mode", "dio", "--flash_freq", "80m",
               "--flash_size", "16MB", "-o", str(build / "crowpanel-standalone-factory.bin")]
    for offset, path in sorted(images):
        command += [hex(offset), str(path)]
    subprocess.run(command, check=True)

env.AddPostAction("$BUILD_DIR/firmware.bin", merge)
