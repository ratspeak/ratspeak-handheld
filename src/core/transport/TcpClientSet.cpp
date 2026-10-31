#include "runtime/TaskOwner.h"
#include "TcpClientSet.h"
#include "config/UserConfig.h"
#include "protocol/RustInterfacePump.h"
#include "runtime/ResourceBudget.h"
#include <algorithm>
#include <new>

static_assert(TcpClientSet::Capacity == RustInterfacePump::MAX_TCP &&
              TcpClientSet::Capacity == handheld::ResourceBudget::TcpOwners,
              "TCP owner and pump limits must agree");
// RX retention (500 bytes per client) is accounted with codec storage. Include
// each client's quarter of the collection and endpoint snapshot in its descriptor.
static_assert(sizeof(TCPClientInterface) - 500 +
              (sizeof(TcpClientSet) + 3) / 4 <= handheld::ResourceBudget::TcpDescriptor,
              "TCP descriptors exceed the approved per-owner budget");
static_assert(4 * 500 + 1034 + 516 <= handheld::ResourceBudget::TcpCodec,
              "TCP receive and shared transmit buffers exceed the Cardputer cap");

bool TcpClientSet::reload(RustInterfacePump& pump, const std::vector<TCPEndpoint>& endpoints) {
    handheld::assertDeviceOwner();
    // Validate before disturbing the current bindings. Configuration is bounded
    // at its owner too; malformed callers cannot create a partial replacement.
    if (endpoints.size() > Capacity) return false;
    std::array<Endpoint, Capacity> next{};
    size_t count = 0;
    for (const auto& endpoint : endpoints) {
        if (!endpoint.autoConnect || endpoint.host.isEmpty()) continue;
        const size_t len = endpoint.host.length();
        if (len >= sizeof(next[0].host) || endpoint.port == 0 ||
            strlen(endpoint.host.c_str()) != len) return false;
        memcpy(next[count].host, endpoint.host.c_str(), len + 1);
        next[count++].port = endpoint.port;
    }
    // A different hub behind a slot's interface ID makes its learned routes wrong; a Wi-Fi
    // reconnect to the same hubs keeps them.
    for (size_t i = 0; i < Capacity; ++i) {
        if (_bound[i] && (i >= count || _bound[i] != fingerprint(next[i]))) {
            pump.replaceTcp(i);
            _bound[i] = 0;
        }
    }
    retire();
    _pump = &pump;
    _desired = next;
    _desiredCount = count;
    _pending = count != 0;
    _retrying = false;
    maintain();
    return true;
}

uint32_t TcpClientSet::fingerprint(const Endpoint& endpoint) {
    uint32_t hash = 2166136261u;  // FNV-1a over host and port; zero means unbound
    for (const char* c = endpoint.host; *c; ++c) hash = (hash ^ uint8_t(*c)) * 16777619u;
    hash = (hash ^ (endpoint.port & 0xff)) * 16777619u;
    hash = (hash ^ (endpoint.port >> 8)) * 16777619u;
    return hash ? hash : 1;
}

void TcpClientSet::retire() {
    if (_pump) _pump->detachTcpAll();
    for (auto* client : _clients) if (client) client->stop();
    _active = 0;
    _cursor = 0;
    _retiring = _owned != 0;
}

void TcpClientSet::stop() {
    handheld::assertDeviceOwner();
    _pending = false;
    _desiredCount = 0;
    retire();
    maintain();
}

void TcpClientSet::maintain() {
    handheld::assertDeviceOwner();
    if (_retiring) {
        for (auto*& client : _clients) {
            if (client && client->canDestroy()) {
                delete client;
                client = nullptr;
                --_owned;
            }
        }
        if (_owned) return;
        _retiring = false;
    }
    if (!_pending || !_pump || WiFi.status() != WL_CONNECTED) return;
    if (_retrying && millis() - _lastCreateAttempt < 1000) return;
    _lastCreateAttempt = millis();
    _retrying = true;
    while (_active < _desiredCount) {
        const auto& endpoint = _desired[_active];
        auto* client = new (std::nothrow) TCPClientInterface(endpoint.host, endpoint.port, "TCP");
        if (!client) return;  // Retain the immutable request for bounded later retry.
        if (_pump->attachTcp(client) < 0) {
            delete client;  // No SDK task has started; destruction cannot wait.
            return;
        }
        _bound[_active] = fingerprint(endpoint);
        _clients[_active++] = client;
        ++_owned;
        // Endpoint and fixed buffers have already been validated. A failed SDK
        // task creation stays a live owner's bounded reconnect state.
        if (!client->start()) {
            retire();
            return;
        }
    }
    _pending = false;
}

void TcpClientSet::poll(unsigned long roundBudgetMs) {
    handheld::assertDeviceOwner();
    const unsigned long start = millis();
    for (size_t turns = 0; turns < _active; ++turns) {
        const unsigned long elapsed = millis() - start;
        if (elapsed >= roundBudgetMs) break;
        _cursor %= _active;
        TCPClientInterface* client = _clients[_cursor];
        _cursor = (_cursor + 1) % _active;  // Advance even if this client uses the round.
        client->loop(std::min(25UL, roundBudgetMs - elapsed), 1024);
        yield();
    }
}
