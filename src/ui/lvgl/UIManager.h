#pragma once

#include <lvgl.h>
#include "LvStatusBar.h"
#include "LvTabBar.h"
#include "hal/Keyboard.h"

// What a screen's current editor needs from an on-screen keyboard. Anchor is
// the widget that must stay visible above it.
enum class TextInputMode : uint8_t { None, Text, Adjust };
struct TextInputRequest {
    TextInputMode mode = TextInputMode::None;
    lv_obj_t* anchor = nullptr;
};

// LVGL screen base class
class LvScreen {
public:
    virtual ~LvScreen() = default;
    virtual void createUI(lv_obj_t* parent) = 0;
    virtual void destroyUI();
    virtual void refreshUI() {}
    virtual void onEnter() {}
    virtual void onExit() {}
    virtual bool handleKey(const KeyEvent& event) { return false; }
    virtual bool handleLongPress() { return false; }
    // Boards without a keyboard show an on-screen one while this is active.
    virtual TextInputRequest textInput() const { return {}; }
    virtual const char* title() const = 0;

    lv_obj_t* screen() const { return _screen; }

protected:
    lv_obj_t* _screen = nullptr;
};

class UIManager {
public:
    void begin();

    // Screen management
    void setScreen(LvScreen* screen);
    LvScreen* getScreen() { return _currentLvScreen; }

    // Component access
    LvStatusBar& lvStatusBar() { return _lvStatusBar; }
    LvTabBar& lvTabBar() { return _lvTabBar; }

    // Update data (called periodically)
    void update();

    // Force full redraw
    void forceRedraw();

    // Re-apply palette to shared styles and persistent shell after a theme switch
    void applyTheme();

    // Handle key event — routes to current screen
    bool handleKey(const KeyEvent& event);
    bool handleLongPress();

    // Boot mode — hides status bar and tab bar
    void setBootMode(bool boot);
    bool isBootMode() const { return _bootMode; }

    // LVGL content area parent (between status bar and tab bar)
    lv_obj_t* contentParent() { return _lvContent; }

private:
    // LVGL components
    LvStatusBar _lvStatusBar;
    LvTabBar _lvTabBar;
    LvScreen* _currentLvScreen = nullptr;
    lv_obj_t* _lvContent = nullptr;

    bool _bootMode = false;
};
