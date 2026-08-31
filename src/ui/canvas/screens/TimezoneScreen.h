#pragma once

#include "Screen.h"
#include "widgets/ScrollList.h"
#include "config/Config.h"
#include <functional>

struct TimezoneEntry {
    const char* label;
    const char* posixTZ;
    int8_t baseOffset;
    uint8_t radioRegion;
};

static const TimezoneEntry TIMEZONE_TABLE[] = {
    {"Honolulu (UTC-10)",     "HST10",                                    -10, REGION_AMERICAS},
    {"Anchorage (UTC-9)",     "AKST9AKDT,M3.2.0,M11.1.0",               -9,  REGION_AMERICAS},
    {"Los Angeles (UTC-8)",   "PST8PDT,M3.2.0,M11.1.0",                 -8,  REGION_AMERICAS},
    {"Denver (UTC-7)",        "MST7MDT,M3.2.0,M11.1.0",                 -7,  REGION_AMERICAS},
    {"Phoenix (UTC-7)",       "MST7",                                    -7,  REGION_AMERICAS},
    {"Chicago (UTC-6)",       "CST6CDT,M3.2.0,M11.1.0",                 -6,  REGION_AMERICAS},
    {"New York (UTC-5)",      "EST5EDT,M3.2.0,M11.1.0",                  -5,  REGION_AMERICAS},
    {"San Juan (UTC-4)",      "AST4",                                     -4,  REGION_AMERICAS},
    {"Sao Paulo (UTC-3)",     "<-03>3",                                   -3,  REGION_AMERICAS},
    {"London (UTC+0)",        "GMT0BST,M3.5.0/1,M10.5.0",                0,   REGION_EUROPE},
    {"Paris (UTC+1)",         "CET-1CEST,M3.5.0,M10.5.0/3",              1,   REGION_EUROPE},
    {"Cairo (UTC+2)",         "EET-2",                                    2,   REGION_EUROPE},
    {"Moscow (UTC+3)",        "MSK-3",                                    3,   REGION_EUROPE},
    {"Dubai (UTC+4)",         "<+04>-4",                                  4,   REGION_EUROPE},
    {"Karachi (UTC+5)",       "PKT-5",                                    5,   REGION_EUROPE},
    {"Kolkata (UTC+5:30)",    "IST-5:30",                                 5,   REGION_EUROPE},
    {"Bangkok (UTC+7)",       "<+07>-7",                                  7,   REGION_ASIA},
    {"Singapore (UTC+8)",     "<+08>-8",                                  8,   REGION_ASIA},
    {"Tokyo (UTC+9)",         "JST-9",                                    9,   REGION_ASIA},
    {"Sydney (UTC+10)",       "AEST-10AEDT,M10.1.0,M4.1.0/3",           10,   REGION_AUSTRALIA},
    {"Auckland (UTC+12)",     "NZST-12NZDT,M9.5.0,M4.1.0/3",            12,   REGION_AUSTRALIA},
};

static constexpr int TIMEZONE_COUNT = sizeof(TIMEZONE_TABLE) / sizeof(TIMEZONE_TABLE[0]);

class TimezoneScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    void onEnter() override;
    const char* title() const override { return "Timezone"; }

    using DoneCb = std::function<void(int tzIndex)>;
    void setDoneCallback(DoneCb cb) { _doneCb = cb; }
    void setSelectedIndex(int idx) { _selectedIdx = idx; }

private:
    DoneCb _doneCb;
    ScrollList _list;
    int _selectedIdx = 0;  // Start at top of list
};
