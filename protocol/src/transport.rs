//! Transport tables, packet I/O, routing, and interface framing.

use super::*;
//
// rsDeck feeds raw Reticulum packet bytes in (RX from the LoRa driver) and polls raw bytes out
// (TX to the driver); the LiteNode (rsReticulumLite) does the relay control plane (dedup, learn
// paths, answer/forward path requests, schedule announce rebroadcasts). All wire/transport logic
// stays in rns-lite-core; this is a thin panic-isolated C ABI over it.

/// Transport counters for periodic telemetry (mirrors `TransportStats` + the live outbound depth).
#[repr(C)]
pub struct RsHandheldTransportStats {
    pub accepted: u64,
    pub duplicates: u64,
    pub learned_announces: u64,
    pub queued_outbound: u64,
    pub dropped: u64,
    pub validation_failures: u64,
    pub outbound_dropped: u64,
    pub announces_rate_dropped: u64,
    pub outbound_len: u32,
}

/// Endpoint transmit route selected from the live Rust path table.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RsHandheldRoute {
    /// 0 = broadcast on every live interface, 1 = direct on `interface_id` only.
    pub kind: i32,
    pub interface_id: u8,
    /// 0 = HEADER_1, 1 = HEADER_2.
    pub header_type: u8,
    pub next_hop: [u8; DESTINATION_LENGTH],
    pub hops: u8,
}

pub(super) const ROUTE_BROADCAST: i32 = 0;
pub(super) const ROUTE_DIRECT: i32 = 1;

/// `IngestAction` as a stable C int (mirrors `RS_HANDHELD_INGEST_*` in the header).
pub(super) fn ingest_action_code(action: IngestAction) -> i32 {
    match action {
        IngestAction::Accepted => 0,
        IngestAction::Duplicate => 1,
        IngestAction::LearnedAnnounce => 2,
        IngestAction::ScheduledAnnounce => 3,
        IngestAction::AnsweredPathRequest => 4,
        IngestAction::ForwardedPathRequest => 5,
        IngestAction::ForwardedTransport => 6,
        IngestAction::ForwardedProof => 7,
        IngestAction::Dropped => 8,
        IngestAction::AnnounceIgnored => 12,
    }
}

/// `RS_HANDHELD_INGEST_LOCAL_FRAME`: a packet addressed to this endpoint (our lxmf.delivery
/// destination, a registered link id, or any inbound PROOF) that the relay core drops as
/// non-forwardable — the C++ pump routes it to the LXMF/link/resource engines instead.
pub(super) const INGEST_LOCAL_FRAME: i32 = 9;

/// `RS_HANDHELD_INGEST_PATH_REQUEST_SELF`: an inbound path request for our OWN lxmf.delivery dest
/// that the relay core dropped (an endpoint has no cached path to answer from). The C++ ProtocolRuntime
/// re-announces our dest as a PATH_RESPONSE (throttled). Endpoint scope only — requests for other
/// dests stay `Dropped`. This is the trusted `local_destinations` branch (rns-transport
/// outbound.rs::handle_inbound_path_request) expressed where the FFI already knows our own dest.
pub(super) const INGEST_PATH_REQUEST_SELF: i32 = 10;
/// A validated inbound announce for an aspect OTHER than `lxmf.delivery` (e.g. `lxst.telephony`,
/// `lxmf.propagation`, NomadNet). Its path is still learned, but it is NOT surfaced as a contact —
/// this LXMF endpoint neither routes nor lists other aspects (the
/// AnnounceManager filters to `lxmf.delivery`, else one identity shows as several phantom peers).
pub(super) const INGEST_ANNOUNCE_OTHER: i32 = 11;

/// True iff the announce payload is for the `lxmf.delivery` aspect (name_hash match). Cheap parse;
/// no signature check (the caller already validated for path-learning).
pub(super) fn is_lxmf_delivery_announce(payload: &[u8], context_flag: bool) -> bool {
    matches!(
        AnnounceView::parse(payload, context_flag, RS_HANDHELD_ANNOUNCE_MAX_APP_DATA),
        Ok(v) if v.name_hash == name_hash(LXMF_DELIVERY_NAME)
    )
}

/// Max raw payload a local frame carries (single-packet MDU; parts/link data all fit).
pub(super) const LOCAL_FRAME_PAYLOAD_MAX: usize = rns_lite_core::constants::MTU;

/// An inbound packet addressed to this endpoint, handed to the C++ delivery engines. `#[repr(C)]`
/// POD; `payload` holds `payload_len` valid bytes (the rest is zeroed). `packet_hash` is the full
/// 32-byte hash (the proof-binding input; computed in Rust so the wire rule stays here).
#[repr(C)]
pub struct RsHandheldLocalFrame {
    pub packet_type: u8, // 0 Data, 1 Announce, 2 LinkRequest, 3 Proof
    pub context: u8,     // PacketContext byte
    pub header_type: u8, // 0 Header1, 1 Header2
    pub hops: u8,        // wire hops (as received)
    pub destination_hash: [u8; DESTINATION_LENGTH],
    pub packet_hash: [u8; 32],
    pub payload_len: u32,
    pub payload: [u8; LOCAL_FRAME_PAYLOAD_MAX],
}

/// Classify a raw packet (already run through the relay core, which dropped it as non-forwardable)
/// as a local delivery for this endpoint: DATA/LINKREQUEST to our lxmf.delivery dest, a Link-typed
/// packet to a registered link id, or ANY inbound PROOF (the C++ pending-proof map decides). Fills
/// `out` and returns true iff it is local. Wire parsing stays in Rust (PacketView) per the
/// port-direction rule; the routing decision is data-only.
pub(super) fn classify_local_frame(
    ctx: &RsHandheldRns,
    raw: &[u8],
    out: &mut RsHandheldLocalFrame,
) -> bool {
    let Ok(view) = PacketView::parse(raw) else {
        return false;
    };
    let h = view.header;
    let is_local = match h.flags.packet_type {
        PacketType::Proof => true, // C++ matches against its pending (hash, key) set
        PacketType::Data | PacketType::LinkRequest => {
            let to_our_dest = ctx
                .identity
                .as_ref()
                .is_some_and(|id| id.lxmf_delivery_hash() == h.destination_hash);
            let to_our_link = h.flags.destination_type == DestinationType::Link
                && ctx
                    .link_ids
                    .iter()
                    .flatten()
                    .any(|l| *l == h.destination_hash);
            to_our_dest || to_our_link
        }
        PacketType::Announce => false,
    };
    if !is_local || view.payload.len() > LOCAL_FRAME_PAYLOAD_MAX {
        return false;
    }
    out.packet_type = h.flags.packet_type as u8;
    out.context = h.context.to_byte();
    out.header_type = h.flags.header_type as u8;
    out.hops = h.hops;
    out.destination_hash = h.destination_hash;
    out.packet_hash = packet_hash(raw, h.flags.header_type);
    out.payload = [0u8; LOCAL_FRAME_PAYLOAD_MAX];
    out.payload[..view.payload.len()].copy_from_slice(view.payload);
    out.payload_len = view.payload.len() as u32;
    true
}

/// Detect an inbound path request whose requested destination is our OWN lxmf.delivery dest
/// (the endpoint branch of Python `Transport.path_request` / trusted rns-transport
/// `handle_inbound_path_request` local-dest answer). Returns its exact request tag iff `raw` is a
/// PLAIN/DATA/Header1 packet to the `rnstransport.path.request` control dest whose payload requests
/// our delivery dest with a non-empty tag. Tagless requests are ignored (Python parity).
pub(super) fn classify_own_path_request(ctx: &RsHandheldRns, raw: &[u8]) -> Option<OwnPathRequest> {
    let Ok(view) = PacketView::parse(raw) else {
        return None;
    };
    let h = view.header;
    if h.flags.packet_type != PacketType::Data
        || h.flags.destination_type != DestinationType::Plain
        || h.destination_hash != rns_lite_core::path_request_destination()
    {
        return None;
    }
    // Payload layout (Transport.py:2854 / lite handle_path_request): requested(16), then either
    // tag(<=16) for a leaf requestor, or requestor_transport_id(16)+tag(<=16) for a transport peer.
    let payload = view.payload;
    if payload.len() <= 16 {
        return None; // tagless — ignored, same as the relay core
    }
    let tag = if payload.len() > 32 {
        &payload[32..payload.len().min(48)]
    } else {
        &payload[16..payload.len().min(32)]
    };
    if tag.is_empty() {
        return None;
    }
    if ctx
        .identity
        .as_ref()
        .is_none_or(|id| id.lxmf_delivery_hash() != payload[..16])
    {
        return None;
    }
    let mut tag_bytes = [0u8; DESTINATION_LENGTH];
    tag_bytes[..tag.len()].copy_from_slice(tag);
    Some(OwnPathRequest {
        tag: tag_bytes,
        tag_len: tag.len(),
    })
}

/// `OutboundReason` as a stable C int (mirrors `RS_HANDHELD_TX_*` in the header).
pub(super) fn outbound_reason_code(reason: OutboundReason) -> i32 {
    match reason {
        OutboundReason::AnnounceRebroadcast => 0,
        OutboundReason::PathResponse => 1,
        OutboundReason::PathRequestForward => 2,
        OutboundReason::PathRequest => 3,
        OutboundReason::TransportForward => 4,
        OutboundReason::ProofReturn => 5,
    }
}

/// Byte size of the transport node for `profile`, or 0 if this artifact was not compiled
/// for `profile` (wrong-artifact tripwire — size the open_transport buffer with this).
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_rns_transport_size(profile: i32) -> usize {
    if profile == ACTIVE_PROFILE {
        core::mem::size_of::<ActiveNode>()
    } else {
        0
    }
}

/// Required alignment of the transport node buffer for `profile`, or 0 if this artifact
/// was not compiled for `profile`.
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_rns_transport_align(profile: i32) -> usize {
    if profile == ACTIVE_PROFILE {
        core::mem::align_of::<ActiveNode>()
    } else {
        0
    }
}

/// In-place construction of a `LiteNode` in caller-provided uninitialized memory.
/// Validates exactly like `LiteNode::new` BEFORE touching `slot`, then writes every field
/// through raw place projections — table arrays per element (`None` per slot; the largest
/// by-value temp is one ELEMENT, 552 B host, never a table or the node).
///
/// This is the one place a bounded unsafe block is genuinely required: every safe
/// construction path (`MaybeUninit::write`, `Box::new`, `ptr::write` of `new_const`) takes
/// the node BY VALUE, which the compiler may materialize on the calling task's stack
/// (~188 KiB for the SMALL profile; copy elision is never guaranteed). Zero-init is
/// UNSOUND (`Option<PathEntry>` etc. are niche-packed: `None` != all-zeros). Static
/// placement cannot serve the SMALL profile (193 KB exceeds free internal SRAM).
/// rns-lite-core forbids unsafe, so the writer lives here over its documented
/// `#[doc(hidden)]` placement field contract.
///
/// SOUNDNESS:
/// (a) `slot` is `&mut MaybeUninit<Node>`: size, alignment and write permission are
///     guaranteed by the caller's reference;
/// (b) every field of `LiteNode` (and of its `Queue`) is written exactly once below,
///     before any reference to the whole node is formed — the exhaustive-destructure
///     drift guard in tests breaks the BUILD if rns-lite-core adds a field this writer
///     does not cover;
/// (c) all field types are `Copy`/no-`Drop` plain data: no drop obligations exist on the
///     uninitialized memory and abandoning a previous node is sound;
/// (d) all writes go through `addr_of_mut!` place projections on the raw pointer — no
///     intermediate `&`/`&mut` to uninitialized bytes is ever created.
pub(super) fn init_node_in<
    const PATHS: usize,
    const HASHES: usize,
    const ANNOUNCES: usize,
    const REVERSE: usize,
    const LINKS: usize,
    const TAGS: usize,
    const OUTBOUND: usize,
    const KNOWN_DESTINATIONS: usize,
>(
    slot: &mut MaybeUninit<
        LiteNode<PATHS, HASHES, ANNOUNCES, REVERSE, LINKS, TAGS, OUTBOUND, KNOWN_DESTINATIONS>,
    >,
    config: LiteConfig,
    transport_id: [u8; DESTINATION_LENGTH],
) -> Result<
    &mut LiteNode<PATHS, HASHES, ANNOUNCES, REVERSE, LINKS, TAGS, OUTBOUND, KNOWN_DESTINATIONS>,
    TransportError,
> {
    LiteNode::<
        PATHS,
        HASHES,
        ANNOUNCES,
        REVERSE,
        LINKS,
        TAGS,
        OUTBOUND,
        KNOWN_DESTINATIONS,
    >::validate_config(&config)?;

    /// Write `None` into every element of a `[Option<T>; N]` behind `arr`.
    /// SAFETY (caller): `arr` must be valid for writes of the whole array.
    unsafe fn fill_none<T, const N: usize>(arr: *mut [Option<T>; N]) {
        let first: *mut Option<T> = arr.cast();
        for i in 0..N {
            // SAFETY: `i < N` stays inside the array `arr` points to.
            unsafe { first.add(i).write(None) };
        }
    }

    let p = slot.as_mut_ptr();
    // SAFETY: per the function-level soundness argument (a)-(d).
    unsafe {
        addr_of_mut!((*p).config).write(config);
        addr_of_mut!((*p).transport_id).write(transport_id);
        fill_none(addr_of_mut!((*p).own_destinations));
        addr_of_mut!((*p).announce_admission).write(AnnounceAdmission::new());
        addr_of_mut!((*p).known_destinations).write(KnownDestinations::new());
        fill_none(addr_of_mut!((*p).packet_hashes.entries));
        fill_none(addr_of_mut!((*p).paths.entries));
        fill_none(addr_of_mut!((*p).announce_cache.entries));
        fill_none(addr_of_mut!((*p).announce_schedule.entries));
        fill_none(addr_of_mut!((*p).reverse.entries));
        fill_none(addr_of_mut!((*p).links.entries));
        fill_none(addr_of_mut!((*p).request_tags.entries));
        fill_none(addr_of_mut!((*p).outbound.entries));
        addr_of_mut!((*p).outbound.head).write(0);
        addr_of_mut!((*p).outbound.len).write(0);
        addr_of_mut!((*p).stats).write(TransportStats::default());
        // Every field is initialized: forming the reference is now sound.
        Ok(&mut *p)
    }
}

/// Open the transport/relay node for the active context IN PLACE in the caller-owned
/// buffer `buf` (`buf_len` bytes), with the caller's 16-byte transport id and a
/// `transport_enabled` flag (1 = participate as a transport/relay; 0 = endpoint-only).
/// `profile` must be the profile this artifact was compiled with (`ErrUnsupported`
/// otherwise — catches a wrong-artifact link at bring-up). `buf` must be at least
/// `rs_handheld_rns_transport_size(profile)` bytes, aligned to
/// `rs_handheld_rns_transport_align(profile)` (`ErrInvalidArg` otherwise).
///
/// OWNERSHIP: the buffer stays CALLER-owned. Rust stores only a pointer and never frees
/// it; the node is plain no-Drop data. Teardown order: resource closes ->
/// `rs_handheld_rns_shutdown(ctx)` -> free(buf). Replaces any previously-opened node
/// (the old node is abandoned, not freed; the same buffer may be reused).
///
/// # Safety
/// `ctx` live; `transport_id` 16 readable bytes; `buf` writable for `buf_len` bytes, not
/// aliased by other live objects, and kept alive (and unmoved) until shutdown or the node
/// is replaced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_open_transport(
    ctx: *mut RsHandheldRns,
    profile: i32,
    transport_id: *const [u8; DESTINATION_LENGTH],
    transport_enabled: i32,
    buf: *mut u8,
    buf_len: usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || transport_id.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if profile != ACTIVE_PROFILE {
            return RsHandheldStatus::ErrUnsupported;
        }
        if buf.is_null()
            || buf_len < core::mem::size_of::<ActiveNode>()
            || (buf as usize) % core::mem::align_of::<ActiveNode>() != 0
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mut config = ACTIVE_CONFIG;
        config.transport_enabled = transport_enabled != 0;
        config.announce_admission = AnnounceAdmissionConfig {
            steady_per_sec: 5,
            grace_per_sec: 3,
            grace_secs: 60,
        };
        // SAFETY: non-null per the contract; `buf` is sized/aligned for an ActiveNode
        // (checked above) and viewing caller memory as MaybeUninit imposes no validity
        // requirement on its current contents.
        let ctx = unsafe { &mut *ctx };
        ctx.own_path_request = None;
        let slot = unsafe { &mut *buf.cast::<MaybeUninit<ActiveNode>>() };
        match init_node_in(slot, config, *unsafe { &*transport_id }) {
            Ok(node) => {
                // Own-announce guard (see load_identity): cover the open-after-load order too.
                if let Some(id) = &ctx.identity {
                    let _ = node.register_own_destination(id.lxmf_delivery_hash());
                }
                ctx.node = Some(NonNull::from(node));
                RsHandheldStatus::Ok
            }
            Err(TransportError::CapacityTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Feed one raw inbound Reticulum packet to the transport node with the receiving interface's
/// authoritative mode. `*out_action` receives the
/// `RS_HANDHELD_INGEST_*` code. If the packet is a valid announce and `out_event` is non-null, the
/// validated announce is also written to `*out_event` (for the C++ AnnounceManager) — check
/// `*out_action` for an announce code to know it was filled. `ErrNotReady` if no node is open;
/// `ErrInvalidArg` on null or a malformed packet.
///
/// # Safety
/// `ctx` live with an open node; `raw` `raw_len` readable bytes; `out_action` a writable i32;
/// `out_event` null or a writable, aligned `RsHandheldAnnounceEvent`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_packet_ingest_with_mode(
    ctx: *mut RsHandheldRns,
    raw: *const u8,
    raw_len: usize,
    interface_id: u8,
    interface_mode: i32,
    now_ms: u64,
    out_action: *mut i32,
    out_event: *mut RsHandheldAnnounceEvent,
    out_local: *mut RsHandheldLocalFrame,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || raw.is_null() || out_action.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mode = match interface_mode {
            RS_HANDHELD_IFACE_MODE_FULL => InterfaceMode::Full,
            RS_HANDHELD_IFACE_MODE_ACCESS_POINT => InterfaceMode::AccessPoint,
            RS_HANDHELD_IFACE_MODE_ROAMING => InterfaceMode::Roaming,
            RS_HANDHELD_IFACE_MODE_BOUNDARY => InterfaceMode::Boundary,
            RS_HANDHELD_IFACE_MODE_GATEWAY => InterfaceMode::Gateway,
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        let ctx = unsafe { &mut *ctx };
        ctx.own_path_request = None;
        let node = match ctx.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        let raw = unsafe { core::slice::from_raw_parts(raw, raw_len) };
        let action = match node.ingest(raw, RxMeta::with_mode(interface_id, mode), now_ms) {
            Ok(a) => a,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        let mut action_code = ingest_action_code(action);

        // Python 1.3.8 (Packet.py:247) parse-rejects raw hops >= PATHFINDER_M outright; the
        // relay core drops them, and none of the endpoint post-processing below (contact
        // surfacing, local delivery, path-request self-answer) may resurrect such a packet.
        let hops_valid = PacketView::parse(raw)
            .is_ok_and(|v| v.header.hops < rns_lite_core::constants::PATHFINDER_M);

        // Surface only an announce the transport freshness/quality policy actually accepted.
        // A signature-valid replay returns AnnounceIgnored (12) and must not reach KeyMap or the
        // peer-ratchet table. Other accepted aspects are still suppressed as contacts.
        if hops_valid
            && matches!(
                action,
                IngestAction::LearnedAnnounce | IngestAction::ScheduledAnnounce
            )
        {
            if let Ok(view) = PacketView::parse(raw) {
                if view.header.flags.packet_type == PacketType::Announce {
                    if is_lxmf_delivery_announce(view.payload, view.header.flags.context_flag) {
                        if !out_event.is_null() {
                            let ev = unsafe { &mut *out_event };
                            let status = fill_announce_event(
                                view.payload,
                                &view.header.destination_hash,
                                view.header.flags.context_flag,
                                None,
                                ev,
                            );
                            if status != RsHandheldStatus::Ok {
                                return status;
                            }
                        }
                    } else {
                        action_code = INGEST_ANNOUNCE_OTHER;
                    }
                }
            }
        }

        // Local-delivery detection: an endpoint (transport_enabled=0) drops packets addressed
        // to it as non-forwardable. Re-classify a Dropped frame as a LocalFrame for our own
        // destination / a registered link / any proof, so C++ can run the LXMF/link/resource path.
        if hops_valid
            && action == IngestAction::Dropped
            && !out_local.is_null()
            && classify_local_frame(ctx, raw, unsafe { &mut *out_local })
        {
            action_code = INGEST_LOCAL_FRAME;
        } else if hops_valid && action == IngestAction::Dropped {
            if let Some(request) = classify_own_path_request(ctx, raw) {
                // Endpoint path-request self-answer: the relay core has no cached path to us, so C++
                // re-announces our lxmf.delivery dest as a PATH_RESPONSE (throttled). A literal
                // retransmit is already dropped as Duplicate inside node.ingest (storm-defense layer 1).
                ctx.own_path_request = Some(request);
                action_code = INGEST_PATH_REQUEST_SELF;
            }
        }

        unsafe { *out_action = action_code };
        RsHandheldStatus::Ok
    })
}

/// Compatibility ingest entry point for hosts without interface-mode metadata. Learned paths use
/// the active profile's node-wide mode, matching the pre-interface-scoped API.
///
/// # Safety
/// Same contract as [`rs_handheld_rns_packet_ingest_with_mode`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_packet_ingest(
    ctx: *mut RsHandheldRns,
    raw: *const u8,
    raw_len: usize,
    interface_id: u8,
    now_ms: u64,
    out_action: *mut i32,
    out_event: *mut RsHandheldAnnounceEvent,
    out_local: *mut RsHandheldLocalFrame,
) -> RsHandheldStatus {
    let mode = match ACTIVE_CONFIG.mode {
        InterfaceMode::Full => RS_HANDHELD_IFACE_MODE_FULL,
        InterfaceMode::AccessPoint => RS_HANDHELD_IFACE_MODE_ACCESS_POINT,
        InterfaceMode::Gateway => RS_HANDHELD_IFACE_MODE_GATEWAY,
        InterfaceMode::Roaming => RS_HANDHELD_IFACE_MODE_ROAMING,
        InterfaceMode::Boundary => RS_HANDHELD_IFACE_MODE_BOUNDARY,
    };
    unsafe {
        rs_handheld_rns_packet_ingest_with_mode(
            ctx,
            raw,
            raw_len,
            interface_id,
            mode,
            now_ms,
            out_action,
            out_event,
            out_local,
        )
    }
}

/// Consume the exact tag associated with the most recent `INGEST_PATH_REQUEST_SELF` action.
/// This is intentionally separate from packet_ingest to preserve its existing ABI. Single-task
/// callers must invoke it immediately after action 10 and before ingesting another frame.
///
/// # Safety
/// `ctx` live; `out_tag` 16 writable bytes; `out_tag_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_take_own_path_request_tag(
    ctx: *mut RsHandheldRns,
    out_tag: *mut [u8; DESTINATION_LENGTH],
    out_tag_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_tag.is_null() || out_tag_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let ctx = unsafe { &mut *ctx };
        let Some(request) = ctx.own_path_request.take() else {
            return RsHandheldStatus::ErrNotReady;
        };
        unsafe {
            *out_tag = request.tag;
            *out_tag_len = request.tag_len;
        }
        RsHandheldStatus::Ok
    })
}

/// Register a live link id for inbound routing: a subsequent packet addressed to `link_id`
/// (or any proof for it) is reported by `packet_ingest` as `INGEST_LOCAL_FRAME`. Idempotent;
/// silently no-ops if the fixed slot set is full (endpoint scope). `ErrInvalidArg` on null.
///
/// # Safety
/// `ctx` live; `link_id` 16 readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_register(
    ctx: *mut RsHandheldRns,
    link_id: *const [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || link_id.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract.
        let ctx = unsafe { &mut *ctx };
        let id = *unsafe { &*link_id };
        if ctx.link_ids.iter().flatten().any(|l| *l == id) {
            return RsHandheldStatus::Ok;
        }
        if let Some(slot) = ctx.link_ids.iter_mut().find(|s| s.is_none()) {
            *slot = Some(id);
        }
        RsHandheldStatus::Ok
    })
}

/// Unregister a live link id. No-op if not present. `ErrInvalidArg` on null.
///
/// # Safety
/// `ctx` live; `link_id` 16 readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_unregister(
    ctx: *mut RsHandheldRns,
    link_id: *const [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || link_id.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract.
        let ctx = unsafe { &mut *ctx };
        let id = *unsafe { &*link_id };
        for slot in ctx.link_ids.iter_mut() {
            if *slot == Some(id) {
                *slot = None;
            }
        }
        RsHandheldStatus::Ok
    })
}

// ---- Pure outbound wire construction: rns-lite-core is the only packet-byte producer ----

/// Build one self-originated Reticulum packet with the context flag CLEAR — see
/// [`rs_handheld_rns_packet_build_flagged`] when it must be set (ratcheted announces).
/// `header_type` is 0 (Header1/Broadcast) or 1 (Header2/Transport); Header2 requires
/// `transport_id`, while Header1 requires it to be null. `packet_type` and `destination_type` use
/// the 0..=3 Reticulum wire discriminants. Hops are zero and `context` is the raw PacketContext
/// byte. `ErrCapacity` means the packet exceeds the 500-byte wire MTU or `out_cap`.
///
/// # Safety
/// `destination_hash` points to 16 readable bytes; `transport_id` is null for Header1 or points to
/// 16 readable bytes for Header2; `payload` points to `payload_len` readable bytes (or is null iff
/// zero); `out` points to `out_cap` writable bytes; `out_len` is writable. Inputs must not overlap
/// `out`.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_packet_build(
    header_type: i32,
    packet_type: i32,
    destination_type: i32,
    context: u8,
    transport_id: *const [u8; DESTINATION_LENGTH],
    destination_hash: *const [u8; DESTINATION_LENGTH],
    payload: *const u8,
    payload_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    unsafe {
        rs_handheld_rns_packet_build_flagged(
            header_type,
            packet_type,
            destination_type,
            context,
            0,
            transport_id,
            destination_hash,
            payload,
            payload_len,
            out,
            out_cap,
            out_len,
        )
    }
}

/// [`rs_handheld_rns_packet_build`] with an explicit `context_flag` (0/1). A ratcheted announce
/// MUST set it: receivers parse the announce payload's optional ratchet field off this header bit,
/// so a ratchet emitted with the flag clear is mis-parsed and fails signature validation
/// everywhere (Python `Destination.announce` sets FLAG_SET exactly when a ratchet is attached).
///
/// # Safety
/// Identical to [`rs_handheld_rns_packet_build`].
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_packet_build_flagged(
    header_type: i32,
    packet_type: i32,
    destination_type: i32,
    context: u8,
    context_flag: i32,
    transport_id: *const [u8; DESTINATION_LENGTH],
    destination_hash: *const [u8; DESTINATION_LENGTH],
    payload: *const u8,
    payload_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if destination_hash.is_null()
            || out.is_null()
            || out_len.is_null()
            || (payload.is_null() && payload_len != 0)
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let (header_type, transport_type, transport_id) = match header_type {
            0 if transport_id.is_null() => (HeaderType::Header1, TransportType::Broadcast, None),
            1 if !transport_id.is_null() => (
                HeaderType::Header2,
                TransportType::Transport,
                Some(*unsafe { &*transport_id }),
            ),
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        let packet_type = match packet_type {
            0 => PacketType::Data,
            1 => PacketType::Announce,
            2 => PacketType::LinkRequest,
            3 => PacketType::Proof,
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        let destination_type = match destination_type {
            0 => DestinationType::Single,
            1 => DestinationType::Group,
            2 => DestinationType::Plain,
            3 => DestinationType::Link,
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        let payload = if payload_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(payload, payload_len) }
        };
        let header = PacketHeader {
            flags: PacketFlags {
                header_type,
                context_flag: context_flag != 0,
                transport_type,
                destination_type,
                packet_type,
            },
            hops: 0,
            transport_id,
            destination_hash: *unsafe { &*destination_hash },
            context: PacketContext::from_byte(context),
        };
        let raw = match build_packet(header, payload) {
            Ok(raw) => raw,
            Err(_) => return RsHandheldStatus::ErrCapacity,
        };
        if raw.len() > out_cap {
            return RsHandheldStatus::ErrCapacity;
        }
        unsafe { core::slice::from_raw_parts_mut(out, raw.len()) }.copy_from_slice(raw.as_slice());
        unsafe { *out_len = raw.len() };
        RsHandheldStatus::Ok
    })
}

/// Write canonical IPv6 text plus a trailing NUL. `*out_len` excludes the NUL.
///
/// # Safety
/// `address` points to 16 readable bytes; `out` points to `out_cap` writable chars; `out_len` is
/// writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_auto_format_ipv6(
    address: *const [u8; 16],
    out: *mut c_char,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if address.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if out_cap == 0 {
            return RsHandheldStatus::ErrCapacity;
        }
        let out = unsafe { core::slice::from_raw_parts_mut(out.cast::<u8>(), out_cap) };
        match rns_lite_core::format_ipv6(unsafe { &*address }, &mut out[..out_cap - 1]) {
            Ok(len) => {
                out[len] = 0;
                unsafe { *out_len = len };
                RsHandheldStatus::Ok
            }
            Err(AutoError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
        }
    })
}

/// Derive the default Temporary + Link AutoInterface multicast group from `group_id`.
///
/// # Safety
/// `group_id` points to `group_id_len` readable bytes (or is null iff zero); `out` points to 16
/// writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_auto_multicast_group(
    group_id: *const u8,
    group_id_len: usize,
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if out.is_null() || (group_id.is_null() && group_id_len != 0) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let group_id = if group_id_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(group_id, group_id_len) }
        };
        unsafe { *out = rns_lite_core::multicast_group_for(group_id) };
        RsHandheldStatus::Ok
    })
}

/// Derive the AutoInterface beacon token: SHA-256(group_id || canonical IPv6 text).
///
/// # Safety
/// `group_id` points to `group_id_len` readable bytes (or is null iff zero); `address` points to 16
/// readable bytes; `out` points to 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_auto_beacon_token(
    group_id: *const u8,
    group_id_len: usize,
    address: *const [u8; 16],
    out: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if address.is_null() || out.is_null() || (group_id.is_null() && group_id_len != 0) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let group_id = if group_id_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(group_id, group_id_len) }
        };
        unsafe { *out = rns_lite_core::beacon_token(group_id, &*address) };
        RsHandheldStatus::Ok
    })
}

/// Compute the full 32-byte Reticulum packet hash of `raw` (TX side of the proof binding):
/// the sender needs its own sent packet's hash to track the pending delivery proof. `header_type`
/// is 0 (Header1) / 1 (Header2). The hash rule stays in Rust (wire.rs). `ErrInvalidArg` on null.
///
/// # Safety
/// `raw` `raw_len` readable bytes; `out` 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_packet_hash(
    raw: *const u8,
    raw_len: usize,
    header_type: i32,
    out: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if raw.is_null() || out.is_null() || raw_len < 2 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let ht = match header_type {
            0 => HeaderType::Header1,
            1 => HeaderType::Header2,
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        // SAFETY: non-null + sized per the contract.
        let raw = unsafe { core::slice::from_raw_parts(raw, raw_len) };
        unsafe { *out = packet_hash(raw, ht) };
        RsHandheldStatus::Ok
    })
}

/// Poll one outbound packet the node has queued (announce rebroadcast, path response/request,
/// transport forward, proof). Writes the raw bytes to `out` (capacity `out_cap`), the byte length
/// to `*out_len` (0 = queue empty), the originating `*out_interface_id`, and the `RS_HANDHELD_TX_*`
/// reason to `*out_reason`. `ErrNotReady` if no node is open; `ErrCapacity` if `out` is too small.
///
/// # Safety
/// `ctx` live with an open node; `out` `out_cap` writable bytes; `out_len`/`out_interface_id`/
/// `out_reason` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_poll_outbound(
    ctx: *mut RsHandheldRns,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_interface_id: *mut u8,
    out_reason: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_interface_id.is_null()
            || out_reason.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        // Peek the next frame's length BEFORE consuming it: if `out` is too small, return
        // ErrCapacity with the frame still queued (a destructive pop-then-check would silently
        // drop relay traffic the caller never sees).
        let needed = match node.outbound_peek_len() {
            None => {
                unsafe {
                    *out_len = 0;
                    *out_interface_id = 0;
                    *out_reason = -1;
                }
                return RsHandheldStatus::Ok;
            }
            Some(n) => n,
        };
        if needed > out_cap {
            return RsHandheldStatus::ErrCapacity;
        }
        let frame = match node.poll_tx() {
            Some(f) => f,
            None => {
                unsafe { *out_len = 0 };
                return RsHandheldStatus::Ok;
            }
        };
        let bytes = frame.packet.as_slice();
        let dst = unsafe { core::slice::from_raw_parts_mut(out, bytes.len()) };
        dst.copy_from_slice(bytes);
        unsafe {
            *out_len = bytes.len();
            *out_interface_id = frame.interface_id;
            *out_reason = outbound_reason_code(frame.reason);
        }
        RsHandheldStatus::Ok
    })
}

/// Run time-based maintenance (table expiry + dispatch of due announce rebroadcasts into the
/// outbound queue). Call periodically with a monotonic millisecond clock. `ErrNotReady` if no node.
///
/// # Safety
/// `ctx` live with an open node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_tick(
    ctx: *mut RsHandheldRns,
    now_ms: u64,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        match unsafe { &mut *ctx }.node {
            Some(mut p) => {
                unsafe { p.as_mut() }.tick(now_ms);
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Write 1/0 to `*out_has_path` for whether a live path to `destination_hash` is known.
/// `ErrNotReady` if no node is open.
///
/// # Safety
/// `ctx` live; `destination_hash` 16 readable bytes; `out_has_path` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_has_path(
    ctx: *const RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    now_ms: u64,
    out_has_path: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || destination_hash.is_null() || out_has_path.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        match unsafe { &*ctx }.node {
            Some(p) => {
                let node = unsafe { p.as_ref() };
                let has = node.has_path(unsafe { &*destination_hash }, now_ms);
                unsafe { *out_has_path = has as i32 };
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Write the number of LIVE (unexpired) paths in the node's path table to `*out_count`
/// (the cumulative count differs from current paths: the cumulative `learned_announces` counter over-reports once
/// paths expire). `ErrNotReady` if no node is open.
///
/// # Safety
/// `ctx` live; `out_count` a writable u32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_path_count(
    ctx: *const RsHandheldRns,
    now_ms: u64,
    out_count: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_count.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        match unsafe { &*ctx }.node {
            Some(p) => {
                let node = unsafe { p.as_ref() };
                unsafe { *out_count = node.path_count(now_ms) as u32 };
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Read the live path to `destination_hash` for an endpoint TX decision (header framing +
/// `Identity.recall`-style gating): writes 1/0 to `*out_has_path`, and when known, the hop count to
/// `*out_hops`, the next-hop transport id to `out_next_hop[16]` (zeroed + `*out_has_next_hop`=0 when
/// the path is direct/1-hop), and the announced 64-byte public key to `out_public_key` (optional).
/// The C++ sender uses hops (1 → HEADER_1 broadcast; >1 → HEADER_2 to next_hop, Python
/// `Transport.outbound`) and the key for LXMF ECIES + proof binding. `ErrNotReady` if no node.
///
/// # Safety
/// `ctx` live with an open node; `destination_hash` 16 readable bytes; `out_has_path`/`out_hops`/
/// `out_has_next_hop` writable; `out_next_hop` 16 writable; `out_public_key` null or 64 writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_path_info(
    ctx: *const RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    now_ms: u64,
    out_has_path: *mut i32,
    out_hops: *mut u8,
    out_next_hop: *mut [u8; DESTINATION_LENGTH],
    out_has_next_hop: *mut i32,
    out_public_key: *mut [u8; PUBLIC_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || destination_hash.is_null()
            || out_has_path.is_null()
            || out_hops.is_null()
            || out_next_hop.is_null()
            || out_has_next_hop.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned buffer.
        let node = match unsafe { &*ctx }.node {
            Some(p) => unsafe { p.as_ref() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        match node.path(unsafe { &*destination_hash }, now_ms) {
            Some(entry) => unsafe {
                *out_has_path = 1;
                *out_hops = entry.hops;
                match entry.next_hop {
                    Some(nh) => {
                        *out_next_hop = nh;
                        *out_has_next_hop = 1;
                    }
                    None => {
                        *out_next_hop = [0u8; DESTINATION_LENGTH];
                        *out_has_next_hop = 0;
                    }
                }
                if !out_public_key.is_null() {
                    *out_public_key = entry.public_key;
                }
            },
            None => unsafe {
                *out_has_path = 0;
                *out_hops = 0;
                *out_next_hop = [0u8; DESTINATION_LENGTH];
                *out_has_next_hop = 0;
            },
        }
        RsHandheldStatus::Ok
    })
}

/// Select the endpoint transmit route for `destination_hash` from the live path table. A missing
/// or expired path broadcasts on every live interface. A live one-hop path uses HEADER_1 on only
/// the learned interface; a live multi-hop path uses HEADER_2 with its next-hop transport id.
///
/// # Safety
/// `ctx` is live with an open node; `destination_hash` is 16 readable bytes; `out_route` is a
/// writable, aligned `RsHandheldRoute`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_route(
    ctx: *const RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    now_ms: u64,
    out_route: *mut RsHandheldRoute,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || destination_hash.is_null() || out_route.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &*ctx }.node {
            Some(p) => unsafe { p.as_ref() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        let broadcast = RsHandheldRoute {
            kind: ROUTE_BROADCAST,
            interface_id: 0,
            header_type: 0,
            next_hop: [0; DESTINATION_LENGTH],
            hops: 0,
        };
        let route = match node.path(unsafe { &*destination_hash }, now_ms) {
            Some(entry) if entry.hops <= 1 => RsHandheldRoute {
                kind: ROUTE_DIRECT,
                interface_id: entry.interface_id,
                header_type: 0,
                next_hop: [0; DESTINATION_LENGTH],
                hops: entry.hops,
            },
            Some(entry) => match entry.next_hop {
                Some(next_hop) => RsHandheldRoute {
                    kind: ROUTE_DIRECT,
                    interface_id: entry.interface_id,
                    header_type: 1,
                    next_hop,
                    hops: entry.hops,
                },
                None => broadcast,
            },
            None => broadcast,
        };
        unsafe { *out_route = route };
        RsHandheldStatus::Ok
    })
}

/// Originate a path request for `destination_hash` and enqueue it (poll it via
/// `rs_handheld_rns_poll_outbound`). `tag` is a caller-supplied 16-byte random request tag.
/// `ErrNotReady` if no node is open.
///
/// # Safety
/// `ctx` live with an open node; `destination_hash` / `tag` 16 readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_request_path(
    ctx: *mut RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    tag: *const [u8; DESTINATION_LENGTH],
    interface_id: u8,
    now_ms: u64,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || destination_hash.is_null() || tag.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        match node.request_path(
            unsafe { &*destination_hash },
            unsafe { &*tag },
            interface_id,
            now_ms,
        ) {
            Ok(()) => RsHandheldStatus::Ok,
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Write the transport counters + live outbound depth. `ErrNotReady` if no node is open.
///
/// # Safety
/// `ctx` live; `out_stats` a writable, aligned `RsHandheldTransportStats`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_transport_stats(
    ctx: *const RsHandheldRns,
    out_stats: *mut RsHandheldTransportStats,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_stats.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract; the node pointer targets the caller-owned
        // open_transport buffer, alive until shutdown per the ABI contract.
        match unsafe { &*ctx }.node {
            Some(p) => {
                let node = unsafe { p.as_ref() };
                let s: TransportStats = node.stats();
                let out = unsafe { &mut *out_stats };
                out.accepted = s.accepted;
                out.duplicates = s.duplicates;
                out.learned_announces = s.learned_announces;
                out.queued_outbound = s.queued_outbound;
                out.dropped = s.dropped;
                out.validation_failures = s.validation_failures;
                out.outbound_dropped = s.outbound_dropped;
                out.announces_rate_dropped = s.announces_rate_dropped;
                out.outbound_len = node.outbound_len() as u32;
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Lower or restore the active announce-verification budget. The profile's 60-second grace-window
/// duration remains unchanged. Setting a rate to zero disables limiting for that phase.
///
/// # Safety
/// `ctx` is live with an open node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_set_announce_budget(
    ctx: *mut RsHandheldRns,
    steady_per_sec: u16,
    grace_per_sec: u16,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        node.set_announce_budget(steady_per_sec, grace_per_sec);
        RsHandheldStatus::Ok
    })
}

pub(super) fn known_destination_status(error: KnownDestinationError) -> RsHandheldStatus {
    match error {
        KnownDestinationError::EmptyCapacity
        | KnownDestinationError::OutputTooSmall
        | KnownDestinationError::CountTooLarge => RsHandheldStatus::ErrCapacity,
        KnownDestinationError::KeyConflict
        | KnownDestinationError::DestinationHashMismatch
        | KnownDestinationError::DuplicateDestination => RsHandheldStatus::ErrCrypto,
        KnownDestinationError::InvalidHeader
        | KnownDestinationError::UnsupportedVersion
        | KnownDestinationError::InvalidLength => RsHandheldStatus::ErrInvalidArg,
    }
}

/// Recall a persisted LXMF delivery public key. Successful recall refreshes its LRU recency.
/// Missing destinations return `Ok` with `*out_found = 0` and a zeroed output key.
///
/// # Safety
/// `ctx` is live with an open node; `destination_hash` is 16 readable bytes; both outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_known_dest_recall(
    ctx: *mut RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    out_public_key: *mut [u8; PUBLIC_KEY_LENGTH],
    out_found: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || destination_hash.is_null()
            || out_public_key.is_null()
            || out_found.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        match node.known_destination_recall(unsafe { &*destination_hash }) {
            Some(public_key) => unsafe {
                *out_public_key = public_key;
                *out_found = 1;
            },
            None => unsafe {
                *out_public_key = [0; PUBLIC_KEY_LENGTH];
                *out_found = 0;
            },
        }
        RsHandheldStatus::Ok
    })
}

/// Learn an LXMF delivery destination/public-key binding. Same-key repeats are strict no-ops;
/// conflicting or destination-mismatched keys fail closed.
///
/// # Safety
/// `ctx` is live with an open node; both input arrays are readable; `out_changed` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_known_dest_learn(
    ctx: *mut RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    public_key: *const [u8; PUBLIC_KEY_LENGTH],
    now_ms: u64,
    out_changed: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || destination_hash.is_null()
            || public_key.is_null()
            || out_changed.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        match node.known_destination_learn(
            unsafe { *destination_hash },
            unsafe { *public_key },
            now_ms,
        ) {
            Ok(changed) => {
                unsafe { *out_changed = i32::from(changed) };
                RsHandheldStatus::Ok
            }
            Err(error) => known_destination_status(error),
        }
    })
}

/// Write the number of durable known delivery destinations.
///
/// # Safety
/// `ctx` is live with an open node; `out_count` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_known_dest_count(
    ctx: *const RsHandheldRns,
    out_count: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_count.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &*ctx }.node {
            Some(p) => unsafe { p.as_ref() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        unsafe { *out_count = node.known_destination_count() as u32 };
        RsHandheldStatus::Ok
    })
}

/// Export the versioned, self-validating known-destination blob.
///
/// # Safety
/// `ctx` is live with an open node; `out` is `out_cap` writable bytes; `out_len` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_known_dest_export(
    ctx: *const RsHandheldRns,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &*ctx }.node {
            Some(p) => unsafe { p.as_ref() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match node.known_destinations_export_into(out) {
            Ok(len) => {
                unsafe { *out_len = len };
                RsHandheldStatus::Ok
            }
            Err(error) => known_destination_status(error),
        }
    })
}

/// Atomically replace the known-destination table from a complete validated blob.
///
/// # Safety
/// `ctx` is live with an open node; `blob` is `blob_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_known_dest_import(
    ctx: *mut RsHandheldRns,
    blob: *const u8,
    blob_len: usize,
    now_ms: u64,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || blob.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let node = match unsafe { &mut *ctx }.node {
            Some(mut p) => unsafe { p.as_mut() },
            None => return RsHandheldStatus::ErrNotReady,
        };
        let blob = unsafe { core::slice::from_raw_parts(blob, blob_len) };
        match node.known_destinations_import(blob, now_ms) {
            Ok(()) => RsHandheldStatus::Ok,
            Err(error) => known_destination_status(error),
        }
    })
}
