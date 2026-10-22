#pragma once

// Compile-time hardware selection. Both adapters implement the raw-radio
// contract consumed by LoRaInterface; framing and queue ownership stay there.
#include "config/BoardConfig.h"
#if defined(RSM9)
#include "radio/LR1110Radio.h"
using BoardRadio = LR1110Radio;
#elif defined(RATPAGER_LR1121)
#include "radio/LR1121Radio.h"
using BoardRadio = LR1121Radio;
#elif defined(RSSENSECAPD1L)
#include "radio/SensecapSX1262Radio.h"
using BoardRadio = SensecapSX1262Radio;
#else
#include "radio/SX1262.h"
using BoardRadio = SX1262;
#endif
