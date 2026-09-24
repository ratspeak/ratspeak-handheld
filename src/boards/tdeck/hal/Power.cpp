#include "Power.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"


// Forward declarations — display & keyboard instances provided externally
extern Display display;
extern Keyboard keyboard;

// Battery type lookup table for getting voltage vs percent
namespace {

    struct VoltPoint { float v; int pct; };
    static constexpr VoltPoint LIPO_CURVE[] = {
        {3.90f, 100},
        {3.80f, 90},
        {3.72f, 80},
        {3.65f, 70},
        {3.59f, 60},
        {3.53f, 50},
        {3.48f, 40},
        {3.44f, 30},
        {3.40f, 20},
        {3.36f, 15},
        {3.30f, 10},
        {3.15f, 5},
        {3.00f, 0},
    };
    constexpr int LIPO_CURVE_N = sizeof(LIPO_CURVE) / sizeof(LIPO_CURVE[0]);
}






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
    float v = batteryVoltage();

    // Charging: voltage exceeds the discharge curve's top (about 3.9V = full).
    // A discharging cell never gets there, so treat this as full.
    if (isCharging())
        return 100;

    // Compensate for load-induced voltage drop when running on battery.
    // Calculates an offset and adds it to real voltage for to be able using default LiPo lookup table
    v += (3.9f - _fullBatteryV);

    // Clamp to valid curve range.
    v = constrain(v, 3.0f, 4.2f);

    if (_batteryModel == 1) {
        // Linear: distribution across 3.0–4.2V.
        return (int)((v - 3.0f) / 1.2f * 100.0f);
    }

    // LiPo: interpolate between nearest table entries.
    for (int i = 0; i < LIPO_CURVE_N - 1; i++) {
        if (v >= LIPO_CURVE[i + 1].v) {
            float t = (v - LIPO_CURVE[i + 1].v) / (LIPO_CURVE[i].v - LIPO_CURVE[i + 1].v);
            return (int)(LIPO_CURVE[i + 1].pct + t * (LIPO_CURVE[i].pct - LIPO_CURVE[i + 1].pct));
        }
    }
    return 0;
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
