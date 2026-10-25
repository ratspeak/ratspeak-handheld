#include "NoticeOverlay.h"
#include "Theme.h"
#include <algorithm>
#include <cstring>

void NoticeOverlay::render(M5Canvas& canvas) {
    if (!_visible) return;
    const int left = 8, top = Theme::CONTENT_Y + 2, width = Theme::CONTENT_W - 16, height = Theme::CONTENT_H - 4;
    canvas.fillRect(left, top, width, height, Theme::BG);
    canvas.drawRect(left, top, width, height, Theme::BORDER);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.setTextColor(Theme::ACCENT);
    int y = top + 6;
    canvas.drawString(_heading, left + 6, y); y += 14;
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    // Word wrap in the fixed-width built-in font.
    const size_t columns = size_t(width - 12) / Theme::CHAR_W;
    char line[48];
    for (const char* rest = _text; *rest && y < top + height - 22; y += Theme::UI_FONT_H) {
        size_t cut = strlen(rest);
        if (cut > columns) {
            cut = columns;
            while (cut && rest[cut] != ' ') --cut;
            if (!cut) cut = columns;
        }
        cut = std::min(cut, sizeof(line) - 1);
        memcpy(line, rest, cut); line[cut] = 0;
        canvas.drawString(line, left + 6, y);
        rest += cut;
        while (*rest == ' ') ++rest;
    }
    canvas.setTextColor(Theme::TEXT_MUTED);
    canvas.drawString("Any key closes", left + 6, top + height - 12);
}

bool NoticeOverlay::handleKey(const KeyEvent& event) {
    if (!_visible) return false;
    if (event.repeat) return true;
    _visible = false;
    if (_dismissed) _dismissed();
    return true;
}
