#pragma once
#ifdef RSCROWPANEL
#include "input/RecoveryInput.h"
#endif

#include "FactoryReset.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"

namespace handheld {

// Startup-only recovery: display and keyboard are ready, but no config,
// identity, protocol, or worker owner has been admitted. All boards use this
// same explicit confirmation and reset sequence. The framebuffer is owned by
// this loop until restart; normal LVGL/Canvas/network schedulers never run.
template<class Keyboard, class Display, class Restart>
void factoryResetRecovery(FlashStore& flash, SDStore& sd, Keyboard& hardwareKeyboard,
                          Display& display, Restart restart) {
#ifdef RSCROWPANEL
    crowpanel::RecoveryInput<Display> keyboard(display);
#else
    auto& keyboard=hardwareKeyboard;
#endif
    if (flash.resetState() == FlashStore::ResetState::Clear) return;
    bool confirm = false, paint = true, scopeKnown = false, includesSD = false;
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
                line(90, "ENTER: erase   B: back");
            } else {
                line(72, includesSD && scopeKnown && !sd.isReady() ? "Insert the SD card to retry." : "Old data will not be restored.");
                line(90, "R: review reset");
            }
            line(112, "X: restart (reset stays pending)");
            // An event queued before this warning cannot authorize its action.
            keyboard.discardPending(); paint = false;
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
                    if (result.ok()) { display.fillScreen(0x0000); line(53, detail); restart(); }
                }
            }
        }
        delay(20);
    }
}

} // namespace handheld
