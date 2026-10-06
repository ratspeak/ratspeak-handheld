#pragma once

#include "UIManager.h"
#include <functional>

class LvNameInputScreen : public LvScreen {
public:
    void createUI(lv_obj_t* parent) override;
    bool handleKey(const KeyEvent& event) override;
    TextInputRequest textInput() const override {
        return {_saving ? TextInputMode::None : TextInputMode::Text, _textarea};
    }
    const char* title() const override { return "Setup"; }

    void setDoneCallback(std::function<void(const String&)> cb) { _doneCb = cb; }
    void onEnter() override { _enterTime = millis(); }

    static constexpr int MAX_NAME_LEN = 16;

    void setSaving(bool saving);

private:
    bool _saving = false;
    lv_obj_t* _saveTitle = nullptr;
    lv_obj_t* _textarea = nullptr;
    lv_obj_t* _doneButton = nullptr;
    std::function<void(const String&)> _doneCb;
    unsigned long _enterTime = 0;
    static constexpr unsigned long ENTER_GUARD_MS = 600;  // Ignore Enter for 600ms after screen appears

    void submit(bool enforceEnterGuard);
};
