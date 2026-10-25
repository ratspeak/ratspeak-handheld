#pragma once

#include "storage/FlashStore.h"

namespace handheld {

// Startup-only, before reset recovery and on every board: resolves internal
// storage that did not mount. Returns only once FlashStore classified it as
// ours; otherwise owns the framebuffer until restart. Preparation restarts so
// the boot that follows runs with storage, radio and SD in their normal order.
template<class Keyboard, class Display, class Restart>
void storageStartup(FlashStore& flash, Keyboard& keyboard, Display& display, Restart restart) {
    using Startup = FlashStore::Startup;
    Startup state = flash.startup();
    if (state == Startup::Ready) return;
    const auto line = [&](int y, const char* text) {
        display.setCursor(10, y); display.print(text);
    };
    const auto screen = [&](const char* title) {
        display.fillScreen(0x0000);
        display.setTextWrap(false);
        display.setTextColor(0xffff, 0x0000);
        display.setTextFont(1); display.setTextSize(2);
        line(10, title);
        display.setTextSize(1);
    };
    const auto prepare = [&](const char* doing) {
        keyboard.discardPending();
        screen("Internal storage"); line(53, doing);
        if (flash.prepare()) {
            screen("Internal storage"); line(53, "Storage ready. Restarting.");
            delay(800); restart();
        }
    };
    const char* problem = "Internal storage can't be read.";
    const char* detail = "Nothing has been changed.";
    if (state == Startup::Unprepared) {
        prepare("Preparing internal storage...");
        state = Startup::Failed;
        problem = "Internal storage could not be set up."; detail = "Restart to retry.";
    }
    const bool choice = state == Startup::Damaged || state == Startup::Foreign;
    bool confirm = false, paint = true, failed = false;
    for (;;) {
        if (paint) {
            screen("Internal storage");
            if (confirm) {
                line(37, "Erase internal storage?");
                line(53, state == Startup::Foreign ? "The other app's files are deleted." : "Everything on it is deleted.");
                line(72, "The SD card is not touched.");
                line(90, "ENTER: erase   B: back");
            } else if (choice) {
                line(37, state == Startup::Foreign ? "It holds another app's files." : "Existing data can't be opened.");
                line(53, failed ? "Erase failed. Nothing else changed." : "Nothing has been changed.");
                line(72, "X: keep it and restart");
                line(90, "E: erase and start fresh");
                line(112, "Back up first to keep this data.");
            } else if (state == Startup::Missing) {
                line(37, "No storage partition was found.");
                line(53, "Install the complete .bin or ZIP");
                line(65, "image for this device.");
                line(112, "X: restart");
            } else {
                line(37, problem);
                line(53, detail);
                line(112, "X: restart");
            }
            // An event queued before this screen cannot choose for the user.
            keyboard.discardPending(); paint = false;
        }
        keyboard.update();
        if (keyboard.hasEvent()) {
            const auto event = keyboard.getEvent();
            if (!event.ctrl && !event.alt && !event.fn && !event.repeat) {
                const char key = event.character;
                if (!confirm && (key == 'x' || key == 'X')) {
                    keyboard.discardPending(); restart();
                } else if (confirm && (key == 27 || key == 'b' || key == 'B')) {
                    confirm = false; paint = true;
                } else if (choice && !confirm && (key == 'e' || key == 'E')) {
                    confirm = true; paint = true;
                } else if (confirm && event.enter) {
                    prepare("Erasing. Keep power connected.");
                    confirm = false; failed = paint = true;
                }
            }
        }
        delay(20);
    }
}

} // namespace handheld
