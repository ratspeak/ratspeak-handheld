#pragma once

// Custom Montserrat fonts with extended Unicode ranges:
// ASCII (0x20-0x7F), Latin-1 Supplement (0xA0-0xFF),
// Latin Extended-A (0x100-0x17F), Cyrillic (0x400-0x4FF)
//
// Generated with lv_font_conv from Montserrat-Medium.ttf (OFL license)

#include <lvgl.h>

LV_FONT_DECLARE(lv_font_rsdeck_10)
LV_FONT_DECLARE(lv_font_rsdeck_12)
LV_FONT_DECLARE(lv_font_rsdeck_14)

#ifdef RSCROWPANEL
// CrowPanel's 800x480 surface uses larger type; other boards retain their fonts.
LV_FONT_DECLARE(lv_font_crowpanel_14)
LV_FONT_DECLARE(lv_font_crowpanel_18)
LV_FONT_DECLARE(lv_font_crowpanel_22)
#define lv_font_rsdeck_10 lv_font_crowpanel_14
#define lv_font_rsdeck_12 lv_font_crowpanel_18
#define lv_font_rsdeck_14 lv_font_crowpanel_22
#endif
