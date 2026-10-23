#pragma once
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include "config/BoardConfig.h"
#include "RgbBus.h"
class LGFX_Board : public lgfx::LGFX_Device {
 CrowPanelRgbBus _bus;
 lgfx::Panel_RGB _panel;
public:
 LGFX_Board() {
  auto p=_panel.config();
  p.memory_width=p.panel_width=TFT_WIDTH;
  p.memory_height=p.panel_height=TFT_HEIGHT;
  _panel.config(p);
  auto d=_panel.config_detail(); d.use_psram=1; _panel.config_detail(d);
  auto b=_bus.config(); b.panel=&_panel;
  b.pin_d0=21; b.pin_d1=47; b.pin_d2=48; b.pin_d3=45; b.pin_d4=38;
  b.pin_d5=9; b.pin_d6=10; b.pin_d7=11; b.pin_d8=12; b.pin_d9=13; b.pin_d10=14;
  b.pin_d11=7; b.pin_d12=17; b.pin_d13=18; b.pin_d14=3; b.pin_d15=46;
  b.pin_henable=42; b.pin_vsync=41; b.pin_hsync=40; b.pin_pclk=39;
  b.freq_write=16000000; // Conservative pixel clock, independent of 120 MHz PSRAM.
  b.hsync_polarity=0; b.hsync_front_porch=8; b.hsync_pulse_width=4; b.hsync_back_porch=8;
  b.vsync_polarity=0; b.vsync_front_porch=8; b.vsync_pulse_width=4; b.vsync_back_porch=8;
  b.pclk_idle_high=1;
  _bus.config(b); _panel.setBus(&_bus); setPanel(&_panel);
 }
};
class Display {
public:
 bool begin(); bool beginLVGL();
 void setBrightness(uint8_t level); void sleep(); void wakeup();
 LGFX_Board& gfx() { return _gfx; }
private:
 LGFX_Board _gfx;
 uint8_t _brightness=128;
};
