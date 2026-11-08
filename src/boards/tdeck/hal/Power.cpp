#include "Power.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"

// The display and keyboard objects live in main.cpp.
extern Display display;
extern Keyboard keyboard;

namespace {

// Open-circuit LiPo discharge curve, highest voltage first. Percent is
// interpolated linearly between neighbouring points.
struct CurvePoint { float volts; int percent; };
constexpr CurvePoint LIPO_CURVE[] = {
    {3.90f, 100}, {3.80f, 90}, {3.72f, 80}, {3.65f, 70}, {3.59f, 60},
    {3.53f, 50},  {3.48f, 40}, {3.44f, 30}, {3.40f, 20}, {3.36f, 15},
    {3.30f, 10},  {3.15f, 5},  {3.00f, 0},
};
constexpr int LIPO_POINTS = sizeof(LIPO_CURVE) / sizeof(LIPO_CURVE[0]);

int lipoPercent(float volts) {
    if (volts >= LIPO_CURVE[0].volts) return LIPO_CURVE[0].percent;
    for (int i = 1; i < LIPO_POINTS; i++) {
        const CurvePoint& upper = LIPO_CURVE[i - 1];
        const CurvePoint& lower = LIPO_CURVE[i];
        if (volts < lower.volts) continue;
        const float fraction = (volts - lower.volts) / (upper.volts - lower.volts);
        return (int)(lower.percent + fraction * (upper.percent - lower.percent));
    }
    return 0;
}

}  // namespace

void Power::enablePeripherals() {
    // CRITICAL: GPIO 10 must be HIGH to enable all T-Deck Plus peripherals
    pinMode(BOARD_POWER_PIN, OUTPUT);
    digitalWrite(BOARD_POWER_PIN, HIGH);
    delay(10);  // Allow peripherals to stabilize
}

void Power::begin() {
    _lastActivity = millis();
    _state = ACTIVE;

    // Configure battery ADC
    pinMode(BAT_ADC_PIN, INPUT);
    analogReadResolution(12);

    Serial.println("[POWER] Power manager initialized");
}

float Power::batteryVoltage() const {
    // T-Deck Plus: voltage divider on GPIO 4
    int raw = analogRead(BAT_ADC_PIN);
    // Voltage divider: 2x ratio, 3.3V reference, 12-bit ADC
    return (raw / 4095.0f) * 3.3f * 2.0f;
}

int Power::batteryPercent() const {
    // Above the charge threshold the reading is the charger, not the cell;
    // a discharging cell never gets there, so report full.
    if (isCharging()) return 100;

    // The curve assumes a 3.9 V full cell. Shift the reading by the user's
    // measured full voltage so a sagging pack still maps onto the same table,
    // then keep it inside the table's range.
    float v = batteryVoltage() + (3.9f - _fullBatteryV);
    v = constrain(v, 3.0f, 4.2f);

    if (_batteryModel == 1) {
        // Linear model: 3.0 V is empty, 4.2 V is full.
        return (int)((v - 3.0f) / 1.2f * 100.0f);
    }
    return lipoPercent(v);
}

void Power::setBatteryModel(uint8_t model) {
    _batteryModel = model;
}

bool Power::isCharging() const {
    return batteryVoltage() >= _chargeThreshold;
}

void Power::setChargeThreshold(float v)   {
    _chargeThreshold = v;
}

void Power::setFullBatteryVoltage(float v) {
    _fullBatteryV = v;
}
