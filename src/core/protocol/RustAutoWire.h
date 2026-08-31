#pragma once

#include <stddef.h>
#include <stdint.h>

// AutoInterface timing/port constants (Bucket A GAP-4). IPv6 text, multicast-group,
// and beacon-token bytes are constructed by the Rust FFI.
namespace RustAutoWire {

constexpr uint16_t DISCOVERY_PORT = 29716;          // auto.rs:19 / AutoInterface.py:47
constexpr uint16_t DATA_PORT = 42671;               // auto.rs:20 / py:48
constexpr uint16_t UNICAST_DISCOVERY_PORT = 29717;  // discovery+1 — auto.rs:28 / py:173
constexpr uint32_t BEACON_INTERVAL_MS = 1600;       // auto.rs:21 / py:62 (1.6 s)
constexpr uint32_t REVERSE_PEERING_INTERVAL_MS = 5200;  // 1.6*3.25 — auto.rs:29 / py:146
constexpr uint32_t PEER_JOB_INTERVAL_MS = 4000;     // auto.rs:30 / py:63
constexpr uint32_t PEER_EXPIRY_MS = 22000;          // auto.rs:24 / py:61 (device != Android rule)
constexpr uint32_t MCAST_ECHO_TIMEOUT_MS = 6500;    // auto.rs:31 / py:64
constexpr size_t BEACON_LEN = 32;                   // SHA-256 token — auto.rs:493-498 / py:480
constexpr uint32_t HW_MTU = 1196;                   // auto.rs:25 / py:44 (lite wire budget is 500;
                                                    // >500 B datagrams drop with a stat — no
                                                    // interop impact at RNS defaults)

}  // namespace RustAutoWire

