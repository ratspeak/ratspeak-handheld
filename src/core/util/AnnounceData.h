#pragma once

#include <Arduino.h>
#include "util/Bytes.h"

// Shared LXMF name/capability advertisement for normal and path-response announces.
rs::Bytes encodeAnnounceName(const String& name, const char* destination = nullptr);
