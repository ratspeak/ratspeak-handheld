"""Fail closed if the selected SDK is not Elecrow's octal 120 MHz build."""
from pathlib import Path
import json

Import("env")
board = env.BoardConfig()
if board.get("build.arduino.memory_type") != "qio_opi":
    raise RuntimeError("CrowPanel requires qio_opi memory")
if str(board.get("build.f_boot")).rstrip("L") != "120000000":
    raise RuntimeError("CrowPanel requires the 120 MHz bootloader")
platform = env.PioPlatform()
framework = Path(platform.get_package_dir("framework-arduinoespressif32"))
sdk = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))
if json.loads((framework / "package.json").read_text())["version"] != "3.0.2":
    raise RuntimeError("CrowPanel requires Arduino ESP32 3.0.2 with Elecrow ESP32S3_120M")
config = (sdk / "esp32s3/qio_opi/include/sdkconfig.h").read_text()
for define in ("CONFIG_SPIRAM_MODE_OCT 1", "CONFIG_SPIRAM_SPEED_120M 1", "CONFIG_SPIRAM_SPEED 120"):
    if f"#define {define}" not in config:
        raise RuntimeError(f"Wrong CrowPanel SDK: missing {define}")
boot = sdk / "esp32s3/bin/bootloader_qio_120m.elf"
if not boot.is_file():
    raise RuntimeError(f"Missing Elecrow bootloader: {boot}")
print(f"CrowPanel SDK: {sdk}; QIO flash / OPI PSRAM 120 MHz; bootloader: {boot.name}")

# Preserve the application's scan/cancellation contract on the new SDK.
import sys
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "tools"))
from patch_crowpanel_wifi import SOURCE_FILES, patched_source
sources = {name: (framework / "libraries/WiFi/src" / name).resolve() for name in SOURCE_FILES}
outputs = {name: Path(env.subst("$BUILD_DIR")).resolve() / "sdk-backports" / name for name in sources}
for name, source in sources.items():
    output = outputs[name]
    output.parent.mkdir(parents=True, exist_ok=True)
    data = patched_source(source)
    if not output.exists() or output.read_text() != data:
        output.write_text(data)
selected = set()


def select_wifi(build_env, node):
    if not hasattr(node, "srcnode"):
        return node
    source = Path(node.srcnode().get_abspath()).resolve()
    if source.name not in sources:
        return node
    if source != sources[source.name] or source.name in selected:
        raise ValueError(f"Ambiguous WiFi source: {source}")
    selected.add(source.name)
    return build_env.Object(target=str(outputs[source.name]) + ".o", source=str(outputs[source.name]))


def check_selection(source, target, env):
    if selected != set(sources):
        raise ValueError("CrowPanel WiFi repair sources were not selected exactly once")


env.AddBuildMiddleware(select_wifi)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", check_selection)
