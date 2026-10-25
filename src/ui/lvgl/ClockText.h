#pragma once
#include <cstddef>
#include <cstdio>
#include <ctime>

// One saved 12/24-hour choice for the status bar and message times.
inline void formatClockText(char* out, size_t size, const tm& local, bool use24h) {
    if (use24h) snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
    else snprintf(out, size, "%d:%02d", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min);
}
