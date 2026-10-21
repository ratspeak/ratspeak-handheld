#include "Display.h"
#include <lvgl.h>
#include "runtime/RuntimeMetrics.h"
#include "LvFailureDisplay.h"

static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;
static LGFX_Board* s_gfx = nullptr;

static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    s_gfx->startWrite();
    s_gfx->setAddrWindow(area->x1, area->y1, w, h);
    s_gfx->pushPixels((lgfx::swap565_t*)&color_p->full, w * h);
    s_gfx->endWrite();
    handheld::displayFlushed(millis());
    lv_disp_flush_ready(drv);
}

bool Display::begin() {
    if (!_expander) return false;
    // Confirmed-necessary fix (meshcore-dev's own hardest-won bug on this
    // exact board): the panel's real CS line must be asserted low before
    // its bit-banged init command sequence runs, not just released high
    // afterward — otherwise the panel only receives init commands by
    // electrical coincidence, which fails specifically across a real power
    // cycle (the expander chip's own output register retains state across
    // a warm MCU-only reset, masking the bug until a genuine cold boot).
    _expander->pinMode(TFT_CS_EXPANDER_BIT, OUTPUT);
    _expander->digitalWrite(TFT_CS_EXPANDER_BIT, LOW);
    if (!_gfx.init()) return false;
    _expander->digitalWrite(TFT_CS_EXPANDER_BIT, HIGH);

    _gfx.setBrightness(0);
    _gfx.fillScreen(TFT_BLACK);
    Serial.printf("[DISPLAY] Initialized: %dx%d (LovyanGFX RGB/DPI)\n", _gfx.width(), _gfx.height());
    return true;
}

bool Display::beginLVGL() {
    s_gfx = &_gfx;
    handheld_lvgl_failure_display(handheld::showLvglFailure<LGFX_Board>, s_gfx);

    lv_init();

    const uint32_t bufSize = DISPLAY_WIDTH * 20;
    s_buf1 = (lv_color_t*)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_buf2 = (lv_color_t*)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_buf1) s_buf1 = (lv_color_t*)ps_malloc(bufSize * sizeof(lv_color_t));
    if (!s_buf2) s_buf2 = (lv_color_t*)ps_malloc(bufSize * sizeof(lv_color_t));
    if (!s_buf1 || !s_buf2) {
        Serial.println("[LVGL] FATAL: buffer allocation failed!");
        heap_caps_free(s_buf1); heap_caps_free(s_buf2);
        s_buf1 = s_buf2 = nullptr;
        return false;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf1, s_buf2, bufSize);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    if (!lv_disp_drv_register(&disp_drv)) {
        Serial.println("[LVGL] FATAL: display registration failed!");
        heap_caps_free(s_buf1); heap_caps_free(s_buf2);
        s_buf1 = s_buf2 = nullptr;
        return false;
    }

    Serial.printf("[LVGL] Display driver registered (%dx%d, double-buffered 20-line)\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

void Display::setBrightness(uint8_t level) { _gfx.setBrightness(level); }
void Display::sleep() { _gfx.sleep(); }
void Display::wakeup() { _gfx.wakeup(); }
