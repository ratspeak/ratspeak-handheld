#include "hal/Display.h"
#include <Wire.h>
#include <lvgl.h>
#include "runtime/RuntimeMetrics.h"
#include "LvFailureDisplay.h"

namespace {
constexpr uint8_t REG_DEVICE_CONFIG0 = 0x00;
constexpr uint8_t REG_MAX_CURRENT = 0x01;
constexpr uint8_t REG_ENABLE = 0x02;
constexpr uint8_t REG_DIM_MODE = 0x04;
constexpr uint8_t REG_ENGINE_MODE = 0x05;
constexpr uint8_t REG_UPDATE = 0x0F;
constexpr uint8_t REG_LED0_DC = 0x14;
constexpr uint8_t REG_LED0_PWM = 0x18;
constexpr uint8_t LED_DC = 200;

constexpr uint32_t BufferPixels = 320 * 20;
lv_color_t* s_buf1 = nullptr;
lv_color_t* s_buf2 = nullptr;
LGFX_WioL2* s_gfx = nullptr;

// The display has its own SPI host, so flushing never waits on the radio.
void flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    s_gfx->startWrite();
    s_gfx->setAddrWindow(area->x1, area->y1, w, h);
    s_gfx->pushPixels((lgfx::swap565_t*)&pixels->full, w * h);
    s_gfx->endWrite();
    handheld::displayFlushed(millis());
    lv_disp_flush_ready(drv);
}
} // namespace

bool Light_LP5814::writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(LP5814_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool Light_LP5814::init(uint8_t brightness) {
    bool ok = writeReg(REG_DEVICE_CONFIG0, 0x01);  // chip enable
    ok = writeReg(REG_MAX_CURRENT, 0x01) && ok;
    ok = writeReg(REG_ENABLE, 0x00) && ok;
    ok = writeReg(REG_DIM_MODE, 0x4E) && ok;
    ok = writeReg(REG_ENGINE_MODE, 0xF0) && ok;
    for (uint8_t channel = 0; channel < 4; ++channel)
        ok = writeReg(REG_LED0_DC + channel, LED_DC) && ok;
    ok = writeReg(REG_ENABLE, 0x0F) && ok;
    ok = writeReg(REG_UPDATE, 0x55) && ok;  // latch configuration
    if (!ok) Serial.printf("[DISPLAY] LP5814 backlight not responding at 0x%02X\n", LP5814_ADDR);
    delay(5);
    setBrightness(brightness);
    return ok;
}

void Light_LP5814::setBrightness(uint8_t brightness) {
    for (uint8_t channel = 0; channel < 4; ++channel)
        writeReg(REG_LED0_PWM + channel, brightness);
}

bool Display::begin() {
    if (!_gfx.init()) return false;
    _gfx.setRotation(TFT_ROTATION);  // Landscape: 320x240
    _gfx.setBrightness(0);
    _gfx.fillScreen(TFT_BLACK);
    Serial.printf("[DISPLAY] NV3031B initialized: %dx%d\n", _gfx.width(), _gfx.height());
    return true;
}

bool Display::beginLVGL() {
    s_gfx = &_gfx;
    handheld_lvgl_failure_display(handheld::showLvglFailure<LGFX_WioL2>, s_gfx);
    lv_init();

    s_buf1 = (lv_color_t*)heap_caps_malloc(BufferPixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_buf2 = (lv_color_t*)heap_caps_malloc(BufferPixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_buf1) s_buf1 = (lv_color_t*)ps_malloc(BufferPixels * sizeof(lv_color_t));
    if (!s_buf2) s_buf2 = (lv_color_t*)ps_malloc(BufferPixels * sizeof(lv_color_t));
    if (!s_buf1 || !s_buf2) {
        Serial.println("[LVGL] FATAL: buffer allocation failed!");
        heap_caps_free(s_buf1);
        heap_caps_free(s_buf2);
        s_buf1 = s_buf2 = nullptr;
        return false;
    }

    static lv_disp_draw_buf_t drawBuf;
    lv_disp_draw_buf_init(&drawBuf, s_buf1, s_buf2, BufferPixels);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = TFT_WIDTH;
    driver.ver_res = TFT_HEIGHT;
    driver.flush_cb = flush;
    driver.draw_buf = &drawBuf;
    if (!lv_disp_drv_register(&driver)) {
        Serial.println("[LVGL] FATAL: display registration failed!");
        heap_caps_free(s_buf1);
        heap_caps_free(s_buf2);
        s_buf1 = s_buf2 = nullptr;
        return false;
    }
    Serial.println("[LVGL] Display driver registered (320x240, double-buffered 20-line)");
    return true;
}

void Display::setBrightness(uint8_t level) {
    _level = level;
    if (!_asleep) _gfx.setBrightness(level);
}

void Display::sleep() {
    _gfx.sleep();
    _asleep = true;
}

void Display::wakeup() {
    if (!_asleep) return;
    // NV3031B resets and re-initializes on wake, losing panel RAM. Repaint the
    // whole frame while the backlight is dark, then restore the level.
    _gfx.setBrightness(0);
    _gfx.wakeup();
    _asleep = false;
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
    lv_refr_now(nullptr);
    _gfx.setBrightness(_level);
}
