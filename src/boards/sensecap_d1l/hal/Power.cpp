#include "hal/Power.h"

// No confirmed battery ADC/gauge pin on this board yet (HAS_BATTERY_MODEL
// is false — see BoardConfig.h) and no confirmed master peripheral-enable
// GPIO either: the IO expander, display, radio, and touch all init directly
// without one, per this board's confirmed hardware bring-up. Stub
// everything battery-related rather than guess a pin and report a fake
// reading; revisit if/when a schematic or probing confirms one.
void Power::enablePeripherals() {}

void Power::begin() {
    _lastActivity = millis();
    _state = ACTIVE;
}

float Power::batteryVoltage() const { return 0.0f; }
int Power::batteryPercent() const { return 0; }
bool Power::isCharging() const { return false; }
