#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include "config/BoardConfig.h"
#include "hal/IOExpander.h"

// ST7701 480x480 RGB/DPI panel. Bus/panel timing (hsync/vsync/pclk, the
// d0-d15 pin map) is real, confirmed hardware configuration taken from an
// already-completed port of this exact device (meshcore-dev, MIT-licensed
// — values reused, no code copied). Unlike the SPI-bus displays elsewhere
// in this codebase, the RGB bus has its own dedicated pins (d0-d15, pclk,
// hsync, vsync, de) entirely separate from the radio's SPI pins (SCK/MOSI)
// — so once boot-time panel init is done, there is no ongoing contention
// to arbitrate, unlike T-Pager/T-Deck's shared-SPI displays.
class LGFX_Board : public lgfx::LGFX_Device {
    lgfx::Panel_ST7701 _panel;
    lgfx::Bus_RGB _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_Board() {
        {
            auto cfg = _panel.config();
            cfg.memory_width = DISPLAY_WIDTH;
            cfg.memory_height = DISPLAY_HEIGHT;
            cfg.panel_width = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 2;
            _panel.config(cfg);
        }
        {
            // The panel's real CS/RESET are expander-routed (see
            // Display::begin()'s pre-init step, which asserts CS low over
            // I2C before this bit-banged init sequence runs) — LovyanGFX
            // itself only drives real GPIOs, so pin_cs is NC here, not a
            // placeholder value.
            auto cfg = _panel.config_detail();
            cfg.pin_cs = GPIO_NUM_NC;
            cfg.pin_sclk = SPI_SCK;
            cfg.pin_mosi = SPI_MOSI;
            cfg.use_psram = 1;
            _panel.config_detail(cfg);
        }
        {
            auto cfg = _bus.config();
            cfg.panel = &_panel;
            cfg.freq_write = 8000000;
            cfg.pin_henable = 18;
            cfg.pin_pclk = 21;
            cfg.pclk_active_neg = 0;
            cfg.pclk_idle_high = 0;
            cfg.de_idle_high = 1;
            cfg.pin_hsync = 16;
            cfg.hsync_polarity = 0;
            cfg.hsync_front_porch = 10;
            cfg.hsync_pulse_width = 8;
            cfg.hsync_back_porch = 50;
            cfg.pin_vsync = 17;
            cfg.vsync_polarity = 0;
            cfg.vsync_front_porch = 10;
            cfg.vsync_pulse_width = 8;
            cfg.vsync_back_porch = 20;
            cfg.pin_d0 = 15; cfg.pin_d1 = 14; cfg.pin_d2 = 13; cfg.pin_d3 = 12;
            cfg.pin_d4 = 11; cfg.pin_d5 = 10; cfg.pin_d6 = 9;  cfg.pin_d7 = 8;
            cfg.pin_d8 = 7;  cfg.pin_d9 = 6;  cfg.pin_d10 = 5; cfg.pin_d11 = 4;
            cfg.pin_d12 = 3; cfg.pin_d13 = 2; cfg.pin_d14 = 1; cfg.pin_d15 = 0;
            _bus.config(cfg);
        }
        _panel.setBus(&_bus);
        {
            auto cfg = _light.config();
            cfg.pin_bl = 45;
            _light.config(cfg);
        }
        _panel.setLight(&_light);
        setPanel(&_panel);
    }
};

class Display {
public:
    // The IO expander is one physical chip shared with the radio (CS/RESET
    // for this panel are bits 4/5; the radio uses bits 0-3) — must be the
    // *same* IOExpander instance the radio uses, not a second one, or their
    // independent shadow register copies will silently clobber each other's
    // pin state. Call before begin().
    void setExpander(IOExpander* expander) { _expander = expander; }
    bool begin();
    bool beginLVGL();
    void setBrightness(uint8_t level);
    void sleep();
    void wakeup();

    LGFX_Board& gfx() { return _gfx; }

private:
    LGFX_Board _gfx;
    IOExpander* _expander = nullptr;
};
