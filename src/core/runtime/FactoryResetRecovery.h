#pragma once

#include "FactoryReset.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include <type_traits>

namespace handheld {

// Startup-only recovery: display and keyboard are ready, but no config,
// identity, protocol, or worker owner has been admitted. All boards use this
// same explicit confirmation and reset sequence. The framebuffer is owned by
// this loop until restart; normal LVGL/Canvas/network schedulers never run.
//
// Touch is optional (Touch defaults to void, touch defaults to nullptr) —
// keyboard-only boards (T-Pager, Cardputer) are unaffected. It exists
// because a keyboard-less board (first case: sensecap_d1l) has otherwise no
// way to ever leave this screen: it's the one place in the boot sequence
// that runs before LVGL/UIManager/any touch-aware UI exists, so without
// this the device deadlocks here permanently whenever resetState() isn't
// Clear (confirmed reachable on real hardware: a non-blank data partition
// left over from prior firmware fails the auto-format path on first boot).
// A single tap can only ever *advance* a state, never erase directly — it
// takes two taps (review, then confirm) to reach the same destructive
// action the keyboard path reaches via R then Enter, so an accidental tap
// can't wipe anything.
template<class Keyboard, class Display, class Restart, class Touch = void>
void factoryResetRecovery(FlashStore& flash, SDStore& sd, Keyboard& keyboard,
                          Display& display, Restart restart, Touch* touch = nullptr) {
    if (flash.resetState() == FlashStore::ResetState::Clear) return;
    constexpr bool hasTouch = !std::is_void<Touch>::value;
    bool confirm = false, paint = true, scopeKnown = false, includesSD = false;
    bool firstPaint = true, touchWasDown = false;
    const char* detail = "Reset interrupted or unavailable";
    const auto line = [&](int y, const char* text) {
        display.setCursor(10, y); display.print(text);
    };
    for (;;) {
        if (paint) {
            scopeKnown = flash.resetScope(includesSD);
            display.fillScreen(0x0000);
            display.setTextWrap(false);
            display.setTextColor(0xffff, 0x0000);
            display.setTextFont(1); display.setTextSize(2);
            line(10, "Reset recovery");
            display.setTextSize(1);
            line(37, detail);
            if (scopeKnown) line(53, includesSD ? "Scope: device + " SD_PATH_ROOT " SD" : "Scope: device only");
            else line(53, "Reset scope is not readable.");
            if (confirm) {
                line(72, scopeKnown ? "Permanently erase this scope?" :
                    (sd.isReady() ? "Erase device + mounted SD data?" : "Erase device data? (SD absent)"));
                if constexpr (hasTouch) line(90, "ENTER / tap top: erase   B / tap bottom: back");
                else line(90, "ENTER: erase   B: back");
            } else {
                line(72, includesSD && scopeKnown && !sd.isReady() ? "Insert the SD card to retry." : "Old data will not be restored.");
                if constexpr (hasTouch) line(90, "R / tap top: review reset");
                else line(90, "R: review reset");
            }
            if constexpr (hasTouch) line(112, "X / tap bottom: restart (reset stays pending)");
            else line(112, "X: restart (reset stays pending)");
            // Only the very first paint can race a truly stale event queued
            // before this screen ever appeared (e.g. a leftover boot-time
            // keypress). Later repaints are themselves caused by the user's
            // own key press inside this loop (r/b), so discarding there would
            // also eat a fast, deliberate follow-up press — e.g. a quick
            // r-then-Enter — before it's ever read.
            if (firstPaint) { keyboard.discardPending(); firstPaint = false; }
            paint = false;
        }
        keyboard.update();
        if (keyboard.hasEvent()) {
            const auto event = keyboard.getEvent();
            if (!event.ctrl && !event.alt && !event.fn && !event.repeat) {
                if (event.character == 'x' || event.character == 'X') {
                    keyboard.discardPending(); restart();
                } else if (event.character == 27 || event.character == 'b' || event.character == 'B') {
                    confirm = false; paint = true;
                }
                else if (event.character == 'r' || event.character == 'R') { confirm = true; paint = true; }
                else if (confirm && event.enter) {
                    keyboard.discardPending();
                    display.fillScreen(0x0000); line(53, "Erasing. Keep power connected.");
                    const auto result = performFactoryReset(flash, sd, !scopeKnown);
                    detail = result.detail(); confirm = false; paint = true;
                    Serial.printf("[RESET] %s\n", detail);
                    if (result.ok()) { display.fillScreen(0x0000); line(53, detail); restart(); }
                }
            }
        }
        if constexpr (hasTouch) {
            if (touch) {
                touch->update();
                const bool down = touch->isTouched();
                if (down && !touchWasDown) {
                    // Top half advances (review, then confirm-erase); bottom
                    // half always backs out (cancel review, or restart with
                    // the reset left pending) — mirrors the keyboard's
                    // R/Enter vs B/X split by screen position instead of key.
                    const bool topHalf = touch->y() < DISPLAY_HEIGHT / 2;
                    if (confirm) {
                        if (topHalf) {
                            display.fillScreen(0x0000); line(53, "Erasing. Keep power connected.");
                            const auto result = performFactoryReset(flash, sd, !scopeKnown);
                            detail = result.detail(); confirm = false; paint = true;
                            Serial.printf("[RESET] %s\n", detail);
                            if (result.ok()) { display.fillScreen(0x0000); line(53, detail); restart(); }
                        } else {
                            confirm = false; paint = true;
                        }
                    } else {
                        if (topHalf) { confirm = true; paint = true; }
                        else { restart(); }
                    }
                }
                touchWasDown = down;
            }
        }
        delay(20);
    }
}

} // namespace handheld
