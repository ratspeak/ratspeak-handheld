#pragma once

#include "RustRrcEngine.h"
#include "ui/FeatureAvailability.h"
#include <type_traits>

namespace handheld::rrc {

// Public builds keep RRC unavailable until its release qualification. They
// must not retain an idle session's tables or start its storage/link work.
// Enabling the existing feature switch selects the complete engine unchanged.
class UnavailableEngine {
public:
    void begin(const RustRrcEngine::Deps&) {}
    void stop() {}
    void end() {}
    void loop() {}
    bool drained() const { return true; }
    void learn(const rs_handheld_announce_event_t&) {}
    Status status() const { return {}; }
    size_t hubs(HubView*, size_t) const { return 0; }
    size_t rooms(RoomView*, size_t) const { return 0; }
    size_t channels(RoomView*, size_t, size_t = 0) const { return 0; }
    size_t people(const char*, PersonView*, size_t) const { return 0; }
    size_t directory(DirectoryView*, size_t, size_t = 0) const { return 0; }
    Code command(const Command&, const uint8_t*, size_t) { return Code::Unsupported; }
    bool context(const uint8_t[16], const char*, const uint8_t*, storage::rrc::Context&) const { return false; }
};

using RuntimeEngine = std::conditional_t<ui::RrcEnabled, RustRrcEngine, UnavailableEngine>;
static_assert(ui::RrcEnabled || std::is_empty<RuntimeEngine>::value,
              "Unavailable RRC must not reserve runtime state");

} // namespace handheld::rrc
