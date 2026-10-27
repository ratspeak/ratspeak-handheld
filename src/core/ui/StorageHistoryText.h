#pragma once

#include "storage/StorageContract.h"

namespace handheld::ui::storage_history {
inline constexpr const char* Label = "History";
inline constexpr const char* RollOffTitle = "Message history";
inline constexpr const char* RollOffNotice =
    "Oldest messages are removed to make room. To keep full history, use an SD card (Settings > Storage).";

constexpr const char* mode(storage::HistoryMode value) {
    return value == storage::HistoryMode::Card ? "Complete on SD" :
           value == storage::HistoryMode::CardNotLoaded ? "SD not loaded" : "Rolling";
}
}
