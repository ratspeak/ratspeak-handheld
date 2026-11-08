#include "TouchInput.h"
#if HAS_TOUCH

TouchInput* TouchInput::_instance = nullptr;

namespace {
constexpr uint16_t GT911_STATUS_REG = 0x814E;
constexpr uint16_t GT911_POINT1_REG = 0x814F;

bool acknowledges(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

bool selectRegister(uint8_t address, uint16_t reg) {
    Wire.beginTransmission(address);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    return Wire.endTransmission() == 0;
}

// Writing zero to the status register tells the GT911 the buffer was consumed.
void releaseBuffer(uint8_t address) {
    Wire.beginTransmission(address);
    Wire.write((uint8_t)(GT911_STATUS_REG >> 8));
    Wire.write((uint8_t)(GT911_STATUS_REG & 0xFF));
    Wire.write(0x00);
    Wire.endTransmission();
}
}  // namespace

bool TouchInput::begin() {
    _instance = this;
    pinMode(TOUCH_INT, INPUT);

    // The controller answers at 0x5D or 0x14 depending on the INT level it saw at reset.
    if (acknowledges(TOUCH_I2C_ADDR_1)) {
        _i2cAddress = TOUCH_I2C_ADDR_1;
    } else if (acknowledges(TOUCH_I2C_ADDR_2)) {
        _i2cAddress = TOUCH_I2C_ADDR_2;
    } else {
        Serial.println("[TOUCH] GT911 not found at 0x5D or 0x14");
        return false;
    }
    Serial.printf("[TOUCH] GT911 at 0x%02X, touch input registered\n", _i2cAddress);
    return true;
}

void TouchInput::update() {
    readGT911();
}

bool TouchInput::readGT911() {
    _touched = false;
    if (!selectRegister(_i2cAddress, GT911_STATUS_REG)) return false;
    Wire.requestFrom(_i2cAddress, (uint8_t)1);
    if (!Wire.available()) return false;

    const uint8_t status = Wire.read();
    const bool bufferReady = (status & 0x80) != 0;
    const uint8_t points = status & 0x0F;
    if (!bufferReady || points == 0) {
        releaseBuffer(_i2cAddress);
        return false;
    }

    // First point: track id, then two little-endian coordinates and the contact size.
    if (!selectRegister(_i2cAddress, GT911_POINT1_REG)) return false;
    Wire.requestFrom(_i2cAddress, (uint8_t)7);
    if (Wire.available() < 7) return false;
    (void)Wire.read();
    int first = Wire.read();
    first |= Wire.read() << 8;
    int second = Wire.read();
    second |= Wire.read() << 8;
    (void)Wire.read();
    (void)Wire.read();

    // The panel is mounted rotated: its first axis is our Y, flipped.
    _y = TFT_HEIGHT - 1 - first;
    _x = second;
    _touched = true;

    // Remap touchpad coordinates to display bounds
    _x = (_x - TOUCH_X_MIN) * (TFT_WIDTH - 1) / (TOUCH_X_MAX - TOUCH_X_MIN);
    _y = (_y - TOUCH_Y_MIN) * (TFT_HEIGHT - 1) / (TOUCH_Y_MAX - TOUCH_Y_MIN);

    if (_x < 0) _x = 0;
    if (_x >= TFT_WIDTH) _x = TFT_WIDTH - 1;
    if (_y < 0) _y = 0;
    if (_y >= TFT_HEIGHT) _y = TFT_HEIGHT - 1;

    releaseBuffer(_i2cAddress);
    return true;
}

#else
bool TouchInput::begin() { return false; }
void TouchInput::update() {}
#endif
