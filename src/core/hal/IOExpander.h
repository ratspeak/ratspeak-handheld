#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal driver for the standard NXP-style 16-bit I2C GPIO expander
// (TCA9535/PCA9535/TCA9555 family — same register map across that family).
// Independent implementation of the well-documented standard register
// layout (input 0x00/0x01, output 0x02/0x03, config 0x06/0x07) — not a port
// of any other project's driver for this same chip.
class IOExpander {
public:
    bool begin(TwoWire& wire, uint8_t addr);
    // bit: 0-15. mode: INPUT or OUTPUT (Arduino constants).
    void pinMode(uint8_t bit, uint8_t mode);
    void digitalWrite(uint8_t bit, uint8_t level);
    uint8_t digitalRead(uint8_t bit);

private:
    bool writeReg16(uint8_t reg, uint16_t value);
    bool readReg16(uint8_t reg, uint16_t& value);

    TwoWire* _wire = nullptr;
    uint8_t _addr = 0;
    bool _ready = false;
    // Shadow copies — this driver is the sole writer, so a read-modify-write
    // round trip over I2C isn't needed for every pin change. Power-on
    // defaults for this chip family: all pins input, output register all-high.
    uint16_t _config = 0xFFFF;
    uint16_t _output = 0xFFFF;
};
