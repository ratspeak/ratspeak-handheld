#include "hal/Keyboard.h"
#include "config/BoardConfig.h"

bool Keyboard::begin() {
    Wire.beginTransmission(0x6c); const uint8_t firstStatus = Wire.endTransmission();
    Wire.beginTransmission(0x6d); const uint8_t secondStatus = Wire.endTransmission();
    const bool first = firstStatus == 0, second = secondStatus == 0;
    _address = first != second ? (first ? 0x6c : 0x6d) : 0;
    pinMode(KB_INT, INPUT);
    ledcSetup(2, 12000, 8);
    ledcAttachPin(KB_LED, 2);
    backlightOff();
    Serial.printf("[M9] Keyboard probes: 0x6c=%u 0x6d=%u\n", firstStatus, secondStatus);
    Serial.printf("[M9] Keyboard address=0x%02x revision=%u%s\n", _address, revision(),
                  _address ? "" : " (unknown; GNSS power control disabled)");
    return _address != 0;
}
uint8_t Keyboard::readKey() {
    if (!_address) return 0;
    Wire.beginTransmission(_address);
    Wire.write(0x01);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom(_address, uint8_t{1}) != 1 || !Wire.available()) return 0;
    return Wire.read();
}
void Keyboard::update() {
    const auto kind = m9::decodeKey(readKey(), _event);
    _hasEvent = kind == m9::KeyKind::Key;
    _hold = kind == m9::KeyKind::Hold;
    _activity = kind != m9::KeyKind::None;
}
void Keyboard::discardPending() {
    readKey();
    _event = {}; _hasEvent = _hold = _activity = false;
}
bool Keyboard::setBacklightBrightness(uint8_t percent) {
    _brightness = percent > 100 ? 100 : percent;
    if (_lit) backlightOn();
    return true;
}
bool Keyboard::backlightOn() {
    ledcWrite(2, uint32_t(_brightness) * 255 / 100);
    _lit = _brightness != 0;
    return true;
}
bool Keyboard::backlightOff() { ledcWrite(2, 0); _lit = false; return true; }
