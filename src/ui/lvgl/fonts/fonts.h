#pragma once

// Custom Montserrat fonts (text) merged with DejaVu Sans (symbols), both
// OFL-licensed, via lv_font_conv's multi --font/-r support. Ranges:
// ASCII (0x20-0x7F), Latin-1 Supplement (0xA0-0xFF),
// Latin Extended-A (0x100-0x17F), Cyrillic (0x400-0x4FF) -- Montserrat-Medium.ttf
// General Punctuation (0x2000-0x206F), Arrows (0x2190-0x21FF, reload icon),
// Box Drawing (0x2500-0x257F), Block Elements + Geometric Shapes
// (0x2580-0x25FF), Misc Symbols (0x2600-0x26FF, star icon) -- DejaVuSans.ttf.
// Complete coverage confirmed by parsing the generated cmap for every
// range except Misc Symbols, which DejaVu only partially covers (67/256
// codepoints missing) -- codepointSupported() in LvNomadBrowserScreen.cpp
// allows only the two specific codepoints actually used there (U+2605/6),
// not the whole range. All three source TTFs (Montserrat-Medium,
// Montserrat-Bold, DejaVuSans) are vendored in sources/ alongside this
// file so regeneration never depends on another project's checkout or on
// PlatformIO's lib cache still having LVGL's built-in demo fonts around.

#include <lvgl.h>

LV_FONT_DECLARE(lv_font_rsdeck_10)
LV_FONT_DECLARE(lv_font_rsdeck_12)
LV_FONT_DECLARE(lv_font_rsdeck_14)
LV_FONT_DECLARE(lv_font_rsdeck_16)

// Bold weight (Montserrat-Bold.ttf, same ranges), for Micron `! styling.
LV_FONT_DECLARE(lv_font_rsdeck_10_bold)
LV_FONT_DECLARE(lv_font_rsdeck_12_bold)
LV_FONT_DECLARE(lv_font_rsdeck_14_bold)
LV_FONT_DECLARE(lv_font_rsdeck_16_bold)
