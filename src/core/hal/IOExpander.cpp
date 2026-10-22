#include "IOExpander.h"

bool IOExpander::begin(TwoWire& wire, uint8_t addr) {
    _wire = &wire;
    _addr = addr;
    _wire->beginTransmission(_addr);
    _ready = (_wire->endTransmission() == 0);
    if (_ready) {
        // Reassert the power-on-default shadow state so a warm reset (which
        // doesn't power-cycle the expander chip) starts from a known state
        // rather than whatever a previous session left in its registers.
        writeReg16(0x06, _config);
        writeReg16(0x02, _output);
    }
    return _ready;
}

bool IOExpander::writeReg16(uint8_t reg, uint16_t value) {
    if (!_wire) return false;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(uint8_t(value & 0xFF));
    _wire->write(uint8_t(value >> 8));
    return _wire->endTransmission() == 0;
}

bool IOExpander::readReg16(uint8_t reg, uint16_t& value) {
    if (!_wire) return false;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(_addr, (uint8_t)2) != 2) return false;
    uint8_t lo = _wire->read();
    uint8_t hi = _wire->read();
    value = uint16_t(lo) | (uint16_t(hi) << 8);
    return true;
}

void IOExpander::pinMode(uint8_t bit, uint8_t mode) {
    if (!_ready || bit > 15) return;
    const uint16_t mask = uint16_t(1) << bit;
    // This chip family's config register: 1 = input, 0 = output.
    if (mode == OUTPUT) _config &= ~mask;
    else _config |= mask;
    writeReg16(0x06, _config);
}

void IOExpander::digitalWrite(uint8_t bit, uint8_t level) {
    if (!_ready || bit > 15) return;
    const uint16_t mask = uint16_t(1) << bit;
    if (level) _output |= mask;
    else _output &= ~mask;
    writeReg16(0x02, _output);
}

uint8_t IOExpander::digitalRead(uint8_t bit) {
    if (!_ready || bit > 15) return LOW;
    uint16_t value = 0;
    if (!readReg16(0x00, value)) return LOW;
    return (value & (uint16_t(1) << bit)) ? HIGH : LOW;
}
