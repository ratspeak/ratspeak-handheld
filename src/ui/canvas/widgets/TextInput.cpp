#include "TextInput.h"
#include <new>
#include <algorithm>
#include <cstring>

bool TextInput::reserveTextCapacity(size_t capacity) {
    if (_text.size() > capacity) return false;
    try { if (_text.capacity() < capacity) _text.reserve(capacity); }
    catch (const std::bad_alloc&) { return false; }
    _capacityLimit = capacity;
    return true;
}

bool TextInput::setText(const std::string& text) {
    return setText(text.data(), text.size());
}

bool TextInput::setText(const char* text, size_t length) {
    if ((!text && length) || (_capacityLimit && length > _capacityLimit)) return false;
    const bool changed = _text.size() != length || (length && _text.compare(0, length, text, length));
    _text.assign(text ? text : "", length);
    if (changed && _revision != UINT64_MAX) ++_revision;
    _cursorPos = _text.length();
    return true;
}

bool TextInput::appendText(const char* text, size_t length) {
    if ((!text && length) || (_capacityLimit && length > _capacityLimit - _text.size()) ||
        length + _text.size() > size_t(_maxLength)) return false;
    if (length) { _text.append(text, length); if (_revision != UINT64_MAX) ++_revision; }
    _cursorPos = _text.length(); return true;
}

void TextInput::clear() {
    if (!_text.empty() && _revision != UINT64_MAX) ++_revision;
    _text.clear();
    _cursorPos = 0;
    _numericOnly = false;
}

bool TextInput::handleKey(const KeyEvent& event) {
    if (!_active) return false;

    // Enter → submit
    if (event.enter) {
        if (_submitCb && !_text.empty()) {
            _submitCb(_text);
        }
        return true;
    }

    if (event.left) {
        if (_cursorPos > 0) { --_cursorPos; while (_cursorPos && (uint8_t(_text[_cursorPos]) & 0xc0) == 0x80) --_cursorPos; }
        return true;
    }

    if (event.right) {
        if (_cursorPos < (int)_text.length()) { ++_cursorPos; while (_cursorPos < int(_text.size()) && (uint8_t(_text[_cursorPos]) & 0xc0) == 0x80) ++_cursorPos; }
        return true;
    }

    // Backspace removes the character before the cursor.
    if (event.backspace) {
        if (_cursorPos > 0 && !_text.empty()) {
            int start = _cursorPos - 1;
            while (start && (uint8_t(_text[start]) & 0xc0) == 0x80) --start;
            _text.erase(start, _cursorPos - start);
            if (_revision != UINT64_MAX) ++_revision;
            _cursorPos = start;
        }
        return true;
    }

    // Fn+Backspace is the printed forward-Delete action.
    if (event.forwardDelete) {
        if (_cursorPos < (int)_text.length()) {
            size_t end = _cursorPos + 1;
            while (end < _text.size() && (uint8_t(_text[end]) & 0xc0) == 0x80) ++end;
            _text.erase(_cursorPos, end - _cursorPos);
            if (_revision != UINT64_MAX) ++_revision;
        }
        return true;
    }

    // Regular character (includes space at ASCII 32)
    if (event.character >= 32 && event.character < 127) {
        if (_numericOnly && (event.character < '0' || event.character > '9')) {
            return true;  // Reject non-digit input
        }
        if ((int)_text.length() < _maxLength && (!_capacityLimit || _text.size() < _capacityLimit)) {
            _text.insert(_text.begin() + _cursorPos, event.character);
            if (_revision != UINT64_MAX) ++_revision;
            _cursorPos++;
        }
        return true;
    }

    // Space (fallback if character wasn't set but space flag is)
    if (event.space && event.character < 32) {
        if ((int)_text.length() < _maxLength && (!_capacityLimit || _text.size() < _capacityLimit)) {
            _text.insert(_text.begin() + _cursorPos, ' ');
            if (_revision != UINT64_MAX) ++_revision;
            _cursorPos++;
        }
        return true;
    }

    return false;
}

void TextInput::render(M5Canvas& canvas, int x, int y, int w) {
    int h = Theme::CHAR_H + 4;

    // Background and border
    canvas.fillRect(x, y, w, h, Theme::BG_ELEVATED);
    canvas.drawRect(x, y, w, h, _active ? Theme::PRIMARY : Theme::BORDER);

    // Prompt
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.setTextColor(Theme::ACCENT);
    canvas.setCursor(x + 2, y + 2);
    canvas.print("> ");

    // Text (scroll if too wide)
    int textAreaW = w - 4 - 2 * Theme::CHAR_W;  // Account for "> " prompt
    int maxChars = textAreaW / Theme::CHAR_W;
    int textX = x + 2 + 2 * Theme::CHAR_W;

    // Byte-bounded viewport with complete UTF-8 scalars, including drafts and
    // nickname insertions that were not typed on the ASCII hardware keyboard.
    char visible[96]{};
    maxChars = std::max(0, std::min(maxChars, int(sizeof visible - 1)));
    size_t start = _text.size() > size_t(maxChars) ? size_t(std::max(0, _cursorPos - maxChars + 1)) : 0;
    start = std::min(start, _text.size());
    while (start < _text.size() && (uint8_t(_text[start]) & 0xc0) == 0x80) ++start;
    size_t end = std::min(_text.size(), start + size_t(maxChars));
    while (end > start && end < _text.size() && (uint8_t(_text[end]) & 0xc0) == 0x80) --end;
    memcpy(visible, _text.data() + start, end - start);
    canvas.setTextColor(Theme::TEXT_PRIMARY);canvas.setCursor(textX, y + 2);canvas.print(visible);

    // Cursor blink
    if (_active) {
        unsigned long now = millis();
        if (now - _lastBlink > 500) {
            _cursorVisible = !_cursorVisible;
            _lastBlink = now;
        }
        if (_cursorVisible) {
            const size_t cursor = size_t(_cursorPos) > start ? std::min(size_t(_cursorPos) - start, end - start) : 0;
            visible[cursor] = 0;
            const int cursorX = textX + canvas.textWidth(visible);
            canvas.fillRect(cursorX, y + 1, Theme::CHAR_W, Theme::CHAR_H + 2, Theme::PRIMARY);
        }
    }
    Theme::useSmallFont(canvas);
}
