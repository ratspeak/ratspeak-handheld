#include "config/BoardConfig.h"
#if defined(RSDECK) || defined(RSM9)
#include "hal/Power.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
extern Display display;
extern Keyboard keyboard;

uint8_t Power::percentToPWM(uint8_t pct) const {
    if (pct == 0) return 0;
    if (pct >= 100) return 255;
    // Map 1-100 to ~6-255 (minimum visible PWM ~6)
    return (uint8_t)(6 + (uint16_t)(pct - 1) * 249 / 99);
}

void Power::activity() {
    _lastActivity = millis();
    if (_state == SCREEN_OFF) {
        _justWokeFromOff = true;
    }
    if (_state != ACTIVE) {
        setState(ACTIVE);
    }
}

void Power::forceScreenOff() {
    if (_justWokeFromOff) {
        _justWokeFromOff = false;
        return;
    }
    setState(SCREEN_OFF);
}

void Power::weakActivity() {
    _lastActivity = millis();
    // Trackball wakes from DIM but not from SCREEN_OFF
    if (_state == DIMMED) {
        setState(ACTIVE);
    }
}

void Power::setBrightness(uint8_t percent) {
    _brightnessPct = constrain(percent, 1, 100);
    if (_state == ACTIVE) {
        display.setBrightness(percentToPWM(_brightnessPct));
    }
}

void Power::setKbBrightness(uint8_t percent, bool apply) {
    percent = constrain(percent, 0, 100);
    keyboard.setBacklightBrightness(percent);
    if (percent == 0) {
        keyboard.backlightOff();
    } else if (apply) { // Show the new brightness
        keyboard.backlightOn();
    }
}

void Power::loop() {
    unsigned long elapsed = millis() - _lastActivity;

    switch (_state) {
        case ACTIVE:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            } else if (_dimTimeout > 0 && elapsed >= _dimTimeout) {
                setState(DIMMED);
            }
            break;

        case DIMMED:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            }
            break;

        case SCREEN_OFF:
            break;
    }

    _justWokeFromOff = false;
}

void Power::setState(State newState) {
    if (newState == _state) return;
    const char* names[] = {"ACTIVE", "DIMMED", "SCREEN_OFF"};
    Serial.printf("[POWER] %s -> %s\n", names[_state], names[newState]);
    State oldState = _state;
    _state = newState;

    switch (_state) {
        case ACTIVE:
            if (oldState == SCREEN_OFF) {
                // Pre-load correct brightness into LovyanGFX state before wakeup.
                // wakeup() sends SLPOUT then restores LGFX's internal _brightness
                // to the LEDC — with this ordering, it restores the correct value
                // instead of a stale 0, eliminating the rapid 0→0→correct triple-
                // write that can cause missed LEDC duty updates on ESP32-S3.
                display.setBrightness(percentToPWM(_brightnessPct));
                display.wakeup();
            } else {
                display.setBrightness(percentToPWM(_brightnessPct));
            }
            // On wake, relight only what screen-off forced dark (or per auto-on) —
            // never force-enable for users who keep the kb light off.
            if (_kbAutoOn || (oldState == SCREEN_OFF && _kbLitBeforeOff)) {
                keyboard.backlightOn();
            }
            break;
        case DIMMED:
            display.setBrightness(DIM_PWM);
            if (_kbAutoOff) {
                keyboard.backlightOff();
            }
            break;
        case SCREEN_OFF:
            // LovyanGFX sleep() sets brightness to 0 internally — no
            // need to call setBrightness(0) beforehand.
            display.sleep();
            // Kb backlight always follows screen-off — the timeout exists to save battery.
            _kbLitBeforeOff = keyboard.backlightIsLit();
            keyboard.backlightOff();
            break;
    }
}

#endif
