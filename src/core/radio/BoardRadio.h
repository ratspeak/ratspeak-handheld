#pragma once

// Compile-time hardware selection. Both adapters implement the raw-radio
// contract consumed by LoRaInterface; framing and queue ownership stay there.
#include "config/BoardConfig.h"
#if defined(RSM9)
#include "radio/LR1110Radio.h"
using BoardRadio = LR1110Radio;
#else
#include "radio/SX1262.h"
using BoardRadio = SX1262;
#endif
