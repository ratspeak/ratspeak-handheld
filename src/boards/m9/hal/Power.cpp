#include "hal/Power.h"
#include "hal/Keyboard.h"
#include <Wire.h>
extern Keyboard keyboard;

void Power::enablePeripherals() {
    for (int cs : {LORA_CS, SD_CS, TFT_CS}) { digitalWrite(cs, HIGH); pinMode(cs, OUTPUT); }
    digitalWrite(TFT_BL, HIGH); pinMode(TFT_BL, OUTPUT);
    digitalWrite(BOARD_POWER_PIN, LOW); pinMode(BOARD_POWER_PIN, OUTPUT);
    // Keep revision-dependent GNSS enable undriven until keyboard detection.
    pinMode(GPS_ENABLE_PIN, INPUT);
    delay(10);
}
void Power::begin() {
    _lastActivity = millis(); _state = ACTIVE;
    pinMode(BAT_ADC_PIN, INPUT);
    pinMode(EXT_POWER_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    Wire1.begin(PERIPHERAL_I2C_SDA, PERIPHERAL_I2C_SCL);
    Wire1.setTimeOut(20);
    if (keyboard.revision()) {
        // Both known board revisions retain the GNSS reset pin; power differs.
        digitalWrite(GPS_RESET_PIN, HIGH); pinMode(GPS_RESET_PIN, OUTPUT);
        digitalWrite(GPS_ENABLE_PIN, keyboard.revision() == 1 ? HIGH : LOW);
        pinMode(GPS_ENABLE_PIN, OUTPUT);
    }
}
float Power::batteryVoltage() const { return analogReadMilliVolts(BAT_ADC_PIN) * 0.002f; }
int Power::batteryPercent() const {
    // Uncalibrated beta estimate for the specified high-voltage cell, not the
    // T-Deck's loaded-cell curve. Voltage remains available in the status UI.
    return constrain(int((batteryVoltage() - 3.3f) * 100.0f / 1.05f), 0, 100);
}
bool Power::isCharging() const {
    // External power is confirmed by the board reference. DONE polarity has
    // not been qualified, so do not invent a charge-complete transition.
    return digitalRead(EXT_POWER_PIN) == LOW;
}
