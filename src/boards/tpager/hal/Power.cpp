#include "Power.h"
#include "PagerBattery.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
#include <Wire.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// Forward declarations — display & keyboard instances provided externally
extern Display display;
extern Keyboard keyboard;

namespace {
constexpr uint8_t XL_REG_OUTPUT_0 = 0x02;
constexpr uint8_t XL_REG_OUTPUT_1 = 0x03;
constexpr uint8_t XL_REG_CONFIG_0 = 0x06;
constexpr uint8_t XL_REG_CONFIG_1 = 0x07;

bool xlWrite8(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool xlRead8(uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)XL9555_ADDR, (uint8_t)1) != 1) return false;
    value = Wire.read();
    return true;
}

bool xlWrite16(uint8_t regLo, uint16_t value) {
    return xlWrite8(regLo, value & 0xFF) && xlWrite8(regLo + 1, value >> 8);
}

bool xlRead16(uint8_t regLo, uint16_t& value) {
    uint8_t lo = 0, hi = 0;
    if (!xlRead8(regLo, lo) || !xlRead8(regLo + 1, hi)) return false;
    value = (uint16_t)lo | ((uint16_t)hi << 8);
    return true;
}

uint16_t xlBit(uint8_t pin) {
    return (uint16_t)1U << pin;
}

uint16_t peripheralRails() {
    return xlBit(XL9555_DRV_EN) |
           xlBit(XL9555_AMP_EN) |
           xlBit(XL9555_KB_RST) |
           xlBit(XL9555_LORA_EN) |
           xlBit(XL9555_GPS_EN) |
           xlBit(XL9555_NFC_EN) |
           xlBit(XL9555_GPS_RST) |
           xlBit(XL9555_KB_EN) |
           xlBit(XL9555_GPIO_EN) |
           xlBit(XL9555_SD_PULLEN) |
           xlBit(XL9555_SD_EN);
}

pager::Battery<TwoWire> battery(Wire);
}  // namespace

void Power::enablePeripherals() {
    uint16_t outputs = 0;
    uint16_t config = 0xFFFF;
    xlRead16(XL_REG_OUTPUT_0, outputs);
    xlRead16(XL_REG_CONFIG_0, config);

    const uint16_t speaker = xlBit(XL9555_AMP_EN);
    const uint16_t rails = peripheralRails() & ~speaker;

    outputs |= rails;
    outputs &= ~speaker;
    config &= ~(rails | speaker);    // 0 = output, 1 = input on XL9555/PCA9555
    config |= xlBit(XL9555_SD_DET);  // card detect remains input

    bool ok = xlWrite16(XL_REG_OUTPUT_0, outputs) && xlWrite16(XL_REG_CONFIG_0, config);
    Serial.printf("[POWER] XL9555 peripheral rails %s\n", ok ? "enabled" : "not detected");
    delay(20);
}

bool Power::setSpeakerPower(bool enable) {
    uint16_t outputs = 0;
    uint16_t config = 0xFFFF;
    if (!xlRead16(XL_REG_OUTPUT_0, outputs) || !xlRead16(XL_REG_CONFIG_0, config)) {
        Serial.printf("[POWER] Speaker amp %s failed\n", enable ? "enable" : "disable");
        return false;
    }

    const uint16_t speaker = xlBit(XL9555_AMP_EN);
    if (enable) {
        outputs |= speaker;
    } else {
        outputs &= ~speaker;
    }
    config &= ~speaker;

    bool ok = xlWrite16(XL_REG_OUTPUT_0, outputs) && xlWrite16(XL_REG_CONFIG_0, config);
    Serial.printf("[POWER] Speaker amp %s%s\n", enable ? "enabled" : "disabled", ok ? "" : " failed");
    return ok;
}

void Power::begin() {
    _lastActivity = millis();
    _state = ACTIVE;

    if (BAT_ADC_PIN >= 0) {
        pinMode(BAT_ADC_PIN, INPUT);
        analogReadResolution(12);
    }

    pinMode(BTN_BOOT, INPUT_PULLUP);

    const auto initialized = battery.begin();
    Serial.printf("[POWER] BQ25896 %s\n", battery.initName(initialized));
    printBatteryDiagnostics();

    Serial.println("[POWER] Power manager initialized");
}

float Power::batteryVoltage() const {
    const auto sample = battery.gauge();
    return sample.present() && sample.voltageMv > 0 && sample.voltageMv <= 6000
        ? sample.voltageMv / 1000.0f : -1.0f;
}

int Power::batteryPercent() const {
    // An uninitialized/configuring gauge or invalid SOC is unknown, not full.
    // Do not estimate SOC from USB-influenced voltage or rewrite gauge learning.
    return battery.gauge().percent();
}

void Power::printBatteryDiagnostics() const {
    battery.printDiagnostics(Serial);
}

uint8_t Power::percentToPWM(uint8_t pct) const {
    if (pct == 0) return 0;
    if (pct >= 100) return 255;
    // Map 1-100 to ~6-255 (minimum visible PWM ~6)
    return (uint8_t)(6 + (uint16_t)(pct - 1) * 249 / 99);
}

void Power::activity() {
    _lastActivity = millis();
    if (_state == SCREEN_OFF) {
        _justWokeFromOff = true;
    }
    if (_state != ACTIVE) {
        setState(ACTIVE);
    }
}

void Power::forceScreenOff() {
    if (_justWokeFromOff) {
        _justWokeFromOff = false;
        return;
    }
    setState(SCREEN_OFF);
}

void Power::weakActivity() {
    _lastActivity = millis();
    // Encoder movement wakes from DIM but not from SCREEN_OFF
    if (_state == DIMMED) {
        setState(ACTIVE);
    }
}

void Power::setBrightness(uint8_t percent) {
    _brightnessPct = constrain(percent, 1, 100);
    if (_state == ACTIVE) {
        display.setBrightness(percentToPWM(_brightnessPct));
    }
}

void Power::setKbBrightness(uint8_t percent, bool apply) {
    percent = constrain(percent, 0, 100);
    keyboard.setBacklightBrightness(percent);
    if (percent == 0) {
        keyboard.backlightOff();
    } else if (apply) { // Show the new brightness
        keyboard.backlightOn();
    }
}

void Power::pollBootButton() {
    bool down = digitalRead(BTN_BOOT) == LOW;
    if (!down) {
        if (_btnWasDown && _btnFromScreenOn && !_gestureLatched) {
            _screenSleepPending = true;
        }
        _btnWasDown = false;
        _btnFromScreenOn = false;
        _gestureLatched = false;
        return;
    }
    if (!_btnWasDown) {
        _btnWasDown = true;
        _btnFromScreenOn = isScreenOn();
        _btnDownMs = millis();
        if (_btnFromScreenOn) {
            _gestureLatched = false;
        } else {
            activity();
        }
        return;
    }
    unsigned long held = millis() - _btnDownMs;
    if (held >= POWEROFF_FORCE_MS) {
        Serial.println("[POWER] BOOT force-hold");
        powerOff();
    } else if (_btnFromScreenOn && !_gestureLatched && held >= POWEROFF_PROMPT_MS) {
        _gestureLatched = true;
        _gesturePending = true;
    }
}

bool Power::screenSleepGestureFired() {
    bool fired = _screenSleepPending;
    _screenSleepPending = false;
    return fired;
}

bool Power::powerOffGestureFired() {
    bool fired = _gesturePending;
    _gesturePending = false;
    return fired;
}

bool Power::vbusPresent() const {
    uint8_t status = 0;
    return battery.readCharger(0x0b, status) && (status & 0x04);
}

void Power::disablePeripherals() {
    uint16_t outputs = 0;
    if (xlRead16(XL_REG_OUTPUT_0, outputs)) {
        xlWrite16(XL_REG_OUTPUT_0, outputs & ~peripheralRails());
    }
}

void Power::powerOff() {
    Serial.println("[POWER] Power off");

    display.sleep();
    keyboard.backlightOff();

    // Wait out a still-held BOOT so the ext0 fallback below can't instantly
    // re-wake (bounded in case the pin is stuck).
    unsigned long waitStart = millis();
    while (digitalRead(BTN_BOOT) == LOW && millis() - waitStart < 15000) delay(10);
    delay(50);

    // On USB, retain the charging path and park only the MCU/peripherals.
    // On battery, ship mode cuts power. Read/write failure leaves deep sleep
    // as the bounded fallback; do not claim that the hardware cut succeeded.
    const auto sleep = battery.prepareSleep();
    switch (sleep) {
        case pager::Battery<TwoWire>::Sleep::UsbStandby:
            Serial.println("[POWER] USB standby; battery remains connected"); break;
        case pager::Battery<TwoWire>::Sleep::BatteryShip:
            Serial.println("[POWER] ship requested; still powered, parking"); break;
        case pager::Battery<TwoWire>::Sleep::Unverified:
            Serial.println("[POWER] charger sleep state unverified; parking"); break;
    }
    Serial.flush();

    disablePeripherals();

    // Wake on encoder click. NOT BOOT/GPIO0: it's a strapping pin, and a
    // held BOOT at the wake edge straps the ROM into download mode — the
    // device looks dead until RST.
    rtc_gpio_pullup_en((gpio_num_t)ROTARY_CLICK);
    rtc_gpio_pulldown_dis((gpio_num_t)ROTARY_CLICK);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)ROTARY_CLICK, 0);
    esp_deep_sleep_start();
}

void Power::loop() {
    pollBootButton();

    unsigned long elapsed = millis() - _lastActivity;

    switch (_state) {
        case ACTIVE:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            } else if (_dimTimeout > 0 && elapsed >= _dimTimeout) {
                setState(DIMMED);
            }
            break;

        case DIMMED:
            if (_offTimeout > 0 && elapsed >= _offTimeout) {
                setState(SCREEN_OFF);
            }
            break;

        case SCREEN_OFF:
            break;
    }

    _justWokeFromOff = false;
}

void Power::setState(State newState) {
    if (newState == _state) return;
    const char* names[] = {"ACTIVE", "DIMMED", "SCREEN_OFF"};
    Serial.printf("[POWER] %s -> %s\n", names[_state], names[newState]);
    State oldState = _state;
    _state = newState;

    switch (_state) {
        case ACTIVE:
            if (oldState == SCREEN_OFF) {
                // Pre-load correct brightness into LovyanGFX state before wakeup.
                // wakeup() sends SLPOUT then restores LGFX's internal _brightness
                // to the LEDC — with this ordering, it restores the correct value
                // instead of a stale 0, eliminating the rapid 0→0→correct triple-
                // write that can cause missed LEDC duty updates on ESP32-S3.
                display.setBrightness(percentToPWM(_brightnessPct));
                display.wakeup();
            } else {
                display.setBrightness(percentToPWM(_brightnessPct));
            }
            // On wake, relight only what screen-off forced dark (or per auto-on) —
            // never force-enable for users who keep the kb light off.
            if (_kbAutoOn || (oldState == SCREEN_OFF && _kbLitBeforeOff)) {
                keyboard.backlightOn();
            }
            break;
        case DIMMED:
            display.setBrightness(DIM_PWM);
            if (_kbAutoOff) {
                keyboard.backlightOff();
            }
            break;
        case SCREEN_OFF:
            // LovyanGFX sleep() sets brightness to 0 internally — no
            // need to call setBrightness(0) beforehand.
            display.sleep();
            // Kb backlight always follows screen-off — the timeout exists to save battery.
            _kbLitBeforeOff = keyboard.backlightIsLit();
            keyboard.backlightOff();
            break;
    }
}
