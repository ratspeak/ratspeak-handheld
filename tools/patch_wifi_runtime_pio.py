"""Select the scan/reconnect backport only for the standalone handheld builds."""
from pathlib import Path
import importlib.util

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
spec = importlib.util.spec_from_file_location("wifi_runtime_backport", root / "tools/patch_wifi_runtime.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")).resolve()
sources = {name: framework / "libraries/WiFi/src" / name for name in patch.SOURCE_FILES}
outputs = {name: Path(env.subst("$BUILD_DIR")).resolve() / "sdk-backports" / name for name in sources}
for name in sources:
    patch.write_patched(sources[name], outputs[name])
selected = set()


def replace_wifi_runtime(build_env, node):
    if not hasattr(node, "srcnode"):
        return node
    actual = Path(node.srcnode().get_abspath()).resolve()
    name = actual.name
    if name not in sources:
        return node
    if actual != sources[name] or name in selected:
        raise ValueError(f"ambiguous WiFi runtime source: {actual}")
    selected.add(name)
    return build_env.Object(target=str(outputs[name]) + ".o", source=str(outputs[name]))


def require_selection(source, target, env):
    if selected != set(sources):
        raise ValueError("WiFi scan/reconnect sources were not selected exactly once")


env.AddBuildMiddleware(replace_wifi_runtime)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", require_selection)
