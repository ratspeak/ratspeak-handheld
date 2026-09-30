#pragma once

#include "config/PropagationSettings.h"
#include "util/DisplayName.h"
#include "ratspeak_protocol.h"

namespace handheld::propagation {

// Owner-only bounded candidate cache. Route facts are supplied by the actual
// Reticulum path table, never inferred from the interface carrying an announce.
class Nodes {
public:
    static constexpr size_t PerFamily = 5;
    static constexpr size_t Capacity = PerFamily * 2;
    static constexpr uint8_t MaxStampCost = 20;
    static constexpr uint64_t MetadataAge = 24ULL * 60 * 60 * 1000;
    struct Node {
        uint64_t seen = 0, retryAt = 0, timebase = 0, transferBytes = 0;
        uint8_t address[16]{}, publicKey[64]{};
        char name[32]{};
        uint32_t interfaceGeneration = 0;
        uint8_t interface = UINT8_MAX, hops = 0, cost = 0, failures = 0;
        bool used = false, enabled = false, routed = false;
    };

    void reset() { const auto sequence = _sequence; *this = Nodes{}; _sequence = sequence; }
    // One upload or inbox transaction, including its storage gaps. Tokens never
    // wrap or revive after identity reset; a stale owner cannot release another.
    uint32_t claim() {
        if (_owner || _sequence == UINT32_MAX) return 0;
        return _owner = ++_sequence;
    }
    bool owns(uint32_t token) const { return token && token == _owner; }
    bool release(uint32_t token) {
        if (!owns(token)) return false;
        _owner = 0; return true;
    }
    bool busy() const { return _owner != 0; }
    void configure(const Settings& settings) {
        if (settings.hasManual && (!_settings.hasManual ||
                std::memcmp(settings.manual, _settings.manual, 16))) {
            const Node* known = find(settings.manual);
            _manual = known ? *known : Node{};
        } else if (!settings.hasManual) _manual = {};
        _settings = settings;
    }
    const Settings& settings() const { return _settings; }
    const Node* at(size_t index) const {
        return index < Capacity && _nodes[index].used ? &_nodes[index] : nullptr;
    }
    const Node* find(const uint8_t address[16]) const {
        for (const auto& node : _nodes)
            if (node.used && !std::memcmp(node.address, address, 16)) return &node;
        return _manual.used && !std::memcmp(_manual.address, address, 16) ? &_manual : nullptr;
    }
    const Node* active() const { return _hasActive ? find(_active) : nullptr; }
    static bool usable(const Node& node, uint64_t now) {
        return node.used && node.enabled && node.routed && node.interfaceGeneration &&
            node.cost <= MaxStampCost && node.transferBytes >= 256 &&
            now >= node.seen && now - node.seen <= MetadataAge && now >= node.retryAt;
    }

    // Metadata has already been signature/binding checked and decoded by Rust.
    // A new announcement cannot erase failure backoff or replace a bound key.
    bool learn(const uint8_t address[16], const uint8_t key[64],
               const rs_handheld_propagation_node_t& metadata,
               const rs_handheld_route_t& route, uint32_t generation, uint64_t now) {
        const Node* previous = find(address);
        if (previous && (std::memcmp(previous->publicKey, key, 64) ||
                         metadata.timebase < previous->timebase)) return false;
        Node next = previous ? *previous : Node{};
        next.used = true; next.seen = now; next.timebase = metadata.timebase;
        next.enabled = metadata.enabled != 0; next.cost = metadata.stamp_cost;
        if (metadata.transfer_limit_kb > UINT64_MAX / 1000) return false;
        next.transferBytes = metadata.transfer_limit_kb * 1000;
        std::memcpy(next.address, address, 16); std::memcpy(next.publicKey, key, 64);
        std::memset(next.name, 0, sizeof next.name);
        const size_t length = strnlen(reinterpret_cast<const char*>(metadata.name), sizeof metadata.name);
        const size_t prefix = displayNamePrefix(reinterpret_cast<const char*>(metadata.name), length, sizeof next.name - 1);
        std::memcpy(next.name, metadata.name, prefix);
        routeFacts(next, route, generation);
        if (_settings.hasManual && !std::memcmp(_settings.manual, address, 16)) _manual = next;
        return retain(next, now);
    }

    // Refresh once per scheduler interval; a route can move families only when
    // Reticulum actually replaced it. A vanished route leaves a visible candidate.
    void routeChanged(const uint8_t address[16], const rs_handheld_route_t& route,
                      uint32_t generation, uint64_t now) {
        const Node* current = find(address);
        if (!current) return;
        Node next = *current;
        routeFacts(next, route, generation);
        if (_manual.used && !std::memcmp(_manual.address, address, 16)) _manual = next;
        retain(next, now);
    }

    // Selection is stable within a family. A healthy WiFi/TCP route may replace
    // LoRa only at an idle boundary. MANUAL never substitutes another address.
    const Node* select(uint64_t now, bool idle = true, uint32_t owner = 0) {
        if (!idle || (_owner && !owns(owner))) return active();
        if (!_settings.enabled) { _hasActive = false; return nullptr; }
        if (_settings.selection == Selection::Manual) {
            _hasActive = _settings.hasManual;
            if (_hasActive) std::memcpy(_active, _settings.manual, 16);
            const Node* node = _hasActive ? find(_active) : nullptr;
            return node && usable(*node, now) ? node : nullptr;
        }
        const Node* current = active();
        const Node* best = nullptr;
        for (const auto& node : _nodes) {
            if (!usable(node, now)) continue;
            if (!best || (node.interface != 0 && best->interface == 0) ||
                ((node.interface == 0) == (best->interface == 0) && better(node, *best, now))) best = &node;
        }
        if (current && usable(*current, now) &&
            (!best || current->interface != 0 || best->interface == 0)) best = current;
        _hasActive = best != nullptr;
        if (best) std::memcpy(_active, best->address, 16);
        return best;
    }

    void outcome(const uint8_t address[16], bool success, uint64_t now) {
        auto update = [&](Node& node) {
            if (!node.used || std::memcmp(node.address, address, 16)) return;
            if (success) { node.failures = 0; node.retryAt = 0; return; }
            if (node.failures < 6) ++node.failures;
            uint64_t backoff = 60000ULL << (node.failures - 1);
            if (backoff > 1800000) backoff = 1800000;
            node.retryAt = now > UINT64_MAX - backoff ? UINT64_MAX : now + backoff;
        };
        for (auto& node : _nodes) update(node);
        update(_manual);
    }

private:
    static void routeFacts(Node& node, const rs_handheld_route_t& route, uint32_t generation) {
        node.routed = route.kind == RS_HANDHELD_ROUTE_DIRECT && route.interface_id <= 6 && generation;
        if (node.routed) { node.interface = route.interface_id; node.hops = route.hops; }
        node.interfaceGeneration = node.routed ? generation : 0;
    }
    static bool better(const Node& a, const Node& b, uint64_t now) {
        if (usable(a, now) != usable(b, now)) return usable(a, now);
        if (a.failures != b.failures) return a.failures < b.failures;
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.hops != b.hops) return a.hops < b.hops;
        if (a.seen != b.seen) return a.seen > b.seen;
        return std::memcmp(a.address, b.address, 16) < 0;
    }
    bool retain(const Node& candidate, uint64_t now) {
        // A first observation without any selected route belongs only in the
        // separate manual pin; it cannot consume a fictitious interface bucket.
        if (candidate.interface > 6) return false;
        const size_t base = candidate.interface == 0 ? 0 : PerFamily;
        for (size_t i = 0; i < Capacity; ++i) {
            if (!_nodes[i].used || std::memcmp(_nodes[i].address, candidate.address, 16)) continue;
            if (i >= base && i < base + PerFamily) { _nodes[i] = candidate; return true; }
            _nodes[i] = {};
        }
        Node* worst = nullptr;
        for (size_t i = base; i < base + PerFamily; ++i) {
            auto& node = _nodes[i];
            if (!node.used) { node = candidate; return true; }
            if (_hasActive && !std::memcmp(node.address, _active, 16)) continue;
            if (!worst || better(*worst, node, now)) worst = &node;
        }
        const bool movingActive = _hasActive && !std::memcmp(candidate.address, _active, 16);
        if (worst && (movingActive || better(candidate, *worst, now))) { *worst = candidate; return true; }
        return false;
    }
    Node _nodes[Capacity]{}, _manual;
    Settings _settings;
    uint8_t _active[16]{};
    uint32_t _owner = 0, _sequence = 0;
    bool _hasActive = false;
};
static_assert(sizeof(Nodes) <= 2048, "Review propagation candidate retention budget");

} // namespace handheld::propagation
