#include "hal/Power.h"
#include "hal/Expander.h"
#include <Wire.h>
#include <atomic>

namespace {
constexpr uint8_t ADS_REG_CONVERSION = 0x00;
constexpr uint8_t ADS_REG_CONFIG = 0x01;
// Start, AIN0 vs GND, +/-4.096 V, single shot, 860 SPS, comparator off. The
// vendor's +/-2.048 V range clips above a 4.096 V cell; this range does not.
constexpr uint16_t ADS_SINGLE_AIN0 = 0xC3E3;
constexpr float ADS_VOLTS_PER_COUNT = 4.096f / 32768.0f;
constexpr uint32_t BatteryCacheMs = 30000;

std::atomic<bool> s_reading{false};
float s_batteryV = -1.0f;
uint32_t s_batteryAt = 0;
bool s_batteryKnown = false;

bool adsWrite(uint8_t reg, uint16_t value) {
    Wire.beginTransmission(ADS1115_ADDR);
    Wire.write(reg);
    Wire.write(uint8_t(value >> 8));
    Wire.write(uint8_t(value));
    return Wire.endTransmission() == 0;
}

bool adsRead(uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(ADS1115_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(uint8_t(ADS1115_ADDR), uint8_t(2)) != 2) return false;
    value = uint16_t(Wire.read()) << 8;
    value |= Wire.read();
    return true;
}

// The sense divider is switched so it only draws current during a read.
bool sampleBattery(float& volts) {
    if (!wiol2::expander::setOutput(EXP_BATT_SENSE, true)) return false;
    delay(2);
    bool ok = adsWrite(ADS_REG_CONFIG, ADS_SINGLE_AIN0);
    uint16_t config = 0;
    for (int i = 0; ok && i < 5; ++i) {
        delay(2);
        ok = adsRead(ADS_REG_CONFIG, config);
        if (ok && (config & 0x8000)) break;
    }
    uint16_t raw = 0;
    ok = ok && (config & 0x8000) && adsRead(ADS_REG_CONVERSION, raw);
    wiol2::expander::setOutput(EXP_BATT_SENSE, false);
    if (!ok) return false;
    volts = int16_t(raw) * ADS_VOLTS_PER_COUNT * BATT_DIVIDER;
    return true;
}
} // namespace

void Power::enablePeripherals() {
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQUENCY);
    Wire.setTimeOut(20);
    // Radio CS high before its bus starts; the LoRa rail is always on.
    digitalWrite(LORA_CS, HIGH);
    pinMode(LORA_CS, OUTPUT);
    if (!wiol2::expander::begin())
        Serial.println("[POWER] Expander bring-up failed; display and peripherals may be dark");
}

bool Power::setSpeakerPower(bool enable) {
    return wiol2::expander::setOutput(EXP_AUDIO_PA, enable);
}

void Power::begin() {
    _lastActivity = millis();
    _state = ACTIVE;
    float volts = 0;
    if (sampleBattery(volts)) {
        s_batteryV = volts;
        s_batteryKnown = true;
        s_batteryAt = millis();
        Serial.printf("[POWER] ADS1115 battery %.2f V\n", volts);
    } else {
        Serial.printf("[POWER] ADS1115 not responding at 0x%02X\n", ADS1115_ADDR);
    }
}

float Power::batteryVoltage() const {
    // The status bar polls often; sample at most every 30 s.
    if (s_batteryKnown && millis() - s_batteryAt < BatteryCacheMs) return s_batteryV;
    if (s_reading.exchange(true)) return s_batteryV;
    float volts = 0;
    if (sampleBattery(volts)) {
        s_batteryV = volts;
        s_batteryKnown = true;
    }
    s_batteryAt = millis();
    s_reading.store(false);
    return s_batteryV;
}

int Power::batteryPercent() const {
    const float volts = batteryVoltage();
    if (volts < 1.0f) return -1;
    // Uncalibrated linear estimate across the usable LiPo range.
    return constrain(int((volts - 3.3f) * 100.0f / 0.9f), 0, 100);
}

bool Power::isCharging() const {
    // No charger status line is known on this board.
    return false;
}
