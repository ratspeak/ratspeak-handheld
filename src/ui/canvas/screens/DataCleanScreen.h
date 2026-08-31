#pragma once

#include "Screen.h"
#include <functional>

class DataCleanScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Setup"; }

    // Callback: true = user chose Yes (wipe), false = user chose No (skip)
    void setDoneCallback(std::function<void(bool)> cb) { _doneCb = cb; }

    // Replace buttons with a status message
    void setStatus(const char* msg);

private:
    bool _selectedYes = true;
    const char* _status = nullptr;
    std::function<void(bool)> _doneCb;
};
