#pragma once

#include <Arduino.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418_registers.h>

// Direct FIFO reader for the Cardputer ADV keyboard controller. The
// M5Cardputer 1.1.x reader only drains the FIFO after its GPIO ISR fires; a
// timed call to updateKeyList() is therefore not a real polling fallback.
// This adapter checks the TCA8418 event counter directly and works with or
// without the interrupt edge.
class CardputerAdvKeyboard {
public:
    struct Event {
        uint8_t row = 0;
        uint8_t col = 0;
        bool pressed = false;
    };

    bool begin() {
        pinMode(INTERRUPT_PIN, INPUT_PULLUP);
        if (!_controller.begin()) {
            Serial.println("[KEYBOARD] TCA8418 initialization failed");
            _ready = false;
            return false;
        }

        _controller.matrix(7, 8);
        _controller.enableDebounce();
        _controller.flush();
        _controller.enableInterrupts();
        _lastPoll = millis();
        _ready = true;
        Serial.println("[KEYBOARD] Cardputer ADV TCA8418 ready (direct FIFO)");
        return true;
    }

    size_t poll(Event* output, size_t capacity) {
        if (!_ready || output == nullptr || capacity == 0) return 0;

        const unsigned long now = millis();
        const bool irqActive = digitalRead(INTERRUPT_PIN) == LOW;
        const bool timedPoll = now - _lastPoll >= FALLBACK_POLL_MS;
        if (!irqActive && !timedPoll) return 0;
        _lastPoll = now;

        uint8_t count = _controller.available();
        size_t written = 0;
        while (count-- > 0) {
            const uint8_t raw = _controller.getEvent();
            const uint8_t code = raw & 0x7f;
            if (code == 0) continue;

            const uint8_t matrix = code - 1;
            const uint8_t rawRow = matrix / 10;
            const uint8_t rawCol = matrix % 10;
            if (rawRow >= 7 || rawCol >= 8) continue;

            // M5Stack's ADV matrix is electrically 7x8 but physically 4x14.
            const uint8_t logicalRow = (rawCol + 4) % 4;
            const uint8_t logicalCol = rawRow * 2 + (rawCol > 3 ? 1 : 0);
            if (logicalRow >= 4 || logicalCol >= 14) continue;

            if (written < capacity) {
                output[written++] = {logicalRow, logicalCol,
                                     (raw & 0x80) != 0};
            }
        }

        // Acknowledge the keypad interrupt after the FIFO is drained.
        _controller.writeRegister8(TCA8418_REG_INT_STAT,
                                   TCA8418_REG_STAT_K_INT);
        return written;
    }

    bool ready() const { return _ready; }

private:
    static constexpr uint8_t INTERRUPT_PIN = 11;
    static constexpr unsigned long FALLBACK_POLL_MS = 20;

    Adafruit_TCA8418 _controller;
    bool _ready = false;
    unsigned long _lastPoll = 0;
};
