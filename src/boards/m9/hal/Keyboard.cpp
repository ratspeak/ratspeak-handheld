#include "hal/Keyboard.h"
#include "config/BoardConfig.h"

bool Keyboard::begin() {
    pinMode(KB_INT, INPUT);
    ledcSetup(2, 12000, 8);
    ledcAttachPin(KB_LED, 2);
    backlightOff();
    return probe(true);
}
bool Keyboard::probe(bool reportFailure) {
    Wire.setClock(I2C_FREQUENCY);
    Wire.setTimeOut(20);
    Wire.beginTransmission(0x6c); const uint8_t firstStatus = Wire.endTransmission();
    Wire.beginTransmission(0x6d); const uint8_t secondStatus = Wire.endTransmission();
    // A transport timeout is not proof that the other revision is absent.
    const uint8_t address = firstStatus == 0 && secondStatus == 2 ? 0x6c :
                            firstStatus == 2 && secondStatus == 0 ? 0x6d : 0;
    _address.store(address);
    _lastProbe = millis();
    if (address || reportFailure) {
        Serial.printf("[M9] Keyboard probes: 0x6c=%u 0x6d=%u\n", firstStatus, secondStatus);
        Serial.printf("[M9] Keyboard address=0x%02x revision=%u%s\n", address, revision(),
                      address ? "" : " (waiting; discovery will retry)");
    }
    return address != 0;
}
uint8_t Keyboard::readKey() {
    const uint8_t address = _address.load();
    if (!address) return 0;
    Wire.beginTransmission(address);
    Wire.write(0x01);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom(address, uint8_t{1}) != 1 || !Wire.available()) return 0;
    return Wire.read();
}
void Keyboard::update() {
    if (!_address.load() && uint32_t(millis() - _lastProbe) >= 1000) probe(false);
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
