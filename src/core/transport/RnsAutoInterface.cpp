
#include "RnsAutoInterface.h"
#include "protocol/RustAutoWire.h"
#include "ratspeak_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <string.h>

// lwIP socket surface proven in-fleet by the donor micro AutoInterface on these exact
// boards (espressif32 SDK: LWIP_IPV6=1, LWIP_IPV6_MLD on): bind-any AF_INET6 datagram
// sockets, IPV6_MULTICAST_IF/HOPS/LOOP, IPV6_JOIN_GROUP, fcntl O_NONBLOCK.

uint8_t RnsAutoInterface::_rxBuf[1300];

namespace {

constexpr size_t LITE_WIRE_BUDGET = 500;  // rns-lite PacketView budget; see header note
constexpr size_t LEGACY_BEACON_INPUT_CAP = 160;

bool makeNonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool bindAny(int fd, uint16_t port) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    a.sin6_addr = in6addr_any;
    return ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
}

void makeDst(sockaddr_in6& dst, const uint8_t ip[16], uint32_t scopeId, uint16_t port) {
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_port = htons(port);
    dst.sin6_scope_id = scopeId;
    memcpy(&dst.sin6_addr, ip, 16);
}

int openUdp6(uint16_t port) {
    int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
    if (!bindAny(fd, port) || !makeNonblocking(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

bool RnsAutoInterface::computeSelf(const uint8_t llRaw[16]) {
    memcpy(_selfLl, llRaw, 16);
    size_t textLen = 0;
    if (rs_handheld_rns_auto_format_ipv6(_selfLl, _selfLlStr, sizeof(_selfLlStr), &textLen) !=
        RS_HANDHELD_OK) {
        Serial.println("[AUTOIFACE] Rust IPv6 formatting failed");
        return false;
    }
    if (_groupId.size() + textLen > LEGACY_BEACON_INPUT_CAP) {
        Serial.println("[AUTOIFACE] group id too long");
        return false;
    }
    if (rs_handheld_rns_auto_beacon_token(
            reinterpret_cast<const uint8_t*>(_groupId.data()), _groupId.size(), _selfLl,
            _selfToken) != RS_HANDHELD_OK) {
        Serial.println("[AUTOIFACE] Rust wire derivation failed");
        return false;
    }
    return true;
}

bool RnsAutoInterface::start(const char* groupId, uint8_t maxPeers,
                             const uint8_t llRaw[16], uint32_t scopeId) {
    if (_online) return true;
    _groupId = (groupId && groupId[0]) ? groupId : "reticulum";
    _maxPeers = maxPeers == 0 ? 1
              : (maxPeers > MAX_PEERS_CAP ? (uint8_t)MAX_PEERS_CAP : maxPeers);
    _scopeId = scopeId;
    _prevLlCount = 0;
    if (!computeSelf(llRaw)) return false;

    if (rs_handheld_rns_auto_multicast_group(
            reinterpret_cast<const uint8_t*>(_groupId.data()), _groupId.size(), _mcastGroup) !=
        RS_HANDHELD_OK) {
        return false;
    }
    char mc[46];
    size_t mcLen = 0;
    if (rs_handheld_rns_auto_format_ipv6(_mcastGroup, mc, sizeof(mc), &mcLen) != RS_HANDHELD_OK)
        return false;
    _mcastAddrStr = mc;

    if (!openSockets()) {
        closeSockets();
        return false;
    }

    memset(_peers, 0, sizeof(_peers));
    _online = true;
    _rxEnabled = false;
    _nextBeaconMs = 0;  // first loop() pass beacons immediately
    _nextPeerJobMs = millis() + RustAutoWire::PEER_JOB_INTERVAL_MS;
    _lastEchoMs = 0;
    _lastAnyRxMs = 0;
    _carrierOk = false;
    Serial.printf("[AUTOIFACE] online (rust) - mcast=%s ll=%s scope=%u\n",
                  _mcastAddrStr.c_str(), _selfLlStr, (unsigned)_scopeId);
    return true;
}

bool RnsAutoInterface::openSockets() {
    const int yes = 1;
    const int hops = 1;  // SCOPE_LINK: never leaves the LAN

    _discSock = openUdp6(RustAutoWire::DISCOVERY_PORT);
    if (_discSock < 0) {
        Serial.println("[AUTOIFACE] discovery socket failed");
        return false;
    }
    const unsigned int ifindex = static_cast<unsigned int>(_scopeId);
    ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
    // Own-echo carrier check wants loopback; if lwIP rejects it, carrier detection
    // degrades to the any-RX heuristic in peerJobs (log-only, no wire effect).
    _mcastLoopOk = ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &yes,
                                sizeof(yes)) == 0;
    if (!_mcastLoopOk) {
        Serial.printf("[AUTOIFACE] IPV6_MULTICAST_LOOP unsupported (errno=%d) - "
                      "carrier check degrades to any-RX\n", errno);
    }
    struct ipv6_mreq mreq{};
    memcpy(&mreq.ipv6mr_multiaddr, _mcastGroup, 16);
    mreq.ipv6mr_interface = ifindex;
    if (::setsockopt(_discSock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
        // Best-effort (Python parity): boot without LAN and recover later.
        Serial.printf("[AUTOIFACE] IPV6_JOIN_GROUP failed (errno=%d) - "
                      "multicast RX may not work\n", errno);
    }

    _unicastSock = openUdp6(RustAutoWire::UNICAST_DISCOVERY_PORT);
    if (_unicastSock < 0) {
        Serial.println("[AUTOIFACE] unicast discovery socket failed");
        return false;
    }
    _dataSock = openUdp6(RustAutoWire::DATA_PORT);
    if (_dataSock < 0) {
        Serial.println("[AUTOIFACE] data socket failed");
        return false;
    }
    return true;
}

void RnsAutoInterface::closeSockets() {
    if (_discSock >= 0) {
        struct ipv6_mreq mreq{};
        memcpy(&mreq.ipv6mr_multiaddr, _mcastGroup, 16);
        mreq.ipv6mr_interface = static_cast<unsigned int>(_scopeId);
        ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq));
        ::close(_discSock);
        _discSock = -1;
    }
    if (_unicastSock >= 0) {
        ::close(_unicastSock);
        _unicastSock = -1;
    }
    if (_dataSock >= 0) {
        ::close(_dataSock);
        _dataSock = -1;
    }
}

void RnsAutoInterface::stop() {
    if (!_online) return;
    closeSockets();
    memset(_peers, 0, sizeof(_peers));
    _online = false;
    _rxEnabled = false;
    Serial.println("[AUTOIFACE] stopped");
}

void RnsAutoInterface::notifyLinkChange(const uint8_t llRaw[16], uint32_t scopeId) {
    if (!_online) return;
    if (memcmp(llRaw, _selfLl, 16) == 0 && scopeId == _scopeId) return;  // idempotent

    // Keep the old LL self-classified: late echoes of our prior beacons must not
    // re-add our former address as a peer.
    if (_prevLlCount < 2) {
        memcpy(_prevLl[_prevLlCount++], _selfLl, 16);
    } else {
        memcpy(_prevLl[0], _prevLl[1], 16);
        memcpy(_prevLl[1], _selfLl, 16);
    }

    uint32_t prevScope = _scopeId;
    _scopeId = scopeId;
    computeSelf(llRaw);
    if (prevScope != scopeId && _discSock >= 0) {
        struct ipv6_mreq mreq{};
        memcpy(&mreq.ipv6mr_multiaddr, _mcastGroup, 16);
        mreq.ipv6mr_interface = static_cast<unsigned int>(prevScope);
        ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq));
        const unsigned int idx = static_cast<unsigned int>(scopeId);
        ::setsockopt(_discSock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
        mreq.ipv6mr_interface = idx;
        if (::setsockopt(_discSock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
            Serial.printf("[AUTOIFACE] rejoin on scope=%u failed (errno=%d)\n",
                          (unsigned)scopeId, errno);
        }
    }
    Serial.printf("[AUTOIFACE] link-local changed - ll=%s scope=%u\n", _selfLlStr,
                  (unsigned)scopeId);
}

void RnsAutoInterface::loop() {
    if (!_online) return;
    unsigned long now = millis();

    pollDiscovery(_discSock);
    pollDiscovery(_unicastSock);
    pollData();

    if ((long)(now - _nextBeaconMs) >= 0) {
        sendBeacon();
        _nextBeaconMs = now + RustAutoWire::BEACON_INTERVAL_MS;
        _rxEnabled = true;  // final_init parity: process discovery only once we announce
    }
    if ((long)(now - _nextPeerJobMs) >= 0) {
        peerJobs(now);
        _nextPeerJobMs = now + RustAutoWire::PEER_JOB_INTERVAL_MS;
    }
}

void RnsAutoInterface::sendBeacon() {
    sockaddr_in6 dst;
    makeDst(dst, _mcastGroup, _scopeId, RustAutoWire::DISCOVERY_PORT);
    ssize_t n = ::sendto(_discSock, _selfToken, RustAutoWire::BEACON_LEN, 0,
                         reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    if (n == (ssize_t)RustAutoWire::BEACON_LEN) {
        _stats.txBeacons++;
    } else if (errno != ENETUNREACH && errno != EHOSTUNREACH) {
        Serial.printf("[AUTOIFACE] beacon TX failed (errno=%d)\n", errno);
    }
}

bool RnsAutoInterface::isSelf(const uint8_t ip[16]) const {
    if (memcmp(ip, _selfLl, 16) == 0) return true;
    for (int i = 0; i < _prevLlCount; i++) {
        if (memcmp(ip, _prevLl[i], 16) == 0) return true;
    }
    return false;
}

void RnsAutoInterface::pollDiscovery(int fd) {
    if (fd < 0) return;
    for (int i = 0; i < 4; i++) {
        sockaddr_in6 src{};
        socklen_t sl = sizeof(src);
        ssize_t n = ::recvfrom(fd, _rxBuf, sizeof(_rxBuf), 0,
                               reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 0) break;
        if (!_rxEnabled) continue;
        const uint8_t* srcIp = reinterpret_cast<const uint8_t*>(&src.sin6_addr);
        if (isSelf(srcIp)) {
            _lastEchoMs = millis();  // multicast carrier confirmed
            continue;
        }
        if (n < (ssize_t)RustAutoWire::BEACON_LEN) continue;

        // Verify SHA-256(group_id || RFC-5952 source string) — AutoInterface.py:363-369.
        char srcStr[40];
        size_t srcLen = 0;
        if (rs_handheld_rns_auto_format_ipv6(srcIp, srcStr, sizeof(srcStr), &srcLen) !=
                RS_HANDHELD_OK ||
            _groupId.size() + srcLen > LEGACY_BEACON_INPUT_CAP)
            continue;
        uint8_t expected[32];
        if (rs_handheld_rns_auto_beacon_token(
                reinterpret_cast<const uint8_t*>(_groupId.data()), _groupId.size(), srcIp,
                expected) != RS_HANDHELD_OK)
            continue;
        if (memcmp(_rxBuf, expected, 32) != 0) {
            _stats.rxBadBeacons++;
            continue;
        }
        _stats.rxBeacons++;
        _lastAnyRxMs = millis();
        upsertPeer(srcIp, millis());
    }
}

void RnsAutoInterface::pollData() {
    if (_dataSock < 0) return;
    for (int i = 0; i < 4; i++) {
        sockaddr_in6 src{};
        socklen_t sl = sizeof(src);
        ssize_t n = ::recvfrom(_dataSock, _rxBuf, sizeof(_rxBuf), 0,
                               reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 0) break;
        if (!_rxEnabled) continue;
        const uint8_t* srcIp = reinterpret_cast<const uint8_t*>(&src.sin6_addr);
        Peer* p = findPeer(srcIp);
        if (!p) {
            // Python parity: data only from beacon-verified peers (py:592-594).
            _stats.rxUnknownPeer++;
            continue;
        }
        p->lastHeardMs = millis();  // data RX refreshes the peer (py:629)
        _lastAnyRxMs = p->lastHeardMs;
        if (n > (ssize_t)LITE_WIRE_BUDGET) {
            _stats.rxOversize++;
            if (!_oversizeLogged) {
                _oversizeLogged = true;
                Serial.printf("[AUTOIFACE] dropped %d-byte frame (lite wire budget %u) - "
                              "peer using >default MTU\n", (int)n, (unsigned)LITE_WIRE_BUDGET);
            }
            continue;
        }
        _stats.rxFrames++;
        if (_rawSink) _rawSink(_rxBuf, (size_t)n);
    }
}

RnsAutoInterface::Peer* RnsAutoInterface::findPeer(const uint8_t ip[16]) {
    for (size_t i = 0; i < MAX_PEERS_CAP; i++) {
        if (_peers[i].used && memcmp(_peers[i].ip, ip, 16) == 0) return &_peers[i];
    }
    return nullptr;
}

void RnsAutoInterface::upsertPeer(const uint8_t ip[16], unsigned long now) {
    Peer* p = findPeer(ip);
    if (p) {
        p->lastHeardMs = now;
        return;
    }
    size_t used = peerCount();
    if (used >= _maxPeers) {
        _stats.peersDropped++;
        return;
    }
    for (size_t i = 0; i < MAX_PEERS_CAP; i++) {
        if (!_peers[i].used) {
            memcpy(_peers[i].ip, ip, 16);
            _peers[i].lastHeardMs = now;
            _peers[i].lastReverseMs = now;  // beacon just heard; reverse due in 5.2 s
            _peers[i].used = true;
            char s[46] = "?";
            size_t sLen = 0;
            rs_handheld_rns_auto_format_ipv6(ip, s, sizeof(s), &sLen);
            Serial.printf("[AUTOIFACE] added peer %s (%u total)\n", s,
                          (unsigned)(used + 1));
            return;
        }
    }
}

void RnsAutoInterface::peerJobs(unsigned long now) {
    for (size_t i = 0; i < MAX_PEERS_CAP; i++) {
        if (!_peers[i].used) continue;
        if (now - _peers[i].lastHeardMs > RustAutoWire::PEER_EXPIRY_MS) {
            char s[46] = "?";
            size_t sLen = 0;
            rs_handheld_rns_auto_format_ipv6(_peers[i].ip, s, sizeof(s), &sLen);
            Serial.printf("[AUTOIFACE] removed peer %s (timeout)\n", s);
            _peers[i].used = false;
            continue;
        }
        // Reverse peering keeps us reachable across one-way multicast paths
        // (py:393-401 / auto.rs reverse task).
        if (now - _peers[i].lastReverseMs >= RustAutoWire::REVERSE_PEERING_INTERVAL_MS) {
            sockaddr_in6 dst;
            makeDst(dst, _peers[i].ip, _scopeId, RustAutoWire::UNICAST_DISCOVERY_PORT);
            ::sendto(_unicastSock, _selfToken, RustAutoWire::BEACON_LEN, 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            _peers[i].lastReverseMs = now;
        }
    }

    // Carrier detection (log-only): own multicast echo, degraded to any verified RX
    // when loopback is unavailable. Wire-neutral either way.
    bool echoOk = _lastEchoMs != 0 &&
                  now - _lastEchoMs <= RustAutoWire::MCAST_ECHO_TIMEOUT_MS;
    bool anyRxOk = _lastAnyRxMs != 0 &&
                   now - _lastAnyRxMs <= RustAutoWire::MCAST_ECHO_TIMEOUT_MS;
    bool ok = _mcastLoopOk ? (echoOk || anyRxOk) : anyRxOk;
    if (ok != _carrierOk) {
        _carrierOk = ok;
        Serial.printf("[AUTOIFACE] carrier %s (%s)\n", ok ? "ok" : "lost",
                      echoOk ? "mcast echo" : (anyRxOk ? "peer RX" : "no RX in 6.5s"));
    }
}

bool RnsAutoInterface::sendRaw(const uint8_t* data, size_t len) {
    if (!_online || _dataSock < 0 || !data || len == 0) return false;
    bool sent = false;
    for (size_t i = 0; i < MAX_PEERS_CAP; i++) {
        if (!_peers[i].used) continue;
        sockaddr_in6 dst;
        makeDst(dst, _peers[i].ip, _scopeId, RustAutoWire::DATA_PORT);
        if (::sendto(_dataSock, data, len, 0, reinterpret_cast<sockaddr*>(&dst),
                     sizeof(dst)) == (ssize_t)len) {
            sent = true;
        }
    }
    if (sent) _stats.txFrames++;
    return sent;
}

size_t RnsAutoInterface::peerCount() const {
    size_t n = 0;
    for (size_t i = 0; i < MAX_PEERS_CAP; i++) {
        if (_peers[i].used) n++;
    }
    return n;
}
