#pragma once
#include <Arduino.h>
#include "config/BoardConfig.h"
#include "hal/Expander.h"

namespace wiol2 {
// GNSS power and reset are expander bits; reset is active HIGH. Power the
// receiver with reset asserted, then release it after 10 ms.
inline bool setGnssPower(bool enabled) {
    if (!enabled) {
        return expander::setOutput(EXP_GNSS_RST, false) &&
               expander::setOutput(EXP_GNSS_POWER, false);
    }
    if (!expander::setOutput(EXP_GNSS_RST, true) ||
        !expander::setOutput(EXP_GNSS_POWER, true)) return false;
    delay(10);
    return expander::setOutput(EXP_GNSS_RST, false);
}
} // namespace wiol2
