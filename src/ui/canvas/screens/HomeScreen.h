#pragma once

#include "Screen.h"
#include "radio/SX1262.h"
#include "config/UserConfig.h"
#include <functional>

class ProtocolBackend;

class HomeScreen : public Screen {
public:
    void setBackend(ProtocolBackend* backend) { _backend = backend; }
    void setRadio(SX1262* radio) { _radio = radio; }
    void setUserConfig(UserConfig* cfg) { _userConfig = cfg; }
    // Returns true if an announce was actually sent (interface admission result)
    void setAnnounceCallback(std::function<bool()> cb) { _announceCb = cb; }

    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Home"; }

private:
    ProtocolBackend* _backend = nullptr;
    SX1262* _radio = nullptr;
    UserConfig* _userConfig = nullptr;
    std::function<bool()> _announceCb;
    unsigned long _announceFlashUntil = 0;
};
