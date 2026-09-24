#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "config/BoardConfig.h"

// Shared ST7789 configuration; electrical differences belong to BoardConfig.
// Hardware panel is ST7789V (240x320)
class LGFX_Board : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_Board() {
        // SPI bus config
        auto cfg_bus = _bus.config();
        cfg_bus.spi_host = SPI2_HOST;
        cfg_bus.spi_mode = 0;
        cfg_bus.freq_write = TFT_SPI_FREQ;
        cfg_bus.freq_read = 16000000;
        cfg_bus.pin_sclk = SPI_SCK;
        cfg_bus.pin_miso = SPI_MISO;
        cfg_bus.pin_mosi = SPI_MOSI;
        cfg_bus.pin_dc = TFT_DC;
        _bus.config(cfg_bus);
        _panel.setBus(&_bus);

        // Panel config — native orientation is 240x320 portrait
        auto cfg_panel = _panel.config();
        cfg_panel.pin_cs = TFT_CS;
        cfg_panel.pin_rst = TFT_RST;
        cfg_panel.panel_width = 240;
        cfg_panel.panel_height = 320;
        cfg_panel.offset_x = 0;
        cfg_panel.offset_y = 0;
        cfg_panel.invert = true;
        cfg_panel.rgb_order = false;
        cfg_panel.memory_width = 240;
        cfg_panel.memory_height = 320;
        _panel.config(cfg_panel);

        // Backlight config
        auto cfg_light = _light.config();
        cfg_light.pin_bl = TFT_BL;
        cfg_light.invert = TFT_BL_INVERT;
        cfg_light.freq = TFT_PWM_FREQ;
        cfg_light.pwm_channel = 0;
        _light.config(cfg_light);
        _panel.setLight(&_light);

        setPanel(&_panel);
    }
};

class Display {
public:
    bool begin();

    // Initialize LVGL display driver with double-buffered DMA flush
    bool beginLVGL();

    // Backlight
    void setBrightness(uint8_t level);
    void sleep();
    void wakeup();

    LGFX_Board& gfx() { return _gfx; }

private:
    LGFX_Board _gfx;
};
