#include "UIManager.h"
#include "voice/VoiceInput.h"
#include "util/PerfTrace.h"

bool UIManager::begin() {
    // RGB332: 240x135 one-byte pixels plus the driver's sentinel byte.
    _canvas.setColorDepth(8);
    if (!_canvas.createSprite(Theme::SCREEN_W, Theme::SCREEN_H)) return false;
    Serial.printf("[UI] Canvas: RGB332, %d bytes\n", Theme::SCREEN_W * Theme::SCREEN_H);
    Theme::useSmallFont(_canvas);
    _canvas.fillScreen(Theme::BG);
    _needsRender = true;
    _statusDirty = true;
    _contentDirty = true;
    _tabDirty = true;

    // Wire up dirty flag callbacks
    _statusBar.setDirtyFlag(&_statusDirty);
    _tabBar.setDirtyFlag(&_tabDirty);
    return true;
}

void UIManager::setScreen(Screen* screen) {
    handheld::voice::VoiceInput::instance().cancel();
    if(_voicePanel && _voicePanel->visible()) _voicePanel->hide();
    unsigned long traceStart = PerfTrace::nowMs();
    const char* fromTitle = _currentScreen ? _currentScreen->title() : "none";
    const char* toTitle = screen ? screen->title() : "none";

    if (_currentScreen) {
        _currentScreen->onExit();
    }
    _currentScreen = screen;
    if (_currentScreen) {
        _currentScreen->onEnter();
    }
    markAllDirty();
    (void)fromTitle;
    PerfTrace::write("ui", "screen_transition", toTitle, 0, traceStart, true,
                     RSDECK_PERF_UI_TRACE_MS);
}

void UIManager::render() {
    if (_bootMode) {
        // Boot mode: always render full screen
        _canvas.fillScreen(Theme::BG);
        Theme::useSmallFont(_canvas);
        if (_currentScreen) {
            _currentScreen->render(_canvas);
        }
        flush();
        return;
    }

    // Skip render if nothing changed
    if (!_statusDirty && !_contentDirty && !_tabDirty) return;

    // Full canvas redraw (M5Canvas doesn't support partial push)
    _canvas.fillScreen(Theme::BG);
    Theme::useSmallFont(_canvas);
    _statusBar.render(_canvas);

    if (_currentScreen) {
        _canvas.setClipRect(0, Theme::CONTENT_Y, Theme::CONTENT_W, Theme::CONTENT_H);
        Theme::useSmallFont(_canvas);
        _currentScreen->render(_canvas);
        _canvas.clearClipRect();
    }

    Theme::useSmallFont(_canvas);
    _tabBar.render(_canvas);

    if (_overlay) {
        _canvas.setClipRect(0, Theme::CONTENT_Y, Theme::CONTENT_W, Theme::CONTENT_H);
        Theme::useSmallFont(_canvas);
        _overlay->render(_canvas);
        _canvas.clearClipRect();
    }

    if(_voicePanel) _voicePanel->render(_canvas);
    flush();
    _statusDirty = _contentDirty = _tabDirty = false;
}

void UIManager::flush() {
    _canvas.pushSprite(&M5.Display, 0, 0);
    handheld::displayFlushed(millis());
}

bool UIManager::handleKey(const KeyEvent& event) {
    markContentDirty();
    if(_voicePanel && _voicePanel->handleKey(event)) return true;
    if (_currentScreen) {
        return _currentScreen->handleKey(event);
    }
    return false;
}
