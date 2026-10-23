#!/usr/bin/env python3
"""Exercise the real LVGL keyboard on the host, without ESP32 hardware."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import os
import subprocess

root = Path(__file__).resolve().parents[1]
lvgl = root / ".pio/libdeps/crowpanel/lvgl"
if not (lvgl / "lvgl.h").exists():
    raise SystemExit("Install crowpanel dependencies first (platformio pkg install -e crowpanel)")
build = root / "build/crowpanel-keyboard-test"
build.mkdir(parents=True, exist_ok=True)
(build / "Arduino.h").write_text('''#pragma once
#ifdef __cplusplus
extern "C" {
#endif
unsigned long millis(void);
#ifdef __cplusplus
}
#endif
''')
flags = ["-O1", "-g", "-DRSCROWPANEL=1", "-DLV_CONF_INCLUDE_SIMPLE", "-DDISPLAY_WIDTH=800", "-DDISPLAY_HEIGHT=480"]
for include in (build, root / "src/ui/lvgl", root / "src/boards/crowpanel", root / "src/core", lvgl):
    flags += ["-I", str(include)]
sources = sorted((lvgl / "src").rglob("*.c"))
sources += [root / "src/ui/lvgl/fonts/lv_font_rsdeck_14.c", root / "src/ui/lvgl/Theme.cpp",
            root / "src/ui/lvgl/TouchKeyboard.cpp", root / "tests/crowpanel/keyboard.cpp"]
sources += sorted((root / "src/ui/lvgl/fonts").glob("lv_font_crowpanel_*.c"))
log = build / "compile.log"


def compile_source(item):
    i, source = item
    output = build / f"{i}.o"
    cxx = source.suffix == ".cpp"
    cmd = ["g++" if cxx else "gcc", "-std=c++17" if cxx else "-std=c99", *flags, "-c", str(source), "-o", str(output)]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return output, result


with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
    results = list(pool.map(compile_source, enumerate(sources)))
log.write_text("".join(result.stdout for _, result in results))
if any(result.returncode for _, result in results):
    raise SystemExit(f"Compilation failed; see {log}")
exe = build / "keyboard-test"
subprocess.run(["g++", *[str(obj) for obj, _ in results], "-lm", "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
