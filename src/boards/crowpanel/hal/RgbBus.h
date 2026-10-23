#pragma once

#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <sdkconfig.h>

#if !CONFIG_SPIRAM_FETCH_INSTRUCTIONS || !CONFIG_SPIRAM_RODATA || !CONFIG_ESP32S3_DATA_CACHE_LINE_64B
#error "CrowPanel RGB bounce buffers require PSRAM XIP and 64-byte cache lines"
#endif

// Keep LovyanGFX's drawing API and framebuffer format, but let ESP-IDF feed
// the LCD through internal SRAM. Direct PSRAM DMA is vulnerable to bandwidth
// spikes during flash writes (including saving the keyboard language).
class CrowPanelRgbBus : public lgfx::Bus_RGB {
public:
 static constexpr size_t BounceLines=20;
 bool init() override {
  if (_handle) return true;
  const auto& bus=config();
  esp_lcd_rgb_panel_config_t rgb{};
  rgb.clk_src=LCD_CLK_SRC_DEFAULT;
  rgb.data_width=16;
  rgb.bits_per_pixel=16;
  rgb.num_fbs=1;
  rgb.bounce_buffer_size_px=bus.panel->width()*BounceLines;
  rgb.sram_trans_align=4;
  rgb.psram_trans_align=64;
  rgb.hsync_gpio_num=bus.pin_hsync;
  rgb.vsync_gpio_num=bus.pin_vsync;
  rgb.de_gpio_num=bus.pin_henable;
  rgb.pclk_gpio_num=bus.pin_pclk;
  rgb.disp_gpio_num=-1;
  // Panel_RGB stores RGB565 in byte-swapped order. Match the original
  // LovyanGFX Bus_RGB signal routing rather than swapping every flush.
  for (int i=0;i<16;++i) rgb.data_gpio_nums[i]=bus.pin_data[i ^ 8];
  rgb.timings.pclk_hz=bus.freq_write;
  rgb.timings.h_res=bus.panel->width();
  rgb.timings.v_res=bus.panel->height();
  rgb.timings.hsync_pulse_width=bus.hsync_pulse_width;
  rgb.timings.hsync_back_porch=bus.hsync_back_porch;
  rgb.timings.hsync_front_porch=bus.hsync_front_porch;
  rgb.timings.vsync_pulse_width=bus.vsync_pulse_width;
  rgb.timings.vsync_back_porch=bus.vsync_back_porch;
  rgb.timings.vsync_front_porch=bus.vsync_front_porch;
  rgb.timings.flags.hsync_idle_low=!bus.hsync_polarity;
  rgb.timings.flags.vsync_idle_low=!bus.vsync_polarity;
  rgb.timings.flags.de_idle_high=bus.de_idle_high;
  // Original Bus_RGB uses pclk_idle_high for the output edge, with idle low.
  rgb.timings.flags.pclk_active_neg=bus.pclk_idle_high;
  rgb.timings.flags.pclk_idle_high=0;
  rgb.flags.fb_in_psram=true;
  // Do not invalidate cache lines while the UI may be updating the framebuffer.
  rgb.flags.bb_invalidate_cache=false;
  esp_err_t err=esp_lcd_new_rgb_panel(&rgb,&_handle);
  if (err==ESP_OK) err=esp_lcd_rgb_panel_get_frame_buffer(_handle,1,&_frame);
  if (err==ESP_OK) err=esp_lcd_panel_reset(_handle);
  if (err==ESP_OK) err=esp_lcd_panel_init(_handle);
  if (err!=ESP_OK) {
   ESP_LOGE("CrowPanelRGB","RGB initialization failed: %s",esp_err_to_name(err));
   release();
   return false;
  }
  return true;
 }
 uint8_t* getDMABuffer(uint32_t) override { return static_cast<uint8_t*>(_frame); }
 void release() override {
  if (_handle) esp_lcd_panel_del(_handle);
  _handle=nullptr;
  _frame=nullptr;
 }
private:
 esp_lcd_panel_handle_t _handle=nullptr;
 void* _frame=nullptr;
};
