#!/usr/bin/env python3
"""Guard the LVGL/service boundary; runtime assertions cover indirect callers."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(pattern, text, path, message):
    match = re.search(pattern, text)
    if match:
        line = text.count("\n", 0, match.start()) + 1
        raise SystemExit(f"runtime boundary: {path}:{line}: {message}")


for path in (ROOT / "src/ui/lvgl").rglob("*"):
    if path.suffix not in {".cpp", ".h"}:
        continue
    text = path.read_text()
    check(r"\b(?:ProtocolBackend|MessageStore|AnnounceManager|IdentityManager|FlashStore|SDStore|TCPClientInterface|LoRaInterface|SX1262)\s*\*",
          text, path, "UI must use service views, not owner pointers")
    check(r"\b(?:WiFi|LittleFS|SD)\s*\.\s*\w+\s*\(|\bPreferences\s+\w+|rs_handheld_\w+\s*\(",
          text, path, "UI must not call networking/storage/FFI directly")

for board in ("tdeck", "tpager"):
    path = ROOT / f"src/boards/{board}/main.cpp"
    text = path.read_text()
    ui = text[text.index("void loop() {"):]
    check(r"\b(?:userConfig|backend|protocolRuntime|messageStore|announceManager|identityMgr|sdStore|flash|radio|autoIface|tcpClients|wifiImpl|gps)\s*(?:\.|->)",
          ui, path, "post-startup UI loop reached a service-owned object")
    service = text[text.index("static void serviceNetworkPoll() {"):text.index("void loop() {")]
    check(r"\blv_\w+\s*\(|\b(?:ui|powerMgr|audio|inputManager)\s*\.",
          service, path, "service poll reached a UI-owned object")
    if "radio.setYieldCallback([]() { yield(); });" not in text:
        raise SystemExit(f"runtime boundary: {board} radio yield contract missing")
    display = (ROOT / f"src/boards/{board}/hal/Display.cpp").read_text()
    if "SharedSPILock bus;" not in display or "handheld::displayFlushed(millis());" not in display:
        raise SystemExit(f"runtime boundary: {board} display arbitration/measurement missing")

print("runtime boundary: PASS (LVGL views, owner-only board loops, shared display SPI)")
