#include "hal/Expander.h"
#include "config/BoardConfig.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace wiol2::expander {
namespace {
constexpr uint8_t REG_INPUT_0 = 0x00;
constexpr uint8_t REG_OUTPUT_0 = 0x02;
constexpr uint8_t REG_CONFIG_0 = 0x06;  // 1 = input, 0 = output

uint16_t s_output = 0xFFFF;
uint16_t s_config = 0xFFFF;
bool s_ready = false;

SemaphoreHandle_t lock() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

class Guard {
public:
    Guard() : _held(lock() && xSemaphoreTake(lock(), portMAX_DELAY) == pdTRUE) {}
    ~Guard() { if (_held) xSemaphoreGive(lock()); }
    bool held() const { return _held; }
private:
    bool _held;
};

bool write16(uint8_t reg, uint16_t value) {
    Wire.beginTransmission(EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(uint8_t(value));
    Wire.write(uint8_t(value >> 8));
    return Wire.endTransmission() == 0;
}

bool read16(uint8_t reg, uint16_t& value) {
    Wire.beginTransmission(EXPANDER_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(uint8_t(EXPANDER_ADDR), uint8_t(2)) != 2) return false;
    value = Wire.read();
    value |= uint16_t(Wire.read()) << 8;
    return true;
}

void stage(uint8_t bit, bool high) {
    const uint16_t mask = uint16_t(1U) << bit;
    s_output = high ? (s_output | mask) : (s_output & ~mask);
    s_config &= ~mask;
}

// Latch outputs before direction so a newly driven pin starts at its value.
bool commit() {
    return write16(REG_OUTPUT_0, s_output) && write16(REG_CONFIG_0, s_config);
}

bool step(uint8_t bit, bool high, uint32_t settleMs, const char* what) {
    stage(bit, high);
    if (!commit()) {
        Serial.printf("[WIO] Expander %s failed\n", what);
        return false;
    }
    if (settleMs) delay(settleMs);
    return true;
}
} // namespace

bool begin() {
    Guard guard;
    if (!guard.held()) return false;
    s_ready = false;
    if (!read16(REG_OUTPUT_0, s_output) || !read16(REG_CONFIG_0, s_config)) {
        Serial.printf("[WIO] Expander not found at 0x%02X\n", EXPANDER_ADDR);
        return false;
    }
    for (uint8_t bit : {EXP_WAKE_BUTTON, EXP_I2C_INT, EXP_SD_DETECT, EXP_LCD_CTRL})
        s_config |= uint16_t(1U) << bit;
    stage(EXP_TOUCH_INT, false);
    stage(EXP_LCD_POWER, true);
    stage(EXP_LCD_RST, true);
    stage(EXP_GROVE_POWER, false);
    stage(EXP_TOUCH_RST, false);
    stage(EXP_GNSS_RST, false);
    stage(EXP_USER_LED, false);
    stage(EXP_USB_OTG, false);
    stage(EXP_AUDIO_PA, false);
    stage(EXP_GNSS_POWER, false);  // GPSManager powers it when enabled
    stage(EXP_SD_POWER, true);
    stage(EXP_BATT_SENSE, false);
    // Vendor sequence: LCD reset pulse, 500 ms panel settle, LCD control
    // high, then release touch reset with INT low to select 0x5D.
    const bool ok = step(EXP_LCD_RST, true, 40, "rail setup") &&
                    step(EXP_LCD_RST, false, 10, "LCD reset") &&
                    step(EXP_LCD_RST, true, 500, "LCD reset release") &&
                    step(EXP_LCD_CTRL, true, 10, "LCD control") &&
                    step(EXP_TOUCH_RST, true, 60, "touch reset release");
    s_ready = ok;
    Serial.printf("[WIO] Expander %s out=0x%04X cfg=0x%04X\n",
                  ok ? "ready" : "incomplete", s_output, s_config);
    return ok;
}

bool ready() { return s_ready; }

bool setOutput(uint8_t bit, bool high) {
    Guard guard;
    if (!guard.held() || !s_ready) return false;
    const uint16_t output = s_output, config = s_config;
    stage(bit, high);
    if (commit()) return true;
    s_output = output;
    s_config = config;
    return false;
}

bool readInput(uint8_t bit, bool& high) {
    Guard guard;
    uint16_t input = 0;
    if (!guard.held() || !s_ready || !read16(REG_INPUT_0, input)) return false;
    high = input & (uint16_t(1U) << bit);
    return true;
}
} // namespace wiol2::expander
