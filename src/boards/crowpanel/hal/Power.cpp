#include "hal/Power.h"
void Power::enablePeripherals() {
 digitalWrite(LORA_CS,HIGH); pinMode(LORA_CS,OUTPUT);
 pinMode(TOUCH_INT,INPUT);
}
void Power::begin() { _lastActivity=millis(); _state=ACTIVE; }
// No verified battery ADC on this board. Do not invent a state of charge.
float Power::batteryVoltage() const { return 0; }
int Power::batteryPercent() const { return -1; }
bool Power::isCharging() const { return false; }
void Power::setBatteryModel(uint8_t) {}
void Power::setChargeThreshold(float) {}
void Power::setFullBatteryVoltage(float) {}
