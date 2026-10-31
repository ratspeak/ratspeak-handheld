#pragma once

#include "TCPClientInterface.h"
#include <array>
#include <vector>

class RustInterfacePump;
struct TCPEndpoint;

// One protocol-owner collection. A reload snapshots the selected endpoints,
// detaches old bindings immediately, and waits for all SDK helpers to retire
// before constructing replacements. Active plus retiring owners never exceed 4.
class TcpClientSet {
public:
    static constexpr size_t Capacity = 4;
    TcpClientSet() = default;
    TcpClientSet(const TcpClientSet&) = delete;
    TcpClientSet& operator=(const TcpClientSet&) = delete;
    bool reload(RustInterfacePump& pump, const std::vector<TCPEndpoint>& endpoints);
    void stop();
    void maintain();
    void poll(unsigned long roundBudgetMs);
    bool quiescent() const { return _owned == 0 && !_pending; }
    size_t size() const { return _active; }
    bool empty() const { return _active == 0; }
    size_t owned() const { return _owned; }
    TCPClientInterface* const* begin() const { return _clients.data(); }
    TCPClientInterface* const* end() const { return begin() + _active; }

private:
    struct Endpoint { char host[254] = {}; uint16_t port = 0; };
    static uint32_t fingerprint(const Endpoint& endpoint);
    void retire();
    std::array<Endpoint, Capacity> _desired{};
    std::array<uint32_t, Capacity> _bound{};  // hub last attached per slot; survives stop()
    std::array<TCPClientInterface*, Capacity> _clients{};
    RustInterfacePump* _pump = nullptr;
    size_t _desiredCount = 0, _active = 0, _owned = 0, _cursor = 0;
    bool _retiring = false, _pending = false, _retrying = false;
    unsigned long _lastCreateAttempt = 0;
};
