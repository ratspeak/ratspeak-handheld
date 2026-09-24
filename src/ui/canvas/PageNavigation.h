#pragma once
#include "Theme.h"

namespace handheld { namespace canvas {
constexpr int NavigationHeight = 18;
constexpr int NavigationGap = 8;
constexpr int NavigationWidth = 46;
template<class Enabled> void drawPageNavigation(M5Canvas& canvas, int y, int pageFocus, Enabled isEnabled) {
    constexpr int groupWidth = 4 * NavigationWidth + 3 * NavigationGap;
    for (int action = 0; action < 4; ++action) {
        const int x = (Theme::CONTENT_W - groupWidth) / 2 + action * (NavigationWidth + NavigationGap);
        const bool enabled = isEnabled(action), focused = enabled && pageFocus == action;
        canvas.fillRoundRect(x, y, NavigationWidth, NavigationHeight, 3,
                             focused ? Theme::SELECTION_BG : Theme::BG_ELEVATED);
        canvas.drawRoundRect(x, y, NavigationWidth, NavigationHeight, 3,
                             focused ? Theme::ACCENT : Theme::BORDER);
        const uint16_t color = !enabled ? Theme::TEXT_MUTED : focused ? Theme::ACCENT : Theme::TEXT_PRIMARY;
        const bool doubled = action == 0 || action == 3, right = action >= 2;
        const int width = doubled ? 14 : 6;
        const int left = x + (NavigationWidth - width) / 2;
        // Two-pixel strokes, centered by their actual ink bounds.
        for (int chevron = 0; chevron < (doubled ? 2 : 1); ++chevron) {
            for (int row = 0; row < 10; ++row) {
                const int slope = row < 5 ? row : 9 - row;
                canvas.drawFastHLine(left + chevron * 8 + (right ? slope : 4 - slope),
                                    y + (NavigationHeight - 10) / 2 + row, 2, color);
            }
        }
    }
}

}}
