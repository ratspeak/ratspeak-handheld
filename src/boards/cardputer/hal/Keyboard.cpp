#include "Keyboard.h"
#include "config/BoardConfig.h"

static constexpr unsigned long KEY_REPEAT_START_MS = 400;
static constexpr unsigned long KEY_REPEAT_INTERVAL_MS = 125;

namespace {
bool sameEvent(const KeyEvent& a, const KeyEvent& b) {
    return a.character == b.character &&
           a.ctrl == b.ctrl && a.shift == b.shift && a.fn == b.fn &&
           a.alt == b.alt && a.opt == b.opt && a.enter == b.enter &&
           a.backspace == b.backspace && a.forwardDelete == b.forwardDelete &&
           a.tab == b.tab && a.space == b.space && a.escape == b.escape &&
           a.up == b.up && a.down == b.down &&
           a.left == b.left && a.right == b.right;
}

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

KeyEvent Keyboard::currentEvent() const {
    for (uint8_t row = 0; row < 4; ++row) {
        for (uint8_t col = 0; col < 14; ++col) {
            if (!_pressed[row][col] || isModifierPosition(row, col)) continue;
            KeyEvent event = eventForKey(row, col);
            if (hasAction(event)) return event;
        }
    }
    return {};
}

void Keyboard::update() {
    _hasEvent = false;

    CardputerAdvKeyboard::Event rawEvents[16];
    const size_t rawCount = _hardware.poll(rawEvents, 16);
    KeyEvent tapped = {};
    bool hasTapped = false;
    for (size_t i = 0; i < rawCount; ++i) {
        const auto& raw = rawEvents[i];
        _pressed[raw.row][raw.col] = raw.pressed;
        if (raw.pressed && !isModifierPosition(raw.row, raw.col)) {
            KeyEvent candidate = eventForKey(raw.row, raw.col);
            if (!hasTapped && hasAction(candidate)) {
                tapped = candidate;
                hasTapped = true;
            }
        }
    }

    KeyEvent next = currentEvent();
    if (!hasAction(next)) {
        _keyHeld = false;
        _heldEvent = {};
        if (hasTapped) {
            _event = tapped;
            _hasEvent = true;
            if (_keyCallback) _keyCallback(_event);
        }
        return;
    }

    const unsigned long now = millis();
    const bool changed = !_keyHeld || !sameEvent(next, _heldEvent);
    if (!changed) {
        if (!isRepeatable(next) || now - _heldSince < KEY_REPEAT_START_MS ||
            now - _lastRepeat < KEY_REPEAT_INTERVAL_MS) {
            return;
        }
        _lastRepeat = now;
    } else {
        _keyHeld = true;
        _heldEvent = next;
        _heldSince = now;
        _lastRepeat = now;
    }

    _event = next;
    _hasEvent = true;

    if (_keyCallback) _keyCallback(_event);
}

bool Keyboard::capsLocked() const {
    return _capsLocked;
}

void Keyboard::setCapsLocked(bool locked) {
    _capsLocked = locked;
}
