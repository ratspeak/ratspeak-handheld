#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/panel/Panel_NV3031B.hpp>
#include "config/BoardConfig.h"

// LP5814 four-channel LED driver used as the panel backlight. All channels
// carry the same PWM value.
class Light_LP5814 : public lgfx::v1::ILight {
public:
    bool init(uint8_t brightness) override;
    void setBrightness(uint8_t brightness) override;
private:
    bool writeReg(uint8_t reg, uint8_t value);
};

// NV3031B 240x320 on quad SPI: four data lines, no DC pin, SPI mode 3. Panel
// reset and power are expander bits, so pin_rst stays -1.
class LGFX_WioL2 : public lgfx::LGFX_Device {
    lgfx::Panel_NV3031B _panel;
    lgfx::Bus_SPI _bus;
    Light_LP5814 _light;

public:
    LGFX_WioL2() {
        auto bus = _bus.config();
        bus.spi_host = TFT_SPI_HOST;
        bus.spi_mode = TFT_SPI_MODE;
        bus.freq_write = TFT_SPI_FREQ;
        bus.freq_read = 16000000;
        bus.spi_3wire = false;
        bus.use_lock = true;
        bus.pin_sclk = TFT_SPI_SCK;
        bus.pin_miso = -1;
        bus.pin_mosi = -1;
        bus.pin_dc = -1;
        // Setting all four IO pins selects quad mode in Bus_SPI.
        bus.pin_io0 = TFT_QSPI_IO0;
        bus.pin_io1 = TFT_QSPI_IO1;
        bus.pin_io2 = TFT_QSPI_IO2;
        bus.pin_io3 = TFT_QSPI_IO3;
        _bus.config(bus);
        _panel.setBus(&_bus);

        auto panel = _panel.config();
        panel.pin_cs = TFT_CS;
        panel.pin_rst = -1;
        panel.pin_busy = -1;
        panel.panel_width = 240;
        panel.panel_height = 320;
        panel.memory_width = 240;
        panel.memory_height = 320;
        panel.offset_x = 0;
        panel.offset_y = 0;
        panel.offset_rotation = TFT_PANEL_OFFSET_ROTATION;
        panel.invert = true;
        panel.rgb_order = true;
        panel.dlen_16bit = false;
        panel.bus_shared = false;
        panel.readable = false;
        _panel.config(panel);
        _panel.setLight(&_light);

        setPanel(&_panel);
    }
};

class Display {
public:
    bool begin();
    bool beginLVGL();

    // Backlight level 0-255. While asleep it is stored and applied on wake.
    void setBrightness(uint8_t level);
    void sleep();
    void wakeup();

    LGFX_WioL2& gfx() { return _gfx; }

private:
    LGFX_WioL2 _gfx;
    uint8_t _level = 0;
    bool _asleep = false;
};
