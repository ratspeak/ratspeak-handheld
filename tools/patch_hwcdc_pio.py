"""Select the reviewed native USB CDC repair without editing the installed SDK."""
from pathlib import Path
import importlib.util

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
spec = importlib.util.spec_from_file_location("hwcdc_backport", root / "tools/patch_hwcdc.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")).resolve()
source = framework / "cores/esp32/HWCDC.cpp"
output = Path(env.subst("$BUILD_DIR")).resolve() / "sdk-backports/HWCDC.cpp"
patch.write_patched(source, output)
selected = False


def replace_hwcdc(build_env, node):
    global selected
    # Earlier middleware may already have replaced another SDK source with a
    # SCons NodeList (WiFi's build-local backport). Leave that selection intact.
    if not hasattr(node, "srcnode"):
        return node
    actual = Path(node.srcnode().get_abspath()).resolve()
    if actual.name != source.name:
        return node
    if actual != source or selected:
        raise ValueError("selected SDK HWCDC.cpp must be compiled exactly once")
    selected = True
    return build_env.Object(target=str(output.with_name("HWCDCBackport.cpp.o")), source=str(output))


def require_selection(source, target, env):
    if not selected:
        raise ValueError("HWCDC short-write repair was not selected")


def verify_provider(source, target, env):
    defines = {item[0]: str(item[1]) for item in env.get("CPPDEFINES", [])
               if isinstance(item, (list, tuple)) and len(item) == 2}
    # UART boards compile an empty HWCDC translation unit. Require the repaired
    # provider only when native USB CDC is actually selected by the compiler.
    if defines.get("ARDUINO_USB_CDC_ON_BOOT") == "1" and defines.get("ARDUINO_USB_MODE") == "1":
        patch.verify_map(Path(env.subst("$BUILD_DIR")) / "firmware.map")


env.AddBuildMiddleware(replace_hwcdc)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", require_selection)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_provider)
