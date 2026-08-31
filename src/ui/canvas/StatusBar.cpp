#include "StatusBar.h"
#include "Theme.h"
#include <M5Unified.h>
#include <time.h>

namespace {
int clampBatteryLevel(int level) {
    if (level < 0) return 0;
    if (level > 100) return 100;
    return level;
}

int readBatteryLevel() {
    int level = M5.Power.getBatteryLevel();
    if (level >= 0) return clampBatteryLevel(level);

    int millivolts = M5.Power.getBatteryVoltage();
    if (millivolts > 0) {
        float pct = ((float)millivolts - 3300.0f) * 100.0f / (4150.0f - 3350.0f);
        return clampBatteryLevel((int)(pct + 0.5f));
    }

    return 0;
}

bool isChargingKnown() {
    return M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
}
}

void StatusBar::render(M5Canvas& canvas) {
    canvas.fillRect(0, 0, Theme::SCREEN_W, Theme::STATUS_BAR_H, Theme::BAR_BG);
    canvas.drawFastHLine(0, Theme::STATUS_BAR_H - 1, Theme::SCREEN_W, Theme::DIVIDER);

    // Left side: battery icon + rotating status text.
    unsigned long now = millis();
    if (_smoothedBattery < 0 || now - _lastBatteryRead >= 2000) {
        int raw = readBatteryLevel();
        if (_smoothedBattery < 0) {
            _smoothedBattery = (float)raw;
        } else {
            _smoothedBattery = _smoothedBattery * 0.85f + (float)raw * 0.15f;
        }
        _lastBatteryRead = now;
    }
    int batLevel = clampBatteryLevel((int)(_smoothedBattery + 0.5f));
    bool charging = isChargingKnown();

    Theme::useSmallFont(canvas);

    int bx = _wifiEnabled ? 21 : 3, by = 3, bw = 14, bh = 7;
    if (_wifiEnabled) {
        const uint16_t color = _wifiActive ? Theme::SUCCESS : Theme::WARNING;
        canvas.drawLine(2, 5, 5, 2, color);
        canvas.drawFastHLine(5, 2, 6, color);
        canvas.drawLine(10, 2, 13, 5, color);
        canvas.drawLine(4, 7, 6, 5, color);
        canvas.drawFastHLine(6, 5, 4, color);
        canvas.drawLine(9, 5, 11, 7, color);
        canvas.fillCircle(8, 9, 1, color);
        if (!_wifiActive) canvas.drawLine(2, 11, 13, 1, color);
    }
    uint16_t batColor = Theme::SUCCESS;
    if (batLevel <= 10) batColor = Theme::ERROR;
    else if (batLevel <= 30) batColor = Theme::WARNING;
    canvas.drawRect(bx, by, bw, bh, batColor);
    canvas.fillRect(bx + bw, by + 2, 2, 3, batColor);
    int fillW = (bw - 2) * batLevel / 100;
    if (fillW > 0) canvas.fillRect(bx + 1, by + 1, fillW, bh - 2, batColor);
    if (charging) {
        canvas.drawLine(bx + 7, by + 1, bx + 5, by + 4, Theme::BG);
        canvas.drawLine(bx + 5, by + 4, bx + 8, by + 4, Theme::BG);
        canvas.drawLine(bx + 8, by + 4, bx + 6, by + 7, Theme::BG);
    }

    char batStr[6];
    snprintf(batStr, sizeof(batStr), "%d%%", batLevel);

    int textX = bx + bw + 5;
    time_t t = time(nullptr);
    if (t > 1700000000) {
        struct tm* tm = localtime(&t);
        char clockStr[8];
        snprintf(clockStr, sizeof(clockStr), "%d:%02d", tm->tm_hour, tm->tm_min);
        bool showClock = ((now / 5000UL) % 2UL) == 1UL;
        canvas.setTextColor(showClock ? Theme::TEXT_PRIMARY : (charging ? Theme::ACCENT : batColor));
        canvas.setCursor(textX, Theme::SHELL_TEXT_Y);
        canvas.print(showClock ? clockStr : batStr);
    } else {
        canvas.setTextColor(charging ? Theme::ACCENT : batColor);
        canvas.setCursor(textX, Theme::SHELL_TEXT_Y);
        canvas.print(batStr);
    }

    // Center text — flash "ANNOUNCED" briefly, otherwise show mode
    const char* centerText = _transportMode;
    uint16_t centerColor = Theme::ACCENT;
    if (millis() < _announceFlashUntil) {
        centerText = "ANNOUNCED";
        centerColor = Theme::PRIMARY;
    }
    canvas.setTextColor(centerColor);
    int modeLen = canvas.textWidth(centerText);
    canvas.setCursor((Theme::SCREEN_W - modeLen) / 2, Theme::SHELL_TEXT_Y);
    canvas.print(centerText);

    // Connection indicators (right) — LoRa + TCP + AutoIface peers
    int rx = Theme::SCREEN_W - 4;

    // TCP indicator
    if (_tcpConnected) {
        const char* tcpStr = "TCP";
        int tcpW = canvas.textWidth(tcpStr);
        rx -= tcpW;
        canvas.setTextColor(Theme::ACCENT);
        canvas.setCursor(rx, Theme::SHELL_TEXT_Y);
        canvas.print(tcpStr);
        rx -= 4;
    }

    // AutoInterface peers indicator (only shown when at least one peer)
    if (_autoIfacePeers > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "L:%d", _autoIfacePeers);
        int w = canvas.textWidth(buf);
        rx -= w;
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(rx, Theme::SHELL_TEXT_Y);
        canvas.print(buf);
        rx -= 4;
    }

    // LoRa indicator
    const char* loraStr = _loraOnline ? "LoRa" : "----";
    uint16_t loraColor = _loraOnline ? Theme::SUCCESS : Theme::MUTED;
    int loraW = canvas.textWidth(loraStr);
    rx -= loraW;
    canvas.fillCircle(rx - 5, Theme::STATUS_BAR_H / 2, 2, loraColor);
    canvas.setTextColor(loraColor);
    canvas.setCursor(rx, Theme::SHELL_TEXT_Y);
    canvas.print(loraStr);
    Theme::useSmallFont(canvas);
}
