#include "hal/Display.h"
#include <Wire.h>
#include <lvgl.h>
#include <sdkconfig.h>
#if !CONFIG_SPIRAM_MODE_OCT || CONFIG_SPIRAM_SPEED != 120
#error "CrowPanel requires Elecrow octal PSRAM 120 MHz SDK"
#endif
#include "LvFailureDisplay.h"
#include "runtime/RuntimeMetrics.h"
namespace {
LGFX_Board* panel=nullptr;
bool command(uint8_t value) {
 Wire.beginTransmission(0x30); Wire.write(value); return Wire.endTransmission()==0;
}
bool present(uint8_t address) {
 Wire.beginTransmission(address); return Wire.endTransmission()==0;
}
void flush(lv_disp_drv_t* driver,const lv_area_t* area,lv_color_t* pixels) {
 panel->pushImage(area->x1,area->y1,area->x2-area->x1+1,area->y2-area->y1+1,
                  reinterpret_cast<lgfx::rgb565_t*>(pixels));
 handheld::displayFlushed(millis()); lv_disp_flush_ready(driver);
}
}
bool Display::begin() {
 delay(50);
 bool ready=false;
 for (unsigned attempt=0;attempt<5;++attempt) {
  if (present(0x30) && (present(0x5D) || present(0x14))) { ready=true; break; }
  command(250);
  digitalWrite(TOUCH_INT,LOW); pinMode(TOUCH_INT,OUTPUT); delay(120);
  pinMode(TOUCH_INT,INPUT); delay(100);
 }
 if (!ready) { Serial.println("[CROW] Board controller/touch unavailable"); return false; }
 command(245); // V1.2/V1.3 inverted backlight, keep dark during framebuffer setup.
 if (!_gfx.init()) return false;
 _gfx.setRotation(0); _gfx.fillScreen(TFT_BLACK);
 Serial.printf("[CROW] RGB 800x480, SRAM bounce buffers (2 x %u lines), PSRAM 120 MHz, GT911 ready\n",
               unsigned(CrowPanelRgbBus::BounceLines));
 return true;
}
bool Display::beginLVGL() {
 panel=&_gfx;
 handheld_lvgl_failure_display(handheld::showLvglFailure<LGFX_Board>,panel);
 lv_init();
 // Larger render strips reduce flush overhead without using internal SRAM.
 // These PSRAM draw buffers are separate from the RGB driver's bounce buffers.
 constexpr size_t count=TFT_WIDTH*40;
 auto* a=static_cast<lv_color_t*>(heap_caps_malloc(count*sizeof(lv_color_t),MALLOC_CAP_SPIRAM));
 auto* b=static_cast<lv_color_t*>(heap_caps_malloc(count*sizeof(lv_color_t),MALLOC_CAP_SPIRAM));
 if (!a || !b) { heap_caps_free(a); heap_caps_free(b); return false; }
 static lv_disp_draw_buf_t buffers;
 lv_disp_draw_buf_init(&buffers,a,b,count);
 static lv_disp_drv_t driver; lv_disp_drv_init(&driver);
 driver.hor_res=TFT_WIDTH; driver.ver_res=TFT_HEIGHT;
 driver.flush_cb=flush; driver.draw_buf=&buffers;
 if (!lv_disp_drv_register(&driver)) { heap_caps_free(a); heap_caps_free(b); return false; }
 return true;
}
void Display::setBrightness(uint8_t level) {
 _brightness=level;
 command(level ? uint8_t(244-(uint32_t(level)*244/255)) : 245);
}
void Display::sleep() { command(245); }
void Display::wakeup() { setBrightness(_brightness); }
