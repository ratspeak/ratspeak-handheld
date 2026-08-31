//! Signed announce creation and validation.

use super::*;

/// Max `app_data` an announce can carry, sized to the real single-packet wire bound (`MDU - fixed`)
/// in `rns-lite-core`, NOT an arbitrary cap — so ingest accepts any network-valid announce (a long
/// LXMF display name realistically exceeds a few hundred bytes). rsDeck's own announces are tiny.
pub const RS_HANDHELD_ANNOUNCE_MAX_APP_DATA: usize = MAX_ANNOUNCE_APP_DATA;

/// Max `ratchet_export` output: ring blob + identity signature. Size C-side buffers with this.
pub const RS_HANDHELD_RATCHET_RING_BLOB_MAX: usize = RATCHET_RING_BLOB_MAX + SIGNATURE_LENGTH;
/// Max `peer_ratchets_export` output.
pub const RS_HANDHELD_PEER_RATCHETS_BLOB_MAX: usize = PEER_RATCHETS_BLOB_MAX;
/// Max signed announce-order state output.
pub const RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX: usize =
    ANNOUNCE_WIRE_STATE_BLOB_LEN + SIGNATURE_LENGTH;
// The C header hardcodes these; a silent drift would undersize caller buffers.
const _: () = assert!(RS_HANDHELD_RATCHET_RING_BLOB_MAX == 2690);
const _: () = assert!(RS_HANDHELD_PEER_RATCHETS_BLOB_MAX == 1826);
const _: () = assert!(RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX == 71);

// ABI pin: the C header's `#define RS_HANDHELD_ANNOUNCE_MAX_APP_DATA` and the inline `app_data`
// field of `rs_handheld_announce_event_t` MUST equal this. If MTU/field sizes ever change this,
// the build breaks here and forces the header to be updated in lockstep.
const _: () = assert!(RS_HANDHELD_ANNOUNCE_MAX_APP_DATA == 333);

/// A validated inbound announce, for the C++ `AnnounceManager`. Plain `#[repr(C)]` data —
/// `app_data` is an inline fixed buffer (`app_data_len` valid bytes; rest zero) so the whole
/// event crosses the ABI as one caller-owned struct. `has_ratchet` is 0/1; when 0, `ratchet`
/// is zeroed.
#[repr(C)]
pub struct RsHandheldAnnounceEvent {
    pub destination_hash: [u8; DESTINATION_LENGTH],
    pub identity_hash: [u8; DESTINATION_LENGTH],
    pub public_key: [u8; PUBLIC_KEY_LENGTH],
    pub has_ratchet: i32,
    pub ratchet: [u8; RATCHET_LEN],
    pub app_data_len: u32,
    pub app_data: [u8; RS_HANDHELD_ANNOUNCE_MAX_APP_DATA],
}

/// Build a signed `lxmf.delivery` `announce_data` payload for the active identity.
///
/// The announce `random_hash` is composed here as `rng_seed(5) || announce_order_be(5)`.
/// `announce_order` must come from a successfully persisted+committed announce-state candidate
/// whenever `ratchet` is non-null. This side has no RNG/clock, so the caller supplies both.
/// `ratchet` (32 bytes) is optional: null means no ratchet. `app_data` may be null
/// iff `app_data_len == 0`. The payload is written into `out` (capacity `out_cap`) and `*out_len`
/// set to its byte length; `out_dest_hash` (16) receives the announce destination (the packet's
/// destination_hash). The caller builds the packet header and MUST set its context_flag iff
/// `ratchet` is non-null.
///
/// Returns `ErrNotReady` if no identity is loaded, `ErrCapacity` if `out` is too small or
/// `app_data_len` exceeds `RS_HANDHELD_ANNOUNCE_MAX_APP_DATA`, `ErrInvalidArg` on null/contract
/// violations.
///
/// # Safety
/// `ctx` live; `rng_seed` 5 readable bytes; `ratchet` null or 32 readable bytes; `app_data`
/// `app_data_len` readable bytes (or null iff len 0); `out` `out_cap` writable bytes; `out_len`
/// a writable `usize`; `out_dest_hash` 16 writable bytes. `out` must NOT overlap any input buffer.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_announce(
    ctx: *const RsHandheldRns,
    rng_seed: *const [u8; RANDOM_SEED_LEN],
    announce_order: u64,
    ratchet: *const [u8; RATCHET_LEN],
    app_data: *const u8,
    app_data_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_dest_hash: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        // Validate the pointer/argument contract BEFORE the stateful identity check, so a
        // malformed call surfaces ErrInvalidArg regardless of whether an identity is loaded.
        if ctx.is_null()
            || rng_seed.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_dest_hash.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if app_data.is_null() && app_data_len != 0 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if app_data_len > RS_HANDHELD_ANNOUNCE_MAX_APP_DATA {
            return RsHandheldStatus::ErrCapacity;
        }
        // SAFETY: `ctx` is non-null and valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: pointers are non-null and correctly sized per the contract.
        let ratchet_ref = if ratchet.is_null() {
            None
        } else {
            Some(unsafe { &*ratchet })
        };
        // A ratcheted announce may only claim the exact wire value whose state
        // was durably committed. Unratcheted fallback remains available when
        // persistence is broken, as required by handheld availability policy.
        if let Some(ratchet) = ratchet_ref {
            let c = unsafe { &*ctx };
            if c.announce_wire.last_value() != Some(announce_order)
                || c.ratchet_ring.current_public_key().as_ref() != Some(ratchet)
            {
                return RsHandheldStatus::ErrNotReady;
            }
        }
        let random_hash = compose_random_hash(unsafe { &*rng_seed }, announce_order);
        let app = if app_data_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(app_data, app_data_len) }
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };

        match id.create_lxmf_announce(&random_hash, ratchet_ref, app, out_slice) {
            Ok(n) => {
                // SAFETY: out_len / out_dest_hash non-null and writable per the contract.
                unsafe {
                    *out_len = n;
                    *out_dest_hash = id.lxmf_delivery_hash();
                }
                RsHandheldStatus::Ok
            }
            Err(AnnounceError::OutputTooSmall | AnnounceError::AppDataTooLong) => {
                RsHandheldStatus::ErrCapacity
            }
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Parse + validate an announce `payload` against `dest`/`context_flag` (+ optional first-seen
/// `known` key) and fill `ev`. Shared by `rs_handheld_rns_announce_ingest` and the packet-ingress path.
pub(super) fn fill_announce_event(
    payload: &[u8],
    dest: &[u8; DESTINATION_LENGTH],
    context_flag: bool,
    known: Option<&[u8; PUBLIC_KEY_LENGTH]>,
    ev: &mut RsHandheldAnnounceEvent,
) -> RsHandheldStatus {
    let view = match AnnounceView::parse(payload, context_flag, RS_HANDHELD_ANNOUNCE_MAX_APP_DATA) {
        Ok(v) => v,
        Err(AnnounceError::AppDataTooLong) => return RsHandheldStatus::ErrCapacity,
        Err(_) => return RsHandheldStatus::ErrInvalidArg,
    };
    // Scratch holds signed_data; sized from rns-lite-core so it can never drift from what
    // verify_signature writes (dest||pub||name_hash||random_hash||[ratchet]||app_data).
    let mut scratch = [0u8; SIGNED_DATA_MAX];
    let identity_hash = match view.validate(dest, known, &mut scratch) {
        Ok(h) => h,
        Err(
            AnnounceError::DestinationHashMismatch
            | AnnounceError::SignatureInvalid
            | AnnounceError::InvalidPublicKey
            | AnnounceError::PublicKeyChanged,
        ) => return RsHandheldStatus::ErrCrypto,
        Err(_) => return RsHandheldStatus::ErrInternal,
    };
    ev.destination_hash = *dest;
    ev.identity_hash = identity_hash;
    ev.public_key = view.public_key;
    match view.ratchet {
        Some(r) => {
            ev.has_ratchet = 1;
            ev.ratchet = r;
        }
        None => {
            ev.has_ratchet = 0;
            ev.ratchet = [0u8; RATCHET_LEN];
        }
    }
    ev.app_data = [0u8; RS_HANDHELD_ANNOUNCE_MAX_APP_DATA];
    ev.app_data[..view.app_data.len()].copy_from_slice(view.app_data);
    ev.app_data_len = view.app_data.len() as u32;
    RsHandheldStatus::Ok
}

/// Parse + validate an inbound `announce_data` payload against `dest_hash` (from the packet
/// header), filling `out_event` for the C++ `AnnounceManager`. `context_flag` (0/1) MUST match
/// the packet's flag — it tells the parser whether a ratchet is present.
///
/// Validation here is signature + destination-hash binding, plus (if `known_public_key` is
/// non-null) the first-seen public-key continuity / hash-collision defense — `ErrCrypto` if the
/// announced key differs from `known_public_key` for this destination. It does NOT do the stateful
/// policy checks Python's `validate_announce` also performs: announce-acceptance POLICY is the C++
/// `AnnounceManager`'s responsibility (rsDeck keeps the contact/name cache and the KEY MAP
/// C++-owned). NOTE: no blackhole/ignore list is implemented on either side today — Python's
/// blackholed-identity filtering has no firmware equivalent (recorded in the coverage posture).
/// `RS_HANDHELD_OK` therefore means "signature + binding valid", not "safe to learn
/// unconditionally".
///
/// Returns `ErrCrypto` if signature/binding/key-continuity is invalid, `ErrCapacity` if the
/// announce's `app_data` exceeds `RS_HANDHELD_ANNOUNCE_MAX_APP_DATA`, `ErrInvalidArg` on null or
/// malformed (too-short) input.
///
/// # Safety
/// `data` points to `data_len` readable bytes; `dest_hash` 16 readable bytes; `known_public_key`
/// null or 64 readable bytes; `out_event` a writable, properly-aligned `RsHandheldAnnounceEvent`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_announce_ingest(
    data: *const u8,
    data_len: usize,
    dest_hash: *const [u8; DESTINATION_LENGTH],
    context_flag: i32,
    known_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out_event: *mut RsHandheldAnnounceEvent,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || dest_hash.is_null() || out_event.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null and correctly sized per the contract.
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let dest = unsafe { &*dest_hash };
        let known = if known_public_key.is_null() {
            None
        } else {
            Some(unsafe { &*known_public_key })
        };
        let ev = unsafe { &mut *out_event };
        fill_announce_event(data, dest, context_flag != 0, known, ev)
    })
}

// ---- Announce ratchets: durable own ring, wire ordering, and accepted-peer memory ----------
//
// Upstream posture: always enable, never enforce. The host owns clocks, entropy, and storage.
// Secret-ring and announce-order candidates are prepared without mutating live state, signed,
// persisted by C++, then committed through an exact-candidate digest. Inbound packet validation
// never writes the peer table; C++ calls peer_ratchet_remember only after freshness + KeyMap accept.

pub(super) const RATCHET_PREP_UNCHANGED: i32 = 0;
pub(super) const RATCHET_PREP_PERSIST_METADATA: i32 = 1;
pub(super) const RATCHET_PREP_ROTATED: i32 = 2;
pub(super) const RATCHET_PREP_FULL_RING_PROTECTED: i32 = 3;

/// Prepare a signed ring candidate directly in caller storage. Live keys remain unchanged.
/// `out_action` is one of `RS_HANDHELD_RATCHET_PREP_*`; only actions 1/2 produce bytes that must
/// be durably stored and passed to `ratchet_commit`. `out_current_pub` is the candidate current
/// key and MUST NOT be advertised for action 2 until commit succeeds.
///
/// # Safety
/// All pointers are live and sized as declared; `out` has `out_cap` writable bytes.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_ratchet_prepare(
    ctx: *mut RsHandheldRns,
    entropy: *const [u8; RATCHET_LEN],
    wall_secs: u64,
    uptime_ms: u64,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_action: *mut i32,
    out_current_pub: *mut [u8; RATCHET_LEN],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || entropy.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_action.is_null()
            || out_current_pub.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        c.pending_ratchet = None;
        if out_cap < SIGNATURE_LENGTH {
            return RsHandheldStatus::ErrCapacity;
        }
        let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let clock = RatchetClock::from_host(wall_secs, uptime_ms / 1000);
        let prepared = match c.ratchet_ring.prepare_rotation_into(
            unsafe { *entropy },
            clock,
            &mut out[..out_cap - SIGNATURE_LENGTH],
        ) {
            Ok(prepared) => prepared,
            Err(rns_lite_core::ratchet::RatchetError::OutputTooSmall) => {
                return RsHandheldStatus::ErrCapacity;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };

        let (action, blob_len, current) = match prepared {
            RatchetPreparation::Unchanged => (
                RATCHET_PREP_UNCHANGED,
                None,
                c.ratchet_ring.current_public_key(),
            ),
            RatchetPreparation::PersistMetadata { blob_len } => (
                RATCHET_PREP_PERSIST_METADATA,
                Some(blob_len),
                c.ratchet_ring.current_public_key(),
            ),
            RatchetPreparation::Rotated {
                blob_len,
                public_key,
            } => (RATCHET_PREP_ROTATED, Some(blob_len), Some(public_key)),
            RatchetPreparation::FullRingProtected => (
                RATCHET_PREP_FULL_RING_PROTECTED,
                None,
                c.ratchet_ring.current_public_key(),
            ),
        };
        let current = match current {
            Some(current) => current,
            None => return RsHandheldStatus::ErrInternal,
        };

        let total = if let Some(blob_len) = blob_len {
            let signature = id.sign(&out[..blob_len]);
            out[blob_len..blob_len + SIGNATURE_LENGTH].copy_from_slice(&signature);
            c.pending_ratchet = Some(rns_lite_core::wire::sha256(&out[..blob_len]));
            blob_len + SIGNATURE_LENGTH
        } else {
            0
        };
        unsafe {
            *out_len = total;
            *out_action = action;
            *out_current_pub = current;
        }
        RsHandheldStatus::Ok
    })
}

/// Commit the exact signed candidate most recently returned by `ratchet_prepare`.
/// Call only after the host's durable write succeeds.
///
/// # Safety
/// `ctx` is live; `data` names `data_len` readable bytes; `out_current_pub` is 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_ratchet_commit(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
    out_current_pub: *mut [u8; RATCHET_LEN],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || data.is_null()
            || data_len < SIGNATURE_LENGTH
            || out_current_pub.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let (blob, signature) = data.split_at(data_len - SIGNATURE_LENGTH);
        if id.sign(blob).as_slice() != signature {
            return RsHandheldStatus::ErrCrypto;
        }
        if c.pending_ratchet != Some(rns_lite_core::wire::sha256(blob)) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if c.ratchet_ring.commit_prepared_blob(blob).is_err() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        c.pending_ratchet = None;
        let current = match c.ratchet_ring.current_public_key() {
            Some(current) => current,
            None => return RsHandheldStatus::ErrInternal,
        };
        unsafe { *out_current_pub = current };
        RsHandheldStatus::Ok
    })
}

/// Current own ratchet public key without rotating. `ErrNotReady` if the ring never rotated.
///
/// # Safety
/// `ctx` live; `out_pub` 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_ratchet_current(
    ctx: *const RsHandheldRns,
    out_pub: *mut [u8; RATCHET_LEN],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_pub.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointers valid per the contract.
        match unsafe { &*ctx }.ratchet_ring.current_public_key() {
            Some(p) => {
                unsafe { *out_pub = p };
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Export the own ratchet ring as `blob || identity_signature(64)` for host persistence.
/// Losing this blob across reboots means ratcheted inbound sent against pre-reboot announces
/// cannot be decrypted — persist it whenever a rotation happened. `ErrNotReady` without an
/// identity; `ErrCapacity` if `out_cap` is too small (size with
/// `RS_HANDHELD_RATCHET_RING_BLOB_MAX`).
///
/// # Safety
/// `ctx` live with a loaded identity; `out` `out_cap` writable bytes; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_ratchet_export(
    ctx: *const RsHandheldRns,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let c = unsafe { &*ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        if out_cap < SIGNATURE_LENGTH {
            return RsHandheldStatus::ErrCapacity;
        }
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let n = match c
            .ratchet_ring
            .serialize_into(&mut out_slice[..out_cap - SIGNATURE_LENGTH])
        {
            Ok(n) => n,
            Err(rns_lite_core::ratchet::RatchetError::OutputTooSmall) => {
                return RsHandheldStatus::ErrCapacity;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let total = n + SIGNATURE_LENGTH;
        let sig = id.sign(&out_slice[..n]);
        out_slice[n..total].copy_from_slice(&sig);
        unsafe { *out_len = total };
        RsHandheldStatus::Ok
    })
}

/// Restore the own ratchet ring from a blob written by `rs_handheld_rns_ratchet_export`.
/// The identity signature is re-derived and compared (Ed25519 is deterministic), so a
/// tampered/foreign blob is rejected with `ErrCrypto` and the current ring is left untouched.
///
/// # Safety
/// `ctx` live with a loaded identity; `data` `data_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_ratchet_seed(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || data_len < SIGNATURE_LENGTH {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointers valid per the contract.
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let (blob, sig) = data.split_at(data_len - SIGNATURE_LENGTH);
        if id.sign(blob) != sig {
            return RsHandheldStatus::ErrCrypto;
        }
        if c.ratchet_ring.load_persisted(blob).is_err() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        c.pending_ratchet = None;
        RsHandheldStatus::Ok
    })
}

/// Export the signed 40-bit announce-order state directly into caller storage.
///
/// # Safety
/// `ctx` is live; `out` names `out_cap` writable bytes; `out_len` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_announce_state_export(
    ctx: *const RsHandheldRns,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if out_cap < RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX {
            return RsHandheldStatus::ErrCapacity;
        }
        let c = unsafe { &*ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let n = match c.announce_wire.serialize_into(out) {
            Ok(n) => n,
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let signature = id.sign(&out[..n]);
        out[n..n + SIGNATURE_LENGTH].copy_from_slice(&signature);
        unsafe { *out_len = n + SIGNATURE_LENGTH };
        RsHandheldStatus::Ok
    })
}

/// Restore identity-signed announce-order state. Rejection leaves live state unchanged.
///
/// # Safety
/// `ctx` is live and `data` names `data_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_announce_state_seed(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || data_len != RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let (blob, signature) = data.split_at(ANNOUNCE_WIRE_STATE_BLOB_LEN);
        if id.sign(blob).as_slice() != signature {
            return RsHandheldStatus::ErrCrypto;
        }
        let state = match AnnounceWireState::deserialize(blob) {
            Ok(state) => state,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        c.announce_wire = state;
        c.pending_announce_wire = None;
        RsHandheldStatus::Ok
    })
}

/// Prepare the next signed announce-order state. `wall_secs == 0` selects the durable logical
/// fallback. `out_ready == 0` means real wall time did not advance; no bytes were produced.
///
/// # Safety
/// `ctx` is live; `out` names `out_cap` writable bytes; all output pointers are writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_announce_state_prepare(
    ctx: *mut RsHandheldRns,
    wall_secs: u64,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_wire_value: *mut u64,
    out_ready: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_wire_value.is_null()
            || out_ready.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if out_cap < RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX {
            return RsHandheldStatus::ErrCapacity;
        }
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        c.pending_announce_wire = None;
        let candidate = match c.announce_wire.prepare_next(if wall_secs == 0 {
            None
        } else {
            Some(wall_secs)
        }) {
            Ok(Some(candidate)) => candidate,
            Ok(None) => {
                unsafe {
                    *out_len = 0;
                    *out_wire_value = c.announce_wire.last_value().unwrap_or(0);
                    *out_ready = 0;
                }
                return RsHandheldStatus::Ok;
            }
            Err(_) => return RsHandheldStatus::ErrCapacity,
        };
        let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let n = match candidate.serialize_into(out) {
            Ok(n) => n,
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let signature = id.sign(&out[..n]);
        out[n..n + SIGNATURE_LENGTH].copy_from_slice(&signature);
        c.pending_announce_wire = Some(rns_lite_core::wire::sha256(&out[..n]));
        unsafe {
            *out_len = n + SIGNATURE_LENGTH;
            *out_wire_value = candidate.last_value().unwrap_or(0);
            *out_ready = 1;
        }
        RsHandheldStatus::Ok
    })
}

/// Commit the exact announce-order candidate after its durable write succeeds.
///
/// # Safety
/// `ctx` is live; `data` names `data_len` readable bytes; `out_wire_value` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_announce_state_commit(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
    out_wire_value: *mut u64,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || data.is_null()
            || data_len != RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX
            || out_wire_value.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &mut *ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let (blob, signature) = data.split_at(ANNOUNCE_WIRE_STATE_BLOB_LEN);
        if id.sign(blob).as_slice() != signature {
            return RsHandheldStatus::ErrCrypto;
        }
        if c.pending_announce_wire != Some(rns_lite_core::wire::sha256(blob)) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let candidate = match AnnounceWireState::deserialize(blob) {
            Ok(candidate) => candidate,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        let value = match c.announce_wire.commit_prepared(candidate) {
            Ok(value) => value,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        c.pending_announce_wire = None;
        unsafe { *out_wire_value = value };
        RsHandheldStatus::Ok
    })
}

/// Export the peer-ratchet table for host persistence (plain blob — public keys only).
/// `ErrCapacity` if `out_cap` is too small (size with `RS_HANDHELD_PEER_RATCHETS_BLOB_MAX`).
///
/// # Safety
/// `ctx` live; `out` `out_cap` writable bytes; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_peer_ratchets_export(
    ctx: *const RsHandheldRns,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let out = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let n = match unsafe { &*ctx }.peer_ratchets.serialize_into(out) {
            Ok(n) => n,
            Err(rns_lite_core::ratchet::RatchetError::OutputTooSmall) => {
                return RsHandheldStatus::ErrCapacity;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        unsafe { *out_len = n };
        RsHandheldStatus::Ok
    })
}

/// Restore the peer-ratchet table from a blob written by `rs_handheld_rns_peer_ratchets_export`.
///
/// # Safety
/// `ctx` live; `data` `data_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_peer_ratchets_seed(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointers valid per the contract.
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        if unsafe { &mut *ctx }
            .peer_ratchets
            .load_persisted(data)
            .is_err()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        RsHandheldStatus::Ok
    })
}

/// Restore a peer table and conservatively anchor every unknown/cross-boot age. `out_changed`
/// tells the host to persist the resulting v2 metadata immediately.
///
/// # Safety
/// `ctx` is live; `data` names `data_len` readable bytes; `out_changed` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_peer_ratchets_seed_at(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
    wall_secs: u64,
    uptime_ms: u64,
    out_changed: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || out_changed.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let table = &mut unsafe { &mut *ctx }.peer_ratchets;
        if table.load_persisted(data).is_err() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let changed =
            table.anchor_unknown_ages(RatchetClock::from_host(wall_secs, uptime_ms / 1000));
        unsafe { *out_changed = i32::from(changed) };
        RsHandheldStatus::Ok
    })
}

/// Commit a peer's announced public ratchet only after the C++ freshness and KeyMap gates accept
/// the announce. Re-observing the same key is a no-op and cannot refresh expiry.
///
/// # Safety
/// `ctx` is live; both fixed-size input pointers are readable; `out_changed` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_peer_ratchet_remember(
    ctx: *mut RsHandheldRns,
    destination_hash: *const [u8; DESTINATION_LENGTH],
    ratchet: *const [u8; RATCHET_LEN],
    wall_secs: u64,
    uptime_ms: u64,
    out_changed: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || destination_hash.is_null() || ratchet.is_null() || out_changed.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let changed = unsafe { &mut *ctx }.peer_ratchets.remember(
            unsafe { *destination_hash },
            unsafe { *ratchet },
            RatchetClock::from_host(wall_secs, uptime_ms / 1000),
        );
        unsafe { *out_changed = i32::from(changed) };
        RsHandheldStatus::Ok
    })
}
