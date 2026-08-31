#pragma once

#include "UIManager.h"
#include "config/Config.h"
#include <functional>

#include "config/Timezones.h"

class LvTimezoneScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    bool handleKey(const KeyEvent& event) override;
    void onEnter() override { _enterTime = millis(); }
    const char* title() const override { return "Timezone"; }

    void setDoneCallback(std::function<void(int tzIndex)> cb) { _doneCb = cb; }

    // Pre-select an index (e.g., from saved config)
    void setSelectedIndex(int idx) { _selectedIdx = idx; }

private:
    std::function<void(int)> _doneCb;
    int _selectedIdx = 6;  // Default: New York (EST/EDT)
    unsigned long _enterTime = 0;
    static constexpr unsigned long ENTER_GUARD_MS = 600;

    lv_obj_t* _roller = nullptr;
    lv_obj_t* _upButton = nullptr;
    lv_obj_t* _downButton = nullptr;
    lv_obj_t* _doneButton = nullptr;

    void stepSelection(int delta);
    void submit(bool enforceEnterGuard);
};
