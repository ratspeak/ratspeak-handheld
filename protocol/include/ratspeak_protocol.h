/*
 * C interface to the Ratspeak handheld protocol library (libratspeak_protocol.a).
 * Declarations are implemented in protocol/src/ and checked by the host ABI tests.
 *
 * Buffer sizes: raw private key = 64 bytes (X25519 priv 32 || Ed25519 seed 32), public key
 * = 64 bytes, identity hash = 16 bytes, destination hash = 16 bytes. The caller owns all
 * buffers passed in/out; Rust never allocates them. Array-sized parameters (e.g. [64], [16])
 * are length contracts: they decay to plain pointers in C, so the caller MUST supply exactly
 * that many readable/writable bytes (Rust receives them as fixed-size const/mut [u8;N] ptrs).
 *
 * Secrets: the backend zeroizes its own copy of the raw private key when the context is
 * dropped (shutdown) or the identity is replaced. Buffers the caller receives from
 * rs_handheld_rns_export_identity / _create_identity hold secret key material — the caller is
 * responsible for wiping them.
 *
 * ABI contract:
 *  - Rust owns the opaque context. Create with rs_handheld_rns_init, free with rs_handheld_rns_shutdown.
 *    No Rust allocation is freed by C, and no C allocation is freed by Rust.
 *  - Each context returned by rs_handheld_rns_init must be freed exactly once.
 *  - The transport node lives in a CALLER-owned buffer (rs_handheld_rns_open_transport):
 *    Rust never frees it, shutdown never touches it (the node is plain no-Drop data).
 *    Teardown order: resource closes -> rs_handheld_rns_shutdown(ctx) -> free(buf).
 *  - Host builds catch Rust panics and return RS_HANDHELD_ERR_INTERNAL. Firmware
 *    builds abort on panic; the hardware watchdog resets the device. Unwinding
 *    never crosses this boundary.
 *  - rs_handheld_rns_version() returns a static, NUL-terminated string owned by Rust; do not free.
 *  - All functions are thread-compatible but not internally synchronized; do not call against
 *    the same context concurrently (no internal locks).
 *
 * Stack and memory: cryptographic operations need several KB of task stack.
 * Measure stack headroom when integrating a new board. Use transport_size/align
 * to allocate the node buffer; SMALL uses PSRAM on T-Deck/T-Pager and MICRO uses
 * internal RAM on Cardputer (compile-checked at <= 32 KB). Construction writes
 * the node in place without a node-sized stack temporary.
 * Ratchet persistence buffers (up to 2690 and 1826 bytes) belong in long-lived
 * caller storage, not task-stack locals. Wipe secret-ring buffers after durable
 * write/commit.
 */
#ifndef RATSPEAK_PROTOCOL_H
#define RATSPEAK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status / error codes. Keep in sync with RsHandheldStatus in src/lib.rs (which pins
 * #[repr(i32)] + compile-time asserts). All codes stay nonnegative and <= INT_MAX; this
 * enum must remain int-sized — do NOT compile this header with -fshort-enums. */
typedef enum rs_handheld_status {
    RS_HANDHELD_OK = 0,
    RS_HANDHELD_ERR_INVALID_ARG = 1,
    RS_HANDHELD_ERR_CAPACITY = 2,
    RS_HANDHELD_ERR_CRYPTO = 3,
    RS_HANDHELD_ERR_NOT_READY = 4,
    RS_HANDHELD_ERR_UNSUPPORTED = 5,
    /* Transient, entropy-dependent failure (resource map-hash collision): retry the same
     * call with fresh caller entropy. */
    RS_HANDHELD_ERR_RETRY = 6,
    /* The payload's source was identified, but no Rust-owned known-destination/path key exists. */
    RS_HANDHELD_ERR_SOURCE_UNKNOWN = 7,
    RS_HANDHELD_ERR_INTERNAL = 255
} rs_handheld_status_t;

/* Opaque, Rust-owned backend context. */
typedef struct rs_handheld_rns rs_handheld_rns_t;

/* Static NUL-terminated version string, owned by Rust. Never null; do not free. */
const char *rs_handheld_rns_version(void);

/* Allocate a context; *out receives an owned pointer on RS_HANDHELD_OK. RS_HANDHELD_ERR_INVALID_ARG
 * if out is null. On any non-OK return *out is NOT modified — initialize *out to NULL
 * beforehand and only use it after checking the return code is RS_HANDHELD_OK. */
rs_handheld_status_t rs_handheld_rns_init(rs_handheld_rns_t **out);

/* Report backend status: RS_HANDHELD_OK once an identity is loaded AND a transport node is
 * open (the protocol paths are real from that point); RS_HANDHELD_ERR_NOT_READY before that.
 * Null ctx -> RS_HANDHELD_ERR_INVALID_ARG. */
rs_handheld_status_t rs_handheld_rns_status(const rs_handheld_rns_t *ctx);

/* Free a context from rs_handheld_rns_init. Null is a no-op. Free each context exactly once.
 * The caller-owned transport node buffer is NOT touched — free it after this returns. */
void rs_handheld_rns_shutdown(rs_handheld_rns_t *ctx);

/* ---- Identity: byte-exact with Python Reticulum ----
 * identity hash    = sha256(x25519_pub32 || ed25519_pub32)[:16]
 * destination hash = sha256(sha256("lxmf.delivery")[:10] || identity_hash)[:16]
 */

/* Validate a raw 64-byte identity and derive its hashes WITHOUT a context.
 * out_identity_hash[16] is required; out_public_key[64] is optional (may be NULL). */
rs_handheld_status_t rs_handheld_rns_validate_identity(const uint8_t private_key[64],
                                             uint8_t out_identity_hash[16],
                                             uint8_t out_public_key[64]);

/* Derive an identity from caller-supplied entropy: the 64 bytes become the raw private key
 * (any 64 bytes are a valid Reticulum identity). Rust generates no randomness — the caller
 * supplies platform entropy (e.g. esp_random). Writes out_private_key[64] + out_identity_hash[16]. */
rs_handheld_status_t rs_handheld_rns_create_identity(const uint8_t entropy[64],
                                           uint8_t out_private_key[64],
                                           uint8_t out_identity_hash[16]);

/* Load a raw 64-byte identity into the context (becomes the active identity). */
rs_handheld_status_t rs_handheld_rns_load_identity(rs_handheld_rns_t *ctx, const uint8_t private_key[64]);

/* Write the active identity hash (16 bytes). RS_HANDHELD_ERR_NOT_READY if no identity loaded. */
rs_handheld_status_t rs_handheld_rns_identity_hash(const rs_handheld_rns_t *ctx, uint8_t out[16]);

/* Write the active identity's lxmf.delivery destination hash (16 bytes).
 * RS_HANDHELD_ERR_NOT_READY if no identity loaded. */
rs_handheld_status_t rs_handheld_rns_destination_hash(const rs_handheld_rns_t *ctx, uint8_t out[16]);

/* Export the active identity's raw 64-byte private key. RS_HANDHELD_ERR_NOT_READY if none loaded. */
rs_handheld_status_t rs_handheld_rns_export_identity(const rs_handheld_rns_t *ctx, uint8_t out[64]);

/* ---- Announce: Reticulum announce wire format ----
 * announce_data = pub(64) || name_hash(10) || random_hash(10) || [ratchet(32)] || sig(64) || app_data
 * signed_data   = dest_hash(16) || pub(64) || name_hash(10) || random_hash(10) || [ratchet(32)] || app_data
 */

/* Max app_data an announce can carry: the real single-packet wire bound
 * (MTU - HEADER_MINSIZE - fixed fields = 500 - 19 - 148), NOT an arbitrary cap, so ingest accepts
 * any network-valid announce. MUST equal RS_HANDHELD_ANNOUNCE_MAX_APP_DATA in src/lib.rs (a compile-time
 * assert there pins it to 333). */
#define RS_HANDHELD_ANNOUNCE_MAX_APP_DATA 333u

/* A validated inbound announce, for the C++ AnnounceManager. POD; the caller allocates it.
 * Layout MUST match RsHandheldAnnounceEvent in src/lib.rs (#[repr(C)]): same field order/types.
 * has_ratchet is 0/1; when 0 the ratchet bytes are zeroed. app_data holds app_data_len valid
 * bytes (the remainder is zeroed). */
typedef struct rs_handheld_announce_event {
    uint8_t  destination_hash[16];
    uint8_t  identity_hash[16];
    uint8_t  public_key[64];
    int32_t  has_ratchet;
    uint8_t  ratchet[32];
    uint32_t app_data_len;
    uint8_t  app_data[RS_HANDHELD_ANNOUNCE_MAX_APP_DATA];
} rs_handheld_announce_event_t;

/* Build a signed lxmf.delivery announce_data for the active identity into `out` (capacity
 * out_cap); *out_len receives the byte length, out_dest_hash[16] the announce destination.
 * The announce random_hash is composed here as rng_seed(5) || announce_order_be(5). The latter
 * is a persisted 40-bit ordering value, not a duration clock; obtain it from the announce-state
 * prepare/persist/commit transaction below. This side has no RNG/clock. ratchet is optional:
 * pass NULL for an unratcheted fallback (packet context_flag UNSET). A non-NULL ratchet must be
 * the exact current public key from a successfully persisted+committed ring candidate, and
 * announce_order must be the exact committed announce-state value; otherwise this returns
 * RS_HANDHELD_ERR_NOT_READY. Build a ratcheted packet with context_flag SET. app_data may be NULL
 * iff app_data_len == 0. `out` must NOT overlap any input buffer. RS_HANDHELD_ERR_NOT_READY if no
 * identity is loaded or the ratchet/state durability preconditions are not met;
 * RS_HANDHELD_ERR_CAPACITY if `out` is too small or app_data_len > RS_HANDHELD_ANNOUNCE_MAX_APP_DATA. */
rs_handheld_status_t rs_handheld_rns_announce(const rs_handheld_rns_t *ctx,
                                    const uint8_t rng_seed[5], uint64_t announce_order,
                                    const uint8_t ratchet[32],
                                    const uint8_t *app_data, size_t app_data_len,
                                    uint8_t *out, size_t out_cap, size_t *out_len,
                                    uint8_t out_dest_hash[16]);

/* Parse + validate an inbound announce_data (`data`/`data_len`) against dest_hash[16] from the
 * packet header, filling *out_event. context_flag (0/1) MUST equal the packet's flag (it tells
 * the parser whether a ratchet is present). Validation is signature + destination-hash binding,
 * plus (if known_public_key is non-NULL) first-seen public-key continuity (the hash-collision
 * defense): RS_HANDHELD_ERR_CRYPTO if the announced key differs from known_public_key. Announce-
 * acceptance POLICY stays with the C++ AnnounceManager (contact/name cache + KEY MAP). NOTE: no
 * blackhole/ignore list is implemented on either side today (Python's blackholed-identity filter
 * has no firmware equivalent — recorded in the coverage posture). RS_HANDHELD_OK therefore means
 * "signature + binding valid", not "safe to learn unconditionally".
 * RS_HANDHELD_ERR_CRYPTO on bad signature/binding/key; RS_HANDHELD_ERR_CAPACITY if
 * app_data exceeds the cap; RS_HANDHELD_ERR_INVALID_ARG on NULL or malformed (too-short) input. */
rs_handheld_status_t rs_handheld_rns_announce_ingest(const uint8_t *data, size_t data_len,
                                           const uint8_t dest_hash[16], int32_t context_flag,
                                           const uint8_t known_public_key[64],
                                           rs_handheld_announce_event_t *out_event);

/* ---- Transport / packet IO: raw ingress/egress + path request over rns-lite-core ----
 * rsDeck feeds raw Reticulum packet bytes in (RX from the LoRa driver) and polls raw bytes out
 * (TX to the driver); the node does the relay control plane. The LoRaInterface stays C++-owned. */

/* Transport table profiles. Each prebuilt artifact is compiled for exactly ONE profile
 * (prebuilt/xtensa-esp32s3/{small,micro}); the profile argument below is a wrong-artifact
 * tripwire, not a runtime switch. SMALL = tdeck/tpager (node in PSRAM, host-measured
 * 192,896 B); MICRO = cardputer (internal heap, host-measured 31,232 B, <= 32 KB asserted). */
#define RS_HANDHELD_PROFILE_SMALL 0
#define RS_HANDHELD_PROFILE_MICRO 1

/* Reticulum interface modes used by packet_ingest_with_mode. Values match the trusted runtime
 * discriminants; they are ABI constants, not the Rust enum layout. */
#define RS_HANDHELD_IFACE_MODE_FULL         0x01
#define RS_HANDHELD_IFACE_MODE_ACCESS_POINT 0x08
#define RS_HANDHELD_IFACE_MODE_ROAMING      0x10
#define RS_HANDHELD_IFACE_MODE_BOUNDARY     0x20
#define RS_HANDHELD_IFACE_MODE_GATEWAY      0x40

/* Byte size of the transport node for `profile`, or 0 if this artifact was not compiled for
 * `profile` (size the open_transport buffer with this — never hardcode node sizes in C++). */
size_t rs_handheld_rns_transport_size(int32_t profile);

/* Required alignment of the transport node buffer for `profile`, or 0 if this artifact was
 * not compiled for `profile` (heap_caps_malloc/malloc alignment stays checkable). */
size_t rs_handheld_rns_transport_align(int32_t profile);

/* rs_handheld_rns_packet_ingest *out_action codes (IngestAction in src/lib.rs). */
#define RS_HANDHELD_INGEST_ACCEPTED               0
#define RS_HANDHELD_INGEST_DUPLICATE              1
#define RS_HANDHELD_INGEST_LEARNED_ANNOUNCE       2
#define RS_HANDHELD_INGEST_SCHEDULED_ANNOUNCE     3
#define RS_HANDHELD_INGEST_ANSWERED_PATH_REQUEST  4
#define RS_HANDHELD_INGEST_FORWARDED_PATH_REQUEST 5
#define RS_HANDHELD_INGEST_FORWARDED_TRANSPORT    6
#define RS_HANDHELD_INGEST_FORWARDED_PROOF        7
#define RS_HANDHELD_INGEST_DROPPED                8
/* A packet addressed to THIS endpoint (our lxmf.delivery dest, a registered link id, or any
 * inbound PROOF) that the relay core dropped as non-forwardable — packet_ingest filled the
 * rs_handheld_local_frame_t out-param for the C++ LXMF/link/resource engines. */
#define RS_HANDHELD_INGEST_LOCAL_FRAME            9
/* An inbound path request whose requested-hash == our OWN lxmf.delivery dest — the relay core
 * drops it (endpoint has no cached path to answer from), and packet_ingest re-classifies it so
 * the C++ ProtocolRuntime re-announces our dest as a PATH_RESPONSE (throttled). Endpoint-scope only:
 * path requests for OTHER dests stay DROPPED. No out-param — the arrival interface is already in
 * scope at the C++ call site and the re-announce is a fresh signed announce, not tag-bound. */
#define RS_HANDHELD_INGEST_PATH_REQUEST_SELF     10
/* A validated inbound announce for an aspect OTHER than lxmf.delivery (e.g. lxst.telephony,
 * lxmf.propagation, NomadNet). Its path is learned like any announce, but out_event is NOT filled
 * and the C++ must NOT surface it as a contact — this LXMF endpoint neither routes nor lists other
 * aspects (otherwise one identity can appear as several peers). */
#define RS_HANDHELD_INGEST_ANNOUNCE_OTHER        11
/* A signature-valid announce rejected by transport freshness/quality policy. out_event is NOT
 * touched; it must never reach KeyMap, contacts, or peer-ratchet persistence. */
#define RS_HANDHELD_INGEST_ANNOUNCE_IGNORED      12

/* rs_handheld_rns_poll_outbound *out_reason codes (OutboundReason in src/lib.rs). */
#define RS_HANDHELD_TX_ANNOUNCE_REBROADCAST 0
#define RS_HANDHELD_TX_PATH_RESPONSE        1
#define RS_HANDHELD_TX_PATH_REQUEST_FORWARD 2
#define RS_HANDHELD_TX_PATH_REQUEST         3
#define RS_HANDHELD_TX_TRANSPORT_FORWARD    4
#define RS_HANDHELD_TX_PROOF_RETURN         5

/* Transport counters for periodic telemetry. Layout MUST match RsHandheldTransportStats (#[repr(C)]).
 * outbound_dropped = TX frames evicted because the outbound queue was full (backpressure loss);
 * outbound_len = live queue depth right now. */
typedef struct rs_handheld_transport_stats {
    uint64_t accepted;
    uint64_t duplicates;
    uint64_t learned_announces;
    uint64_t queued_outbound;
    uint64_t dropped;
    uint64_t validation_failures;
    uint64_t outbound_dropped;
    uint64_t announces_rate_dropped;
    uint32_t outbound_len;
} rs_handheld_transport_stats_t;

/* rs_handheld_route_t.kind values. A DIRECT route names exactly one live interface; BROADCAST
 * intentionally fans out over every live interface because there is no usable path. */
#define RS_HANDHELD_ROUTE_BROADCAST 0
#define RS_HANDHELD_ROUTE_DIRECT    1

/* Endpoint transmit route selected by Rust from the live path table. header_type is 0 (HEADER_1)
 * for one-hop paths and 1 (HEADER_2) for multi-hop paths; next_hop is zero for HEADER_1. */
typedef struct rs_handheld_route {
    int32_t kind;
    uint8_t interface_id;
    uint8_t header_type;
    uint8_t next_hop[16];
    uint8_t hops;
} rs_handheld_route_t;

/* Largest versioned known-destination blob (SMALL profile: 128 entries). MICRO blobs are smaller;
 * using this cap for both profiles keeps the persistence shim artifact-independent. */
#define RS_HANDHELD_KNOWN_DESTINATIONS_BLOB_MAX 11271u

/* Open the transport node for the context IN PLACE in the CALLER-owned buffer buf[buf_len]:
 * transport_id[16] is this node's transport address, transport_enabled is 1 to participate as
 * a relay (include transport_id in path requests, answer cached path requests) or 0 for
 * endpoint-only. `profile` must be the profile this artifact was compiled with, else
 * RS_HANDHELD_ERR_UNSUPPORTED (wrong-artifact tripwire). buf must be non-NULL, at least
 * rs_handheld_rns_transport_size(profile) bytes and aligned to
 * rs_handheld_rns_transport_align(profile), else RS_HANDHELD_ERR_INVALID_ARG (nothing written).
 * OWNERSHIP: the buffer stays caller-owned and MUST outlive the node's use — Rust stores a
 * pointer, never frees it, and shutdown never touches it (order: resource closes ->
 * shutdown(ctx) -> free(buf)). Allocate from the right per-board pool: SMALL ->
 * heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) on T-Deck/T-Pager; MICRO -> internal
 * heap on the Cardputer. Replaces any previously-opened node (the old node is plain data —
 * abandoned, not freed; the same buffer may be reused). Construction is placement-init per
 * element: no node-sized stack temp is materialized. */
rs_handheld_status_t rs_handheld_rns_open_transport(rs_handheld_rns_t *ctx, int32_t profile,
                                          const uint8_t transport_id[16],
                                          int32_t transport_enabled,
                                          uint8_t *buf, size_t buf_len);

/* Max raw payload a local frame carries (single-packet MDU). MUST equal LOCAL_FRAME_PAYLOAD_MAX
 * (= MTU 500) in src/lib.rs. */
#define RS_HANDHELD_LOCAL_FRAME_PAYLOAD_MAX 500u

/* A packet addressed to this endpoint, for the C++ delivery engines. POD; the caller
 * allocates it. Layout MUST match RsHandheldLocalFrame in src/lib.rs (#[repr(C)]). packet_type is
 * 0 Data / 1 Announce / 2 LinkRequest / 3 Proof; header_type 0 Header1 / 1 Header2; context is the
 * raw PacketContext byte; packet_hash is the full 32-byte hash (the proof-binding input, computed
 * in Rust); payload holds payload_len valid bytes (the rest zeroed). */
typedef struct rs_handheld_local_frame {
    uint8_t  packet_type;
    uint8_t  context;
    uint8_t  header_type;
    uint8_t  hops;
    uint8_t  destination_hash[16];
    uint8_t  packet_hash[32];
    uint32_t payload_len;
    uint8_t  payload[RS_HANDHELD_LOCAL_FRAME_PAYLOAD_MAX];
} rs_handheld_local_frame_t;

/* Feed one raw inbound packet. The compatibility entry point uses the profile's node-wide mode;
 * new interface registries should call packet_ingest_with_mode so learned-path expiry follows the
 * actual receiving interface. *out_action receives an RS_HANDHELD_INGEST_* code. If transport
 * accepted an lxmf.delivery announce (LEARNED_ANNOUNCE or SCHEDULED_ANNOUNCE) and out_event is
 * non-NULL, the validated announce is written to *out_event for the C++ AnnounceManager. Ignored
 * or other-aspect announces never touch out_event. If
 * the packet is a local delivery for this endpoint and out_local is non-NULL, *out_action is
 * RS_HANDHELD_INGEST_LOCAL_FRAME and *out_local is filled — route it to the LXMF/link/resource
 * engines. Either out-param may be NULL to ignore that class. *out_action is
 * RS_HANDHELD_INGEST_PATH_REQUEST_SELF (10) when the dropped frame is a path request for our own
 * lxmf.delivery dest. Immediately consume its exact tag with
 * rs_handheld_rns_take_own_path_request_tag, then answer as a PATH_RESPONSE (throttled in C++).
 * RS_HANDHELD_ERR_NOT_READY if no node is open; RS_HANDHELD_ERR_INVALID_ARG on NULL or a malformed packet. */
rs_handheld_status_t rs_handheld_rns_packet_ingest(rs_handheld_rns_t *ctx, const uint8_t *raw, size_t raw_len,
                                         uint8_t interface_id, uint64_t now_ms,
                                         int32_t *out_action, rs_handheld_announce_event_t *out_event,
                                         rs_handheld_local_frame_t *out_local);
rs_handheld_status_t rs_handheld_rns_packet_ingest_with_mode(rs_handheld_rns_t *ctx,
                                         const uint8_t *raw, size_t raw_len,
                                         uint8_t interface_id, int32_t interface_mode,
                                         uint64_t now_ms, int32_t *out_action,
                                         rs_handheld_announce_event_t *out_event,
                                         rs_handheld_local_frame_t *out_local);

/* Consume the exact 1..16-byte tag associated with the immediately preceding
 * RS_HANDHELD_INGEST_PATH_REQUEST_SELF action. Writes a zero-padded out_tag[16] and its original
 * length. Single-task callers must call before the next packet_ingest. ERR_NOT_READY if there is
 * no pending tag; a successful call consumes it. */
rs_handheld_status_t rs_handheld_rns_take_own_path_request_tag(rs_handheld_rns_t *ctx,
                                                    uint8_t out_tag[16], size_t *out_tag_len);

/* Register / unregister a live link id for inbound routing: while registered, a packet
 * addressed to link_id[16] (or any proof) is reported by packet_ingest as
 * RS_HANDHELD_INGEST_LOCAL_FRAME. register is idempotent and no-ops when the fixed slot set is full
 * (endpoint scope, 8 slots). RS_HANDHELD_ERR_INVALID_ARG on NULL. */
rs_handheld_status_t rs_handheld_rns_link_register(rs_handheld_rns_t *ctx, const uint8_t link_id[16]);
rs_handheld_status_t rs_handheld_rns_link_unregister(rs_handheld_rns_t *ctx, const uint8_t link_id[16]);

/* Pure outbound packet builder. header_type: 0 = HEADER_1/BROADCAST, 1 =
 * HEADER_2/TRANSPORT. HEADER_1 requires transport_id == NULL; HEADER_2 requires a 16-byte
 * transport_id. packet_type and destination_type are the Reticulum wire values 0..3. Hops are
 * zero, the context flag is clear, and context is the raw PacketContext byte. payload may be NULL
 * iff payload_len is zero. Writes at most the 500-byte wire MTU to out and sets *out_len;
 * RS_HANDHELD_ERR_CAPACITY if the packet exceeds MTU or out_cap. Inputs must not overlap out.
 * Use rs_handheld_rns_packet_build_flagged when the context flag must be SET. */
rs_handheld_status_t rs_handheld_rns_packet_build(int32_t header_type, int32_t packet_type,
                                      int32_t destination_type, uint8_t context,
                                      const uint8_t transport_id[16],
                                      const uint8_t destination_hash[16],
                                      const uint8_t *payload, size_t payload_len,
                                      uint8_t *out, size_t out_cap, size_t *out_len);

/* packet_build with an explicit context_flag (0/1). A ratcheted announce MUST pass 1: receivers
 * locate the announce payload's optional ratchet field from this header bit, so a ratchet emitted
 * with the flag clear is mis-parsed and fails signature validation at every peer (Python
 * Destination.announce sets FLAG_SET exactly when a ratchet is attached). */
rs_handheld_status_t rs_handheld_rns_packet_build_flagged(int32_t header_type, int32_t packet_type,
                                      int32_t destination_type, uint8_t context, int32_t context_flag,
                                      const uint8_t transport_id[16],
                                      const uint8_t destination_hash[16],
                                      const uint8_t *payload, size_t payload_len,
                                      uint8_t *out, size_t out_cap, size_t *out_len);

/* AutoInterface pure derivations. format_ipv6 writes canonical lowercase RFC-5952 text plus a
 * trailing NUL (*out_len excludes NUL; 40 bytes always suffice). multicast_group hashes group_id
 * and returns the default Temporary+Link group. beacon_token returns
 * SHA-256(group_id || canonical address text). group_id may be NULL iff its length is zero. */
rs_handheld_status_t rs_handheld_rns_auto_format_ipv6(const uint8_t address[16], char *out,
                                          size_t out_cap, size_t *out_len);
rs_handheld_status_t rs_handheld_rns_auto_multicast_group(const uint8_t *group_id,
                                              size_t group_id_len, uint8_t out[16]);
rs_handheld_status_t rs_handheld_rns_auto_beacon_token(const uint8_t *group_id,
                                           size_t group_id_len, const uint8_t address[16],
                                           uint8_t out[32]);

/* Compute the full 32-byte Reticulum packet hash of raw[raw_len] (TX-side proof binding): the
 * sender needs its own sent packet's hash to track the pending delivery proof. header_type is
 * 0 (Header1) / 1 (Header2). The hash rule stays in Rust. RS_HANDHELD_ERR_INVALID_ARG on NULL or
 * raw_len < 2. */
rs_handheld_status_t rs_handheld_rns_packet_hash(const uint8_t *raw, size_t raw_len,
                                       int32_t header_type, uint8_t out[32]);

/* Poll one queued outbound packet for transmission, draining one frame per call (loop until
 * *out_len == 0 to flush the queue). Writes raw bytes to out[out_cap], length to *out_len (0 =
 * queue empty), the originating *out_interface_id, and an RS_HANDHELD_TX_* *out_reason. A frame can be
 * up to MTU (500) bytes; on RS_HANDHELD_ERR_CAPACITY (out too small) the frame stays queued and is NOT
 * lost — retry with a larger buffer. RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_poll_outbound(rs_handheld_rns_t *ctx, uint8_t *out, size_t out_cap,
                                         size_t *out_len, uint8_t *out_interface_id,
                                         int32_t *out_reason);

/* Run time-based maintenance (table expiry + dispatch of due announce rebroadcasts). Call
 * periodically with a monotonic millisecond clock. RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_tick(rs_handheld_rns_t *ctx, uint64_t now_ms);

/* Write 1/0 to *out_has_path for whether a live path to destination_hash[16] is known. */
rs_handheld_status_t rs_handheld_rns_has_path(const rs_handheld_rns_t *ctx, const uint8_t destination_hash[16],
                                    uint64_t now_ms, int32_t *out_has_path);

/* Write the number of LIVE (unexpired) paths in the node's path table to *out_count
 * (the cumulative count differs from current paths: the cumulative learned_announces stat over-reports once paths
 * expire). RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_path_count(const rs_handheld_rns_t *ctx, uint64_t now_ms,
                                      uint32_t *out_count);

/* Read the live path to destination_hash[16] for an endpoint TX decision (header framing +
 * Identity.recall-style gating): writes 1/0 to *out_has_path, and when known the hop count to
 * *out_hops, the next-hop transport id to out_next_hop[16] (zeroed + *out_has_next_hop=0 when the
 * path is direct/1-hop), and the announced 64-byte public key to out_public_key (optional, NULL to
 * ignore). Use hops (1 -> HEADER_1 broadcast; >1 -> HEADER_2 to next_hop) + the key for LXMF ECIES
 * + proof binding. RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_path_info(const rs_handheld_rns_t *ctx,
                                     const uint8_t destination_hash[16], uint64_t now_ms,
                                     int32_t *out_has_path, uint8_t *out_hops,
                                     uint8_t out_next_hop[16], int32_t *out_has_next_hop,
                                     uint8_t out_public_key[64]);

/* Select an endpoint TX route from the live Rust path table. Known 1-hop paths are DIRECT/H1;
 * known multi-hop paths are DIRECT/H2/next_hop; missing or expired paths are BROADCAST/H1.
 * A DIRECT caller transmits only on route.interface_id. */
rs_handheld_status_t rs_handheld_rns_route(const rs_handheld_rns_t *ctx,
                                 const uint8_t destination_hash[16], uint64_t now_ms,
                                 rs_handheld_route_t *out_route);

/* Originate a path request for destination_hash[16] and queue it (poll via poll_outbound).
 * tag[16] is a caller-supplied random request tag. RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_request_path(rs_handheld_rns_t *ctx, const uint8_t destination_hash[16],
                                        const uint8_t tag[16], uint8_t interface_id,
                                        uint64_t now_ms);

/* Write the transport counters + live outbound depth. RS_HANDHELD_ERR_NOT_READY if no node is open. */
rs_handheld_status_t rs_handheld_rns_transport_stats(const rs_handheld_rns_t *ctx,
                                           rs_handheld_transport_stats_t *out_stats);

/* Lower/restore the announce-verification budget under host pressure. open_transport starts at
 * steady=5/s, grace=3/s for the first 60 seconds. Zero disables limiting for that phase; this
 * setter does not change the 60-second grace duration. */
rs_handheld_status_t rs_handheld_rns_set_announce_budget(rs_handheld_rns_t *ctx,
                                               uint16_t steady_per_sec,
                                               uint16_t grace_per_sec);

/* Rust-owned durable LXMF delivery destination memory. recall returns OK with *out_found=0 and a
 * zero key when absent; successful recall refreshes LRU recency. learn returns *out_changed=0 for
 * an identical binding and ERR_CRYPTO for a conflict/destination mismatch. export/import use a
 * complete versioned self-validating blob; failed import leaves live state unchanged. */
rs_handheld_status_t rs_handheld_rns_known_dest_recall(rs_handheld_rns_t *ctx,
                                             const uint8_t destination_hash[16],
                                             uint8_t out_public_key[64], int32_t *out_found);
rs_handheld_status_t rs_handheld_rns_known_dest_learn(rs_handheld_rns_t *ctx,
                                            const uint8_t destination_hash[16],
                                            const uint8_t public_key[64], uint64_t now_ms,
                                            int32_t *out_changed);
rs_handheld_status_t rs_handheld_rns_known_dest_count(const rs_handheld_rns_t *ctx,
                                            uint32_t *out_count);
rs_handheld_status_t rs_handheld_rns_known_dest_export(const rs_handheld_rns_t *ctx,
                                             uint8_t *out, size_t out_cap, size_t *out_len);
rs_handheld_status_t rs_handheld_rns_known_dest_import(rs_handheld_rns_t *ctx,
                                             const uint8_t *blob, size_t blob_len,
                                             uint64_t now_ms);

/* ---- LXMF single-frame: opportunistic send/receive, byte-exact with Python LXMF 1.0.1 ----
 * plaintext message = dest(16) || source(16) || sig(64) || msgpack([timestamp, title, content, {}])
 * opportunistic packet payload = ECIES_to_recipient(message[16..])  (the RNS header carries dest). */

/* Inline caps for the decoded message event = the full single-frame payload budget (MAX_LXMF_PAYLOAD),
 * so a valid single-frame title/content is never wrongly rejected. MUST equal the src/lib.rs values
 * (a compile-time assert there pins both to 303). */
#define RS_HANDHELD_LXMF_TITLE_MAX   303u
#define RS_HANDHELD_LXMF_CONTENT_MAX 303u
/* Exact-key hint returned by lxmf_peek_source_hint for the base identity key. Other valid hints
 * are retained-ring indices 0..63. */
#define RS_HANDHELD_LXMF_BASE_KEY_HINT 255u

/* A validated, decrypted inbound single-frame LXMF message, for the C++ MessageStore. POD; the
 * caller allocates it. Layout MUST match RsHandheldLxmfMessage in src/lib.rs (#[repr(C)]). title/content
 * hold *_len valid bytes (the remainder is zeroed). */
typedef struct rs_handheld_lxmf_message {
    uint8_t  message_id[32];
    uint8_t  source_hash[16];
    double   timestamp;
    uint32_t title_len;
    uint8_t  title[RS_HANDHELD_LXMF_TITLE_MAX];
    uint32_t content_len;
    uint8_t  content[RS_HANDHELD_LXMF_CONTENT_MAX];
    /* 1 if the message carries a reaction (standard FIELD_REACTION 0x40 or the legacy Ratspeak
     * envelope). Proof it, but skip store/UI (reactions are not displayed). */
    uint8_t  is_reaction;
} rs_handheld_lxmf_message_t;

/* Build a single-frame OPPORTUNISTIC LXMF message from the active identity (source) to
 * recipient_public_key[64] (from the recipient's announce). Writes the ECIES-encrypted RNS packet
 * payload into out[out_cap] (transmit it to *out_dest_hash, the recipient's lxmf.delivery dest),
 * *out_len its length, the 32-byte *out_message_id. timestamp is the LXMF message timestamp.
 * wall_secs (0 when unavailable) and current-boot uptime_ms are separate local clocks used only
 * for peer-ratchet expiry; neither is serialized into the message.
 * ephemeral_priv[32] + iv[16] are caller entropy (fresh per message on-device; fixed for vectors).
 * title/content may be NULL iff their length is 0; `out` must not overlap inputs. RS_HANDHELD_ERR_NOT_READY
 * if no identity is loaded; RS_HANDHELD_ERR_CAPACITY if too large for one frame or `out` too small. */
rs_handheld_status_t rs_handheld_rns_lxmf_build(const rs_handheld_rns_t *ctx,
                                      const uint8_t recipient_public_key[64], double timestamp,
                                      uint64_t wall_secs, uint64_t uptime_ms,
                                      const uint8_t *title, size_t title_len,
                                      const uint8_t *content, size_t content_len,
                                      const uint8_t ephemeral_priv[32], const uint8_t iv[16],
                                      uint8_t *out, size_t out_cap, size_t *out_len,
                                      uint8_t out_dest_hash[16], uint8_t out_message_id[32]);

/* Decrypt + validate an inbound single-frame OPPORTUNISTIC LXMF packet payload (data/data_len,
 * addressed to the active identity) signed by the identity whose public key is source_public_key[64]
 * (from the source's announce). Fills *out_message. RS_HANDHELD_ERR_CRYPTO if decryption, the signature,
 * or the source-hash binding fails; RS_HANDHELD_ERR_CAPACITY if title/content exceed the caps;
 * RS_HANDHELD_ERR_NOT_READY if no identity is loaded; RS_HANDHELD_ERR_INVALID_ARG on NULL/malformed input. */
rs_handheld_status_t rs_handheld_rns_lxmf_parse(const rs_handheld_rns_t *ctx, const uint8_t *data, size_t data_len,
                                      const uint8_t source_public_key[64],
                                      rs_handheld_lxmf_message_t *out_message);

/* Decrypt-only PEEK of an inbound OPPORTUNISTIC payload's embedded 16-byte source hash, so
 * C++ can recall the source public key BEFORE the full validating lxmf_parse (Python Identity.recall
 * flow — the source is unknown at arrival). Fail-closed: reveals only the hash, validates/stores
 * nothing. RS_HANDHELD_ERR_NOT_READY if no identity; RS_HANDHELD_ERR_CRYPTO on decrypt failure;
 * RS_HANDHELD_ERR_INVALID_ARG on NULL/malformed. */
rs_handheld_status_t rs_handheld_rns_lxmf_peek_source(const rs_handheld_rns_t *ctx,
                                            const uint8_t *data, size_t data_len,
                                            uint8_t out_source_hash[16]);

/* Preferred two-pass receive API. peek_source_hint scans retained ratchets newest-first and then
 * the base key, returning both the decrypted source hash and the exact authenticated key hint.
 * After recalling source_public_key, pass that hint to parse_hint; it tries exactly one key and
 * never rescans/falls back. A stale, wrong, or out-of-range hint fails closed with
 * RS_HANDHELD_ERR_CRYPTO. This bounds a 64-key receive to one scan plus one decrypt. */
rs_handheld_status_t rs_handheld_rns_lxmf_peek_source_hint(const rs_handheld_rns_t *ctx,
                                                 const uint8_t *data, size_t data_len,
                                                 uint8_t out_source_hash[16],
                                                 uint8_t *out_key_hint);
rs_handheld_status_t rs_handheld_rns_lxmf_parse_hint(const rs_handheld_rns_t *ctx,
                                            const uint8_t *data, size_t data_len,
                                            uint8_t key_hint,
                                            const uint8_t source_public_key[64],
                                            rs_handheld_lxmf_message_t *out_message);

/* Preferred self-resolving opportunistic receive API. Decrypts the embedded source hash, resolves
 * its public key from the Rust known-destination table then the volatile path table, and validates
 * the message with that key. On success out_resolved_public_key receives the authenticated key.
 * Returns RS_HANDHELD_ERR_SOURCE_UNKNOWN when neither Rust table knows the source; callers request
 * its path and drop without emitting a delivery proof. The older peek/parse APIs remain compatible. */
rs_handheld_status_t rs_handheld_rns_lxmf_parse_auto(rs_handheld_rns_t *ctx,
                                           const uint8_t *data, size_t data_len,
                                           rs_handheld_lxmf_message_t *out_message,
                                           uint8_t out_resolved_public_key[64]);

/* Build a FULL packed LXMF message for LINK/RESOURCE (DIRECT) delivery: dest(16) ||
 * source(16) || signature(64) || msgpack payload — NO ECIES wrap (link session crypto replaces it),
 * byte-identical to Python LXMessage.pack(). Send it either as a single link data packet
 * (link-encrypted) when it fits the link MDU, or as a resource. Writes the packed bytes to out,
 * *out_len its length, *out_dest_hash (16) the recipient lxmf.delivery dest, *out_message_id (32).
 * title/content may be NULL iff their length is 0. RS_HANDHELD_ERR_NOT_READY if no identity;
 * RS_HANDHELD_ERR_CAPACITY if too large / out too small. */
rs_handheld_status_t rs_handheld_rns_lxmf_build_link(const rs_handheld_rns_t *ctx,
                                           const uint8_t recipient_public_key[64], double timestamp,
                                           const uint8_t *title, size_t title_len,
                                           const uint8_t *content, size_t content_len,
                                           uint8_t *out, size_t out_cap, size_t *out_len,
                                           uint8_t out_dest_hash[16], uint8_t out_message_id[32]);

/* Validate a FULL packed LXMF message received over a link / assembled from a resource:
 * dest(16) || source(16) || signature(64) || msgpack payload. The active identity must be the
 * delivery dest (RS_HANDHELD_ERR_CRYPTO on mismatch/forgery); source_public_key is recalled by the
 * caller from data[16..32]. Writes fields into caller buffers (link/resource content can exceed the
 * single-frame event caps — size title/content to RS_HANDHELD_RESOURCE_DATA_MAX). title/content are
 * truncated to their capacities; *out_*_len receive the TRUE lengths. *out_is_reaction receives 1
 * if the message carries a reaction (standard 0x40 or legacy Ratspeak envelope) — proof it, skip
 * store/UI. RS_HANDHELD_ERR_NOT_READY if no identity; RS_HANDHELD_ERR_CRYPTO on
 * decrypt/signature/binding failure; RS_HANDHELD_ERR_INVALID_ARG on NULL/malformed. */
rs_handheld_status_t rs_handheld_rns_lxmf_parse_link(const rs_handheld_rns_t *ctx,
                                           const uint8_t *data, size_t data_len,
                                           const uint8_t source_public_key[64],
                                           uint8_t out_message_id[32], uint8_t out_source_hash[16],
                                           double *out_timestamp,
                                           uint8_t *title, size_t title_cap, size_t *out_title_len,
                                           uint8_t *content, size_t content_cap,
                                           size_t *out_content_len, int32_t *out_is_reaction);

/* ---- Proof / delivery-status / dedup: byte-exact with Python RNS 1.3.8 ----
 * A proof of receipt is the receiver's Ed25519 signature over the proven packet's full 32-byte hash:
 * implicit = signature(64) (Reticulum default); explicit = packet_hash(32) || signature(64). The
 * sender maps a valid proof -> DELIVERED. Status-transition timing (proof timeout / retry cap /
 * terminal failure) stays C++/ratdeck-owned; the Rust side provides the proof mechanism + dedup. */

/* Max proof bytes (explicit). Implicit is 64. */
#define RS_HANDHELD_PROOF_MAX 96u

/* Build a proof of receipt for the packet whose full 32-byte hash is packet_hash, signed by the
 * active identity. implicit != 0 -> signature(64); else packet_hash(32) || signature(64). Writes the
 * proof to out[out_cap] (>= 64, or >= 96 for explicit) and *out_len. RS_HANDHELD_ERR_NOT_READY if no
 * identity is loaded; RS_HANDHELD_ERR_CAPACITY if out is too small. */
rs_handheld_status_t rs_handheld_rns_proof_build(const rs_handheld_rns_t *ctx, const uint8_t packet_hash[32],
                                       int32_t implicit, uint8_t *out, size_t out_cap,
                                       size_t *out_len);

/* Validate a proof of receipt for packet_hash[32], signed by the identity whose public key is
 * prover_public_key[64]. Writes 1/0 to *out_valid. Stateless. Accepts an implicit (64) or explicit
 * (96, hash-prefixed) proof. RS_HANDHELD_ERR_INVALID_ARG on NULL.
 * BINDING CONTRACT (caller's responsibility — this only proves "prover_public_key signed packet_hash"):
 * pass packet_hash = the ORIGINAL sent packet's own 32-byte hash, and prover_public_key = THAT
 * message's recipient identity public key. For an IMPLICIT proof (no embedded hash) iterate each
 * outstanding sent packet's (hash, recipient_key) and accept DELIVERED only on the first that
 * validates; for an EXPLICIT proof match its hash prefix to the pending receipt first. Never mark an
 * arbitrary pending message DELIVERED on a bare valid proof. */
rs_handheld_status_t rs_handheld_rns_proof_validate(const uint8_t prover_public_key[64],
                                          const uint8_t packet_hash[32], const uint8_t *proof,
                                          size_t proof_len, int32_t *out_valid);

/* Record an inbound message id for duplicate suppression. Writes 1 to *out_is_new if message_id[32]
 * is NEW (process + store it) or 0 if already seen (drop it). Bounded FIFO set (MAX_SEEN_IDS = 100).
 * IN-MEMORY ONLY: the set is empty after init and does NOT persist — seed it at startup from storage
 * via rs_handheld_rns_seed_seen_message (replaying loadRecentMessageIds(MAX_SEEN_IDS)) or the cross-reboot
 * dedup contract does not hold. RS_HANDHELD_ERR_INVALID_ARG on NULL. */
rs_handheld_status_t rs_handheld_rns_seen_message(rs_handheld_rns_t *ctx, const uint8_t message_id[32],
                                        int32_t *out_is_new);

/* Read-only duplicate check. Persist a new message before seed_seen_message and
 * delivery proof; failed storage must leave the id retryable. */
rs_handheld_status_t rs_handheld_rns_has_seen_message(const rs_handheld_rns_t *ctx,
                                        const uint8_t message_id[32], int32_t *out_seen);

/* Seed the seen-message set with a known/stored id at startup (no is_new output). Call once per id
 * after rs_handheld_rns_init, replaying the recently-stored message ids from persistent storage, so a
 * post-reboot retry of a stored message is recognised as a duplicate. RS_HANDHELD_ERR_INVALID_ARG on NULL. */
rs_handheld_status_t rs_handheld_rns_seed_seen_message(rs_handheld_rns_t *ctx, const uint8_t message_id[32]);

/* ---- Link establishment + session encryption: byte-exact with rsReticulum rns-link ----
 * The Reticulum LINK handshake (ephemeral ECDH + an identity-signed proof) and AES-256 session
 * encryption. rsDeck is INITIATOR when opening a link to send a large message, RESPONDER when a peer
 * links to it. These are the link cryptographic + wire primitives; the C++/ratdeck owner drives the
 * state machine, timers and packet send/receive, and holds each link's 64-byte session key for the
 * link's lifetime (passing it back per frame, and zeroizing it on close). The Rust side holds no link
 * state. Handshake:
 *   1. LINKREQUEST (initiator->dest): x25519_pub(32) || ed25519_pub(32) || signalling(3)
 *      link_id = truncated_hash(0x02 || dest_hash || 0x00 || x25519_pub || ed25519_pub)
 *   2. LRPROOF (dest->initiator): signature(64) || responder_x25519_pub(32) || signalling(3)
 *      signature = identity.sign(link_id || responder_x25519_pub || identity_ed25519_pub || signalling)
 *   session key = HKDF(64, ECDH(ephemerals), salt=link_id) = signing(32) || encryption(32).
 * AES-256-CBC is the only enabled mode; a non-AES-256 mode is refused (build -> RS_HANDHELD_ERR_UNSUPPORTED,
 * validate -> *out_valid=0). On all build/encrypt functions, read *out_len only when the call returns
 * RS_HANDHELD_OK (it is untouched on error; initialize it beforehand). */

#define RS_HANDHELD_LINK_REQUEST_LEN 67u  /* x25519_pub(32) + ed25519_pub(32) + signalling(3) */
#define RS_HANDHELD_LINK_PROOF_LEN   99u  /* signature(64) + responder_x25519_pub(32) + signalling(3) */
#define RS_HANDHELD_LINK_KEY_LEN     64u  /* signing(32) + encryption(32) */

/* INITIATOR: build a LINKREQUEST. x25519_priv[32] + ed25519_seed[32] are per-link EPHEMERAL key
 * material (caller entropy; no RNG here) — retain x25519_priv for rs_handheld_rns_link_derive. mode MUST be
 * 1 (AES-256); any other value -> RS_HANDHELD_ERR_UNSUPPORTED. mtu is masked to 21 bits (use 500). Writes
 * the payload to out[out_cap] and *out_len (67). RS_HANDHELD_ERR_CAPACITY if out is too small. */
rs_handheld_status_t rs_handheld_rns_link_request_build(const uint8_t x25519_priv[32],
                                              const uint8_t ed25519_seed[32], uint8_t mode,
                                              uint32_t mtu, uint8_t *out, size_t out_cap,
                                              size_t *out_len);

/* RESPONDER: parse a LINKREQUEST. out_x25519_pub[32] (required) is the initiator's ephemeral X25519
 * public key for rs_handheld_rns_link_derive; out_ed25519_pub[32], out_mode, out_mtu are optional (NULL to
 * ignore). The legacy 64-byte (keys-only) form is also accepted, defaulting to AES-256/MTU-500.
 * RS_HANDHELD_ERR_INVALID_ARG on NULL required args or a length that is neither 64 nor 67. */
rs_handheld_status_t rs_handheld_rns_link_request_parse(const uint8_t *data, size_t data_len,
                                              uint8_t out_x25519_pub[32], uint8_t out_ed25519_pub[32],
                                              uint8_t *out_mode, uint32_t *out_mtu);

/* Compute the 16-byte link id from the destination hash + the LINKREQUEST payload (both peers must
 * agree on it; it salts the session key). Stateless. RS_HANDHELD_ERR_INVALID_ARG on NULL. */
rs_handheld_status_t rs_handheld_rns_link_id(const uint8_t dest_hash[16], const uint8_t *request,
                                   size_t request_len, uint8_t out_link_id[16]);

/* RESPONDER: build an LRPROOF, signed with the ACTIVE identity's long-term Ed25519 key.
 * responder_x25519_priv[32] is the responder's per-link EPHEMERAL X25519 key (retain it for
 * rs_handheld_rns_link_derive). mode MUST be 1 (AES-256) -> else RS_HANDHELD_ERR_UNSUPPORTED; mtu masked to 21
 * bits. Writes the payload to out[out_cap] and *out_len (99). RS_HANDHELD_ERR_NOT_READY if no identity is
 * loaded; RS_HANDHELD_ERR_CAPACITY if out is too small. */
rs_handheld_status_t rs_handheld_rns_link_proof_build(const rs_handheld_rns_t *ctx,
                                            const uint8_t responder_x25519_priv[32],
                                            const uint8_t link_id[16], uint8_t mode, uint32_t mtu,
                                            uint8_t *out, size_t out_cap, size_t *out_len);

/* INITIATOR: validate an LRPROOF against the destination identity's public key (from its announce) +
 * the link id. Writes 1/0 to *out_valid; on a structurally valid proof, the responder's ephemeral
 * X25519 public key to out_responder_x25519_pub[32] (optional, NULL to ignore — needed for
 * rs_handheld_rns_link_derive). A malformed length, a non-AES-256 mode, or a bad signature -> *out_valid = 0
 * with RS_HANDHELD_OK (not an arg error). The legacy 96-byte proof is accepted. Stateless.
 * RS_HANDHELD_ERR_INVALID_ARG only on NULL required args.
 * ACTIVATION CONTRACT (the C++ link state machine's job — NOT done here): a valid proof authenticates
 * the responder but does NOT activate the link. After this returns valid, the initiator must
 * rs_handheld_rns_link_derive the session key, then send an LRRTT packet — a rs_handheld_rns_link_encrypt-wrapped
 * msgpack float of the measured RTT — to the destination. The RESPONDER's link becomes ACTIVE only on
 * receiving + decrypting that RTT packet (handshake message 3); skip it and the responder times out and
 * tears the link down. RTT measurement, msgpack framing, send sequencing, and the ACTIVE/STALE/CLOSED
 * state machine are all C++-owned. */
rs_handheld_status_t rs_handheld_rns_link_proof_validate(const uint8_t identity_public_key[64],
                                               const uint8_t link_id[16], const uint8_t *proof,
                                               size_t proof_len,
                                               uint8_t out_responder_x25519_pub[32],
                                               int32_t *out_valid);

/* Derive the 64-byte link session key (signing(32) || encryption(32)) into out_key. Both peers call
 * this with their own ephemeral X25519 private key + the peer's ephemeral X25519 public key + the
 * shared link_id and arrive at the identical key. The C++ owner stores out_key for the link's lifetime
 * and MUST zeroize it on close. Stateless. RS_HANDHELD_ERR_INVALID_ARG on NULL. */
rs_handheld_status_t rs_handheld_rns_link_derive(const uint8_t my_x25519_priv[32],
                                       const uint8_t peer_x25519_pub[32], const uint8_t link_id[16],
                                       uint8_t out_key[64]);

/* Encrypt one link frame with the session key[64]. Output: IV(16) || AES-256-CBC(PKCS7(pt)) ||
 * HMAC-SHA256(32), written to out[out_cap] and *out_len. iv[16] is caller entropy and MUST be fresh
 * per frame (reuse leaks the CBC key-stream). pt may be NULL iff pt_len == 0. out must NOT overlap any
 * input (pt/iv/key) — encrypt writes in place. RS_HANDHELD_ERR_CAPACITY if too large / out too small;
 * RS_HANDHELD_ERR_CRYPTO on failure. */
rs_handheld_status_t rs_handheld_rns_link_encrypt(const uint8_t key[64], const uint8_t *pt, size_t pt_len,
                                        const uint8_t iv[16], uint8_t *out, size_t out_cap,
                                        size_t *out_len);

/* Decrypt one link frame with the session key[64]. Writes the plaintext to out[out_cap] and *out_len.
 * Every malformed/forged frame returns RS_HANDHELD_ERR_CRYPTO (no padding/HMAC oracle). RS_HANDHELD_ERR_CAPACITY
 * if out is too small. */
rs_handheld_status_t rs_handheld_rns_link_decrypt(const uint8_t key[64], const uint8_t *data, size_t data_len,
                                        uint8_t *out, size_t out_cap, size_t *out_len);

/* ---- Resource transfer: single-segment chunked transfer over an established link ----
 * Byte-exact with Python RNS 1.3.8 RNS.Resource (pinned vectors + live interop). SCOPE SPLIT
 * (mirrors proof/link): the Rust side provides the resource MECHANISM — advertisement codec,
 * blob encryption + part chunking/hashing, request/serve matching, bounded reassembly, delivery
 * proof. The C++ owner drives the transfer STATE MACHINE: when to advertise, request,
 * retransmit, grow/shrink the window (it has the clock), time out, and close — and it holds the
 * 64-byte link session key (rs_handheld_rns_link_derive), passing it to advertise_build /
 * assemble per call. Rust holds no key material between calls; zeroize the key on link close.
 *
 * One outbound + one inbound transfer per context (single-resource staged scope; a new
 * build/accept replaces the previous transfer, close frees the ~3.7 KiB buffers). Wire mapping:
 *   ADV      -> link-encrypted, packet context RESOURCE_ADV
 *   part     -> RAW ciphertext chunk, packet context RESOURCE (NO further encryption: the blob
 *               is already one Token ciphertext over random_hash || data)
 *   request  -> link-encrypted, packet context RESOURCE_REQ
 *   proof    -> link-encrypted PROOF packet, context RESOURCE_PRF
 * Packet framing stays C++-owned. Honest-subset bounds (fail-closed): at most
 * RS_HANDHELD_RESOURCE_MAX_PARTS parts / RS_HANDHELD_RESOURCE_DATA_MAX payload bytes;
 * uncompressed, single-segment, non-request/response, encrypted resources only — anything else
 * is refused at accept time (the peer sees a declined transfer). */

#define RS_HANDHELD_LINK_MDU                 431u /* link plaintext MDU (ADV budget) */
#define RS_HANDHELD_RESOURCE_SDU             464u /* part payload size (raw packet budget) */
#define RS_HANDHELD_RESOURCE_MAX_PARTS         8u
#define RS_HANDHELD_RESOURCE_TRANSFER_MAX   3712u /* MAX_PARTS * SDU (encrypted blob) */
#define RS_HANDHELD_RESOURCE_DATA_MAX       3659u /* max plaintext payload */
#define RS_HANDHELD_RESOURCE_ADV_MAX         192u /* worst-case packed ADV */
#define RS_HANDHELD_RESOURCE_REQUEST_MAX      65u /* worst-case part request */
#define RS_HANDHELD_RESOURCE_PROOF_LEN        64u /* resource_hash(32) || proof(32) */
#define RS_HANDHELD_RESOURCE_RANDOM_HASH_LEN   4u

/* SENDER: open an outbound resource for data[data_len] and emit its packed advertisement.
 * Token-encrypts random_hash || data with the link session key[64], chunks the ciphertext into
 * SDU parts and derives the hashmap. Writes the msgpack ADV to out_adv[out_adv_cap] and its length to *out_adv_len,
 * the part count to *out_num_parts, and (optionally, NULL to ignore) the resource hash to
 * out_resource_hash[32]. Replaces any previous outbound transfer. random_hash[4] + iv[16] are
 * caller entropy, FRESH per resource (fixed only for vectors). data may be NULL iff data_len == 0.
 * RS_HANDHELD_ERR_CAPACITY if data_len > RS_HANDHELD_RESOURCE_DATA_MAX or out_adv is too small;
 * RS_HANDHELD_ERR_RETRY on a map-hash collision (retry with fresh random_hash);
 * RS_HANDHELD_ERR_CRYPTO on encryption failure. */
rs_handheld_status_t rs_handheld_rns_resource_advertise_build(rs_handheld_rns_t *ctx,
                                                              const uint8_t key[64],
                                                              const uint8_t *data, size_t data_len,
                                                              const uint8_t random_hash[4],
                                                              const uint8_t iv[16],
                                                              uint8_t *out_adv, size_t out_adv_cap,
                                                              size_t *out_adv_len,
                                                              uint32_t *out_num_parts,
                                                              uint8_t out_resource_hash[32]);

/* SENDER: copy part `index` (raw ciphertext chunk, at most RS_HANDHELD_RESOURCE_SDU bytes) of the
 * open outbound resource into out[out_cap] (length to *out_len). Send as a RESOURCE-context packet WITHOUT
 * further encryption. RS_HANDHELD_ERR_NOT_READY if no outbound transfer is open;
 * RS_HANDHELD_ERR_INVALID_ARG if index >= num_parts; RS_HANDHELD_ERR_CAPACITY if out is small. */
rs_handheld_status_t rs_handheld_rns_resource_part_emit(const rs_handheld_rns_t *ctx, uint32_t index,
                                                        uint8_t *out, size_t out_cap,
                                                        size_t *out_len);

/* SENDER: parse an inbound RESOURCE_REQ payload (link-decrypted by the caller) and resolve the
 * part indices to (re)send, in wire order. out_indices receives up to
 * RS_HANDHELD_RESOURCE_MAX_PARTS uint32 indices, *out_count how many are valid; emit each via
 * rs_handheld_rns_resource_part_emit. A request for a different resource hash yields
 * *out_count = 0 with RS_HANDHELD_OK (requests are routed by hash; non-matching ones are
 * ignored). RS_HANDHELD_ERR_NOT_READY if no outbound transfer is open;
 * RS_HANDHELD_ERR_INVALID_ARG on a malformed request. */
rs_handheld_status_t rs_handheld_rns_resource_request_serve(const rs_handheld_rns_t *ctx,
                                                            const uint8_t *req, size_t req_len,
                                                            uint32_t *out_indices,
                                                            uint32_t *out_count);

/* SENDER: check a delivery proof (link-decrypted RESOURCE_PRF payload) against the open outbound
 * resource. Writes 1/0 to *out_valid; 1 means the receiver reassembled the exact payload — mark
 * the transfer DELIVERED and close it. Wrong-length/non-matching proofs are *out_valid = 0 with
 * RS_HANDHELD_OK. RS_HANDHELD_ERR_NOT_READY if no outbound transfer is open. */
rs_handheld_status_t rs_handheld_rns_resource_proof_validate(const rs_handheld_rns_t *ctx,
                                                             const uint8_t *proof, size_t proof_len,
                                                             int32_t *out_valid);

/* SENDER: close the open outbound transfer (delivered, failed or timed out — the C++ owner
 * decides when). Frees the transfer buffer. No-op if none is open. */
rs_handheld_status_t rs_handheld_rns_resource_outbound_close(rs_handheld_rns_t *ctx);

/* RECEIVER: parse an advertisement payload (link-decrypted RESOURCE_ADV) and accept the transfer,
 * replacing any previous inbound transfer. Fail-closed honest subset:
 * RS_HANDHELD_ERR_UNSUPPORTED for compressed / multi-segment / metadata / request-response /
 * unencrypted resources; RS_HANDHELD_ERR_CAPACITY beyond the lite bounds;
 * RS_HANDHELD_ERR_INVALID_ARG for a malformed or internally inconsistent ADV. On RS_HANDHELD_OK,
 * *out_num_parts / *out_transfer_size / *out_data_size receive the advertised geometry and
 * out_resource_hash[32] (optional, NULL to ignore) the resource hash. */
/* Read a valid ADV's segment hash without accepting it, including oversized hashmaps.
 * The host sends this hash in link-encrypted RESOURCE_RCL when declining a transfer. */
rs_handheld_status_t rs_handheld_rns_resource_advertisement_hash(const uint8_t *adv,
    size_t adv_len, uint8_t out_resource_hash[32]);

rs_handheld_status_t rs_handheld_rns_resource_advertise_accept(rs_handheld_rns_t *ctx,
                                                               const uint8_t *adv, size_t adv_len,
                                                               uint32_t *out_num_parts,
                                                               uint32_t *out_transfer_size,
                                                               uint32_t *out_data_size,
                                                               uint8_t out_resource_hash[32]);

/* RECEIVER: ingest one received part (raw RESOURCE-context packet payload). Writes 1/0 to
 * *out_new (1 = filled a previously-empty slot; duplicates, tampered parts and out-of-window
 * arrivals are 0 — not an error) and 1/0 to *out_complete (all parts present -> call
 * rs_handheld_rns_resource_assemble). RS_HANDHELD_ERR_NOT_READY if no inbound transfer is open. */
rs_handheld_status_t rs_handheld_rns_resource_part_ingest(rs_handheld_rns_t *ctx,
                                                          const uint8_t *part, size_t part_len,
                                                          int32_t *out_new, int32_t *out_complete);

/* RECEIVER: emit a RESOURCE_REQ for the missing parts in the current receive window into
 * out[out_cap] (length to *out_len; size out to RS_HANDHELD_RESOURCE_REQUEST_MAX); send it link-encrypted
 * with context RESOURCE_REQ. Covers at most `window` parts per round; the retry/timeout cadence
 * is C++-owned. RS_HANDHELD_ERR_NOT_READY if no inbound transfer is open or nothing is left to
 * request (transfer complete/assembled/corrupt); RS_HANDHELD_ERR_CAPACITY if out is too small. */
rs_handheld_status_t rs_handheld_rns_resource_request_build(const rs_handheld_rns_t *ctx,
                                                            uint8_t *out, size_t out_cap,
                                                            size_t *out_len);

/* RECEIVER window control (C++-owned timing; the window bounds the next request_build round).
 * Call grow when a requested batch completed — rate_bytes_per_sec is the observed transfer rate,
 * 0 when unknown (neutral) — and shrink on a request timeout. RS_HANDHELD_ERR_NOT_READY if no
 * inbound transfer is open. */
rs_handheld_status_t rs_handheld_rns_resource_window_grow(rs_handheld_rns_t *ctx,
                                                          uint32_t rate_bytes_per_sec);
rs_handheld_status_t rs_handheld_rns_resource_window_shrink(rs_handheld_rns_t *ctx);

/* RECEIVER: reassemble the completed transfer — Token-decrypt the parts with the link session
 * key[64], verify the resource hash over the plaintext, copy the payload into out[out_cap] and
 * its length to *out_len. Then emit the delivery proof via rs_handheld_rns_resource_proof_build.
 * RS_HANDHELD_ERR_NOT_READY if no inbound transfer is open or parts are missing;
 * RS_HANDHELD_ERR_CAPACITY if out_cap is below this transfer's WORST-CASE plaintext — the
 * advertised data_size rounded up to the AES block (up to data_size + 15): the check runs
 * before the one-shot decrypt, so it must cover everything the Token blob could unpad to; do
 * NOT size out to data_size exactly, RS_HANDHELD_RESOURCE_DATA_MAX always fits — the transfer
 * stays intact, retry with a larger buffer; RS_HANDHELD_ERR_CRYPTO if decryption or the hash
 * check fails (the transfer is then CORRUPT: close it and report the failed transfer). */
rs_handheld_status_t rs_handheld_rns_resource_assemble(rs_handheld_rns_t *ctx,
                                                       const uint8_t key[64], uint8_t *out,
                                                       size_t out_cap, size_t *out_len);

/* RECEIVER: write the 64-byte delivery proof (resource_hash || SHA-256(data || resource_hash))
 * for the assembled transfer into out[out_cap] (length to *out_len); send it link-encrypted as a PROOF packet
 * with context RESOURCE_PRF. RS_HANDHELD_ERR_NOT_READY if no inbound transfer is open or it is
 * not assembled yet; RS_HANDHELD_ERR_CAPACITY if out_cap < RS_HANDHELD_RESOURCE_PROOF_LEN. */
rs_handheld_status_t rs_handheld_rns_resource_proof_build(const rs_handheld_rns_t *ctx,
                                                          uint8_t *out, size_t out_cap,
                                                          size_t *out_len);

/* RECEIVER: close the open inbound transfer (assembled and stored, failed or timed out — the
 * C++ owner decides when). Frees the transfer buffer. No-op if none is open. */
rs_handheld_status_t rs_handheld_rns_resource_inbound_close(rs_handheld_rns_t *ctx);

/* ---- Announce ratchets --------------------------------------------------------------------
 * Upstream posture: always enable, never enforce. Rust owns bounded key/table logic; the host
 * supplies entropy, two explicit local clocks, and durable storage. Wire ordering is a separate
 * persisted 40-bit value and is NEVER interpreted as elapsed seconds. Inbound opportunistic
 * decrypt tries retained ratchets newest-first and then the base key. Outbound opportunistic
 * encryption prefers an accepted, unexpired peer ratchet and otherwise uses the base key.
 *
 * Secret-ring and announce-order changes are two-call transactions:
 *   prepare -> durably write/verify exactly returned bytes -> commit those exact bytes.
 * Prepare never mutates live state. Never advertise an action-2 candidate before commit. If
 * durable storage fails, abandon the candidate, wipe its buffer, log a diagnostic, and emit an
 * unratcheted announce instead. A full ring whose oldest key age cannot be proven never evicts it. */

/* Maximum opaque persistence blobs. Keep these buffers as C++ object members, not task locals. */
#define RS_HANDHELD_RATCHET_RING_BLOB_MAX 2690u /* ring blob + identity signature */
#define RS_HANDHELD_PEER_RATCHETS_BLOB_MAX 1826u /* public peer table */
#define RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX 71u /* 7-byte state + identity signature */

/* rs_handheld_rns_ratchet_prepare out_action values. Actions 1/2 return a signed blob that must
 * be persisted and committed. Actions 0/3 return out_len=0 and the existing current public key. */
#define RS_HANDHELD_RATCHET_PREP_UNCHANGED          0
#define RS_HANDHELD_RATCHET_PREP_PERSIST_METADATA   1
#define RS_HANDHELD_RATCHET_PREP_ROTATED            2
#define RS_HANDHELD_RATCHET_PREP_FULL_RING_PROTECTED 3

/* Prepare a ring transition using entropy[32] as the possible fresh private key. wall_secs is
 * epoch seconds, or 0 when unavailable; uptime_ms is monotonic within this boot. The rotation
 * cadence is 12 hours. out_current_pub is the candidate current key, but for ROTATED it is not
 * live/advertisable until commit succeeds. out contains private key material: wipe after use. */
rs_handheld_status_t rs_handheld_rns_ratchet_prepare(rs_handheld_rns_t *ctx,
                                                     const uint8_t entropy[32],
                                                     uint64_t wall_secs, uint64_t uptime_ms,
                                                     uint8_t *out, size_t out_cap,
                                                     size_t *out_len, int32_t *out_action,
                                                     uint8_t out_current_pub[32]);

/* Commit the exact signed candidate most recently returned by ratchet_prepare, only after its
 * durable write succeeds. Tampered/foreign data returns CRYPTO; stale/unrelated candidates return
 * INVALID_ARG. Writes the now-live current public key. */
rs_handheld_status_t rs_handheld_rns_ratchet_commit(rs_handheld_rns_t *ctx,
                                                    const uint8_t *data, size_t data_len,
                                                    uint8_t out_current_pub[32]);

/* Current own ratchet public key without rotating. RS_HANDHELD_ERR_NOT_READY before the
 * first rotation. */
rs_handheld_status_t rs_handheld_rns_ratchet_current(const rs_handheld_rns_t *ctx,
                                                     uint8_t out_pub[32]);

/* Export the own ring as blob||identity_signature for persistence (size out with
 * RS_HANDHELD_RATCHET_RING_BLOB_MAX). Losing this blob across reboots makes ratcheted
 * inbound sent against pre-reboot announces undecryptable. RS_HANDHELD_ERR_NOT_READY
 * without an identity; RS_HANDHELD_ERR_CAPACITY if out_cap is too small. */
rs_handheld_status_t rs_handheld_rns_ratchet_export(const rs_handheld_rns_t *ctx, uint8_t *out,
                                                    size_t out_cap, size_t *out_len);

/* Restore the own ring from an export blob. The identity signature is verified, so a
 * tampered or foreign blob returns RS_HANDHELD_ERR_CRYPTO and leaves the ring untouched. */
rs_handheld_status_t rs_handheld_rns_ratchet_seed(rs_handheld_rns_t *ctx, const uint8_t *data,
                                                  size_t data_len);

/* Signed durable announce-order state. export/seed support boot restore. prepare uses wall_secs
 * verbatim when it advances and coalesces same/backward time (*out_ready=0, *out_len=0). With
 * wall_secs=0 it produces last+1, including across restart. Persist and verify returned bytes,
 * then commit; only the committed value may accompany a ratcheted announce. */
rs_handheld_status_t rs_handheld_rns_announce_state_export(const rs_handheld_rns_t *ctx,
                                                           uint8_t *out, size_t out_cap,
                                                           size_t *out_len);
rs_handheld_status_t rs_handheld_rns_announce_state_seed(rs_handheld_rns_t *ctx,
                                                         const uint8_t *data, size_t data_len);
rs_handheld_status_t rs_handheld_rns_announce_state_prepare(rs_handheld_rns_t *ctx,
                                                            uint64_t wall_secs,
                                                            uint8_t *out, size_t out_cap,
                                                            size_t *out_len,
                                                            uint64_t *out_wire_value,
                                                            int32_t *out_ready);
rs_handheld_status_t rs_handheld_rns_announce_state_commit(rs_handheld_rns_t *ctx,
                                                           const uint8_t *data, size_t data_len,
                                                           uint64_t *out_wire_value);

/* Export the accepted peer-ratchet table (public keys only) for persistence. */
rs_handheld_status_t rs_handheld_rns_peer_ratchets_export(const rs_handheld_rns_t *ctx,
                                                          uint8_t *out, size_t out_cap,
                                                          size_t *out_len);

/* Compatibility restore without clock anchoring. New boot code should use seed_at. */
rs_handheld_status_t rs_handheld_rns_peer_ratchets_seed(rs_handheld_rns_t *ctx,
                                                        const uint8_t *data, size_t data_len);

/* Restore after complete bounds/shape validation, then conservatively anchor unknown or
 * cross-boot uptime ages to this boot's local clock. *out_changed=1 means export/persist v2 now. */
rs_handheld_status_t rs_handheld_rns_peer_ratchets_seed_at(rs_handheld_rns_t *ctx,
                                                           const uint8_t *data, size_t data_len,
                                                           uint64_t wall_secs, uint64_t uptime_ms,
                                                           int32_t *out_changed);

/* The only peer-table write seam. Call after transport freshness AND C++ KeyMap continuity accept
 * an lxmf.delivery announce. Re-observing the same key is a strict no-op (*out_changed=0), so it
 * cannot refresh expiry. Persist after *out_changed=1. */
rs_handheld_status_t rs_handheld_rns_peer_ratchet_remember(rs_handheld_rns_t *ctx,
                                                           const uint8_t destination_hash[16],
                                                           const uint8_t ratchet[32],
                                                           uint64_t wall_secs, uint64_t uptime_ms,
                                                           int32_t *out_changed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RATSPEAK_PROTOCOL_H */
