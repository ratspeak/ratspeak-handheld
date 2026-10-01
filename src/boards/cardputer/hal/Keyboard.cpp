#include "Keyboard.h"
#include "voice/VoiceInput.h"
#include "config/BoardConfig.h"

static constexpr unsigned long KEY_REPEAT_START_MS = 400;
static constexpr unsigned long KEY_REPEAT_INTERVAL_MS = 125;

namespace {
bool hasAction(const KeyEvent& event) {
    return event.character != 0 || event.enter || event.backspace ||
           event.forwardDelete || event.tab || event.space || event.escape ||
           event.up || event.down || event.left || event.right;
}

bool isRepeatable(const KeyEvent& event) {
    return event.backspace || event.forwardDelete ||
           event.up || event.down || event.left || event.right;
}

bool isModifierPosition(uint8_t row, uint8_t col) {
    return (row == 2 && (col == 0 || col == 1)) ||
           (row == 3 && col <= 2);
}
}  // namespace

void Keyboard::begin() {
    _mode = InputMode::Navigation;
    _hasEvent = false;
    memset(_pressed, 0, sizeof(_pressed));
    _hardware.begin();
}

KeyEvent Keyboard::eventForKey(uint8_t row, uint8_t col) const {
    KeyEvent event = {};
    if (row >= 4 || col >= 14 || isModifierPosition(row, col)) return event;

    event.fn = _pressed[2][0];
    event.shift = _pressed[2][1];
    event.ctrl = _pressed[3][0];
    event.opt = _pressed[3][1];
    event.alt = _pressed[3][2];

    // The Cardputer ADV Fn layer has precedence over printable characters.
    if (event.fn) {
        if (row == 0 && col == 0) event.escape = true;
        else if (row == 0 && col == 13) event.forwardDelete = true;
        else if (row == 2 && col == 11) event.up = true;
        else if (row == 3 && col == 10) event.left = true;
        else if (row == 3 && col == 11) event.down = true;
        else if (row == 3 && col == 12) event.right = true;
        return event;
    }

    Point2D_t position;
    position.x = col;
    position.y = row;
    const KeyValue_t value = M5Cardputer.Keyboard.getKeyValue(position);
    const uint8_t base = static_cast<uint8_t>(value.value_first);

    if (base == KEY_BACKSPACE) event.backspace = true;
    else if (base == KEY_TAB) event.tab = true;
    else if (base == KEY_ENTER) event.enter = true;
    else {
        const char character = (event.shift || _capsLocked)
                                   ? value.value_second
                                   : value.value_first;
        if (character >= 32 && character < 127) {
            event.character = character;
            event.space = character == ' ';
        }
    }
    return event;
}

void Keyboard::update() {
    _hasEvent = false;
    const unsigned long now = millis();

    // Emit every physical press in FIFO order. A held earlier key must neither
    // hide a new key nor be re-emitted when that newer key is released.
    for (size_t i = 0; i < 16; ++i) {
        CardputerAdvKeyboard::Event raw;
        if (!_hardware.poll(&raw, 1)) break;
        _pressed[raw.row][raw.col] = raw.pressed;
        Point2D_t position;position.x=raw.col;position.y=raw.row;
        if(M5Cardputer.Keyboard.getKeyValue(position).value_first==' ')
            handheld::voice::VoiceInput::instance().edge(handheld::voice::VoiceInput::Key,raw.pressed);
        if (!raw.pressed || isModifierPosition(raw.row, raw.col)) continue;
        KeyEvent candidate = eventForKey(raw.row, raw.col);
        if (!hasAction(candidate)) continue;
        _event = candidate;
        _hasEvent = true;
        _keyHeld = isRepeatable(candidate);
        _heldRow = raw.row; _heldCol = raw.col;
        _heldSince = _lastRepeat = now;
        if (_keyCallback) _keyCallback(_event);
        return;
    }

    if (!_keyHeld || !_pressed[_heldRow][_heldCol]) { _keyHeld = false; return; }
    const auto next = eventForKey(_heldRow, _heldCol);
    // Releasing Fn stops arrow/delete repetition; modifiers never synthesize
    // a printable press for a key that was already held.
    if (!isRepeatable(next) || next.backspace != _event.backspace ||
        next.forwardDelete != _event.forwardDelete || next.up != _event.up ||
        next.down != _event.down || next.left != _event.left || next.right != _event.right) {
        _keyHeld = false;
        return;
    }
    if (now - _heldSince < KEY_REPEAT_START_MS || now - _lastRepeat < KEY_REPEAT_INTERVAL_MS) return;
    _lastRepeat = now;
    _event = next;
    _event.repeat = true;
    _hasEvent = true;
    if (_keyCallback) _keyCallback(_event);
}

bool Keyboard::capsLocked() const {
    return _capsLocked;
}

void Keyboard::setCapsLocked(bool locked) {
    _capsLocked = locked;
}

void Keyboard::discardPending() {
    handheld::voice::VoiceInput::instance().cancel();
    for (unsigned i = 0; i < 16; ++i) {
        CardputerAdvKeyboard::Event raw;
        if (!_hardware.poll(&raw, 1)) break;
        _pressed[raw.row][raw.col] = raw.pressed;
        Point2D_t position;position.x=raw.col;position.y=raw.row;
        if(M5Cardputer.Keyboard.getKeyValue(position).value_first==' ')
            handheld::voice::VoiceInput::instance().edge(handheld::voice::VoiceInput::Key,raw.pressed);
    }
    _keyHeld = false;
    _hasEvent = false;
}
