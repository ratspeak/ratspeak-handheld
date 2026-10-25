#pragma once
#include <cstdint>

namespace handheld {
// A completed empty scan is distinct from cancellation or an SDK failure.
enum class ScanResult : uint8_t { Pending, Ready, Cancelled, Failed, NoMemory };
} // namespace handheld
