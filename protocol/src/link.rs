//! Link handshake and session cryptography.

use super::*;
//
// rsDeck is an endpoint: INITIATOR when it opens a link to send a large message, RESPONDER when a
// peer links to it. These are the link cryptographic + wire primitives — the C++/ratdeck owner
// drives the state machine, timers, packet send/receive, and holds each link's 64-byte session key
// for the link's lifetime (passing it back per frame). The Rust side never holds link state.

/// LINKREQUEST payload size: x25519_pub(32) || ed25519_pub(32) || signalling(3).
pub const RS_HANDHELD_LINK_REQUEST_LEN: usize = LINK_REQUEST_LEN;
/// LRPROOF payload size: signature(64) || responder_x25519_pub(32) || signalling(3).
pub const RS_HANDHELD_LINK_PROOF_LEN: usize = LINK_PROOF_LEN;
/// Derived link session key length: signing(32) || encryption(32).
pub const RS_HANDHELD_LINK_KEY_LEN: usize = LINK_KEY_LENGTH;

// ABI pins: these MUST equal the C header `#define`s.
const _: () = {
    assert!(RS_HANDHELD_LINK_REQUEST_LEN == 67);
    assert!(RS_HANDHELD_LINK_PROOF_LEN == 99);
    assert!(RS_HANDHELD_LINK_KEY_LEN == 64);
};

/// Build a LINKREQUEST payload (initiator → destination) into `out`, `*out_len` its length (67).
/// `x25519_priv` (32) + `ed25519_seed` (32) are the initiator's per-link EPHEMERAL key material
/// (caller entropy; `no_std`: no RNG). The initiator MUST retain `x25519_priv` to derive the session
/// key once the proof arrives. `mode` MUST be 1 (AES-256-CBC — rsDeck's only enabled mode); any other
/// value returns `ErrUnsupported`. `mtu` is masked to 21 bits (use 500 for the standard link).
///
/// # Safety
/// `x25519_priv`/`ed25519_seed` 32 readable bytes each; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_request_build(
    x25519_priv: *const [u8; 32],
    ed25519_seed: *const [u8; 32],
    mode: u8,
    mtu: u32,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if x25519_priv.is_null() || ed25519_seed.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match rns_link::build_link_request(
            unsafe { &*x25519_priv },
            unsafe { &*ed25519_seed },
            SignallingData::new(mode, mtu),
            out_slice,
        ) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(LinkError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(LinkError::UnsupportedMode) => RsHandheldStatus::ErrUnsupported,
            Err(_) => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Parse a LINKREQUEST payload (responder side). Writes the initiator's ephemeral X25519 public key
/// (needed for [`rs_handheld_rns_link_derive`]) to `out_x25519_pub` (required); the initiator's ephemeral
/// Ed25519 public key to `out_ed25519_pub`, the negotiated `mode`/`mtu` to `out_mode`/`out_mtu` (all
/// optional — may be null). `ErrInvalidArg` on null required args or a malformed length.
///
/// # Safety
/// `data` `data_len` readable bytes; `out_x25519_pub` 32 writable; `out_ed25519_pub` null or 32
/// writable; `out_mode` null or a writable u8; `out_mtu` null or a writable u32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_request_parse(
    data: *const u8,
    data_len: usize,
    out_x25519_pub: *mut [u8; 32],
    out_ed25519_pub: *mut [u8; 32],
    out_mode: *mut u8,
    out_mtu: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out_x25519_pub.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let view = match LinkRequestView::parse(data) {
            Ok(v) => v,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        unsafe {
            *out_x25519_pub = view.peer_x25519_pub;
            if !out_ed25519_pub.is_null() {
                *out_ed25519_pub = view.peer_ed25519_pub;
            }
            if !out_mode.is_null() {
                *out_mode = view.signalling.mode;
            }
            if !out_mtu.is_null() {
                *out_mtu = view.signalling.mtu;
            }
        }
        RsHandheldStatus::Ok
    })
}

/// Compute the 16-byte link id from the destination hash and the LINKREQUEST payload. Both peers must
/// agree on this id (it salts the session key). Stateless. `ErrInvalidArg` on null.
///
/// # Safety
/// `dest_hash` 16 readable bytes; `request` `request_len` readable bytes; `out_link_id` 16 writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_id(
    dest_hash: *const [u8; DESTINATION_LENGTH],
    request: *const u8,
    request_len: usize,
    out_link_id: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if dest_hash.is_null() || request.is_null() || out_link_id.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let request = unsafe { core::slice::from_raw_parts(request, request_len) };
        let id = rns_link::compute_link_id(unsafe { &*dest_hash }, request);
        unsafe { *out_link_id = id };
        RsHandheldStatus::Ok
    })
}

/// Build an LRPROOF payload (responder side) into `out`, `*out_len` its length (99). Signs
/// `link_id || responder_x25519_pub || identity_ed25519_pub || signalling` with the ACTIVE identity's
/// long-term Ed25519 key (this authenticates the link). `responder_x25519_priv` (32) is the
/// responder's per-link EPHEMERAL X25519 key (caller entropy; retain it for the key derivation).
/// `mode` MUST be 1 (AES-256); any other value returns `ErrUnsupported`. `mtu` is masked to 21 bits.
/// `ErrNotReady` if no identity is loaded; `ErrCapacity` if `out` is too small.
///
/// # Safety
/// `ctx` live with a loaded identity; `responder_x25519_priv` 32 readable bytes; `link_id` 16;
/// `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_link_proof_build(
    ctx: *const RsHandheldRns,
    responder_x25519_priv: *const [u8; 32],
    link_id: *const [u8; DESTINATION_LENGTH],
    mode: u8,
    mtu: u32,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || responder_x25519_priv.is_null()
            || link_id.is_null()
            || out.is_null()
            || out_len.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match rns_link::build_link_proof(
            id,
            unsafe { &*responder_x25519_priv },
            unsafe { &*link_id },
            SignallingData::new(mode, mtu),
            out_slice,
        ) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(LinkError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(LinkError::UnsupportedMode) => RsHandheldStatus::ErrUnsupported,
            Err(_) => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Validate an LRPROOF (initiator side) against the destination identity's 64-byte public key (known
/// from its announce) and the link id. Writes 1/0 to `*out_valid`, and — on a structurally valid
/// proof — the responder's ephemeral X25519 public key to `out_responder_x25519_pub` (optional;
/// needed for [`rs_handheld_rns_link_derive`]). A malformed length, a non-AES-256 mode, or a bad signature
/// is reported as `*out_valid = 0` with `RS_HANDHELD_OK` (not an arg error) — mirroring
/// `rs_handheld_rns_proof_validate`. Stateless. `ErrInvalidArg` only on null required args.
///
/// ACTIVATION CONTRACT (the C++ link state machine is responsible — NOT done here): a valid proof
/// authenticates the responder but does NOT by itself make the link ACTIVE. After this returns valid,
/// the initiator MUST derive the session key ([`rs_handheld_rns_link_derive`]), then send an LRRTT packet —
/// a [`rs_handheld_rns_link_encrypt`]-wrapped msgpack float of the measured RTT — to the link destination.
/// The RESPONDER's link only transitions to ACTIVE once it receives and decrypts that RTT packet
/// (Reticulum handshake message 3). Skipping it leaves the responder pending until it times out and
/// tears the link down. The RTT measurement, msgpack framing, send sequencing, and ACTIVE/STALE/CLOSED
/// state all live in the C++ owner; this call provides only the proof-validation primitive.
///
/// # Safety
/// `identity_public_key` 64 readable bytes; `link_id` 16; `proof` `proof_len` readable bytes;
/// `out_responder_x25519_pub` null or 32 writable; `out_valid` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_proof_validate(
    identity_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    link_id: *const [u8; DESTINATION_LENGTH],
    proof: *const u8,
    proof_len: usize,
    out_responder_x25519_pub: *mut [u8; 32],
    out_valid: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if identity_public_key.is_null()
            || link_id.is_null()
            || proof.is_null()
            || out_valid.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let proof = unsafe { core::slice::from_raw_parts(proof, proof_len) };
        let view = match LinkProofView::parse(proof) {
            Ok(v) => v,
            Err(_) => {
                // Structurally malformed -> not valid (not an arg error).
                unsafe { *out_valid = 0 };
                return RsHandheldStatus::Ok;
            }
        };
        let valid = view.validate(unsafe { &*identity_public_key }, unsafe { &*link_id });
        unsafe {
            *out_valid = valid as i32;
            if !out_responder_x25519_pub.is_null() {
                *out_responder_x25519_pub = view.responder_x25519_pub;
            }
        }
        RsHandheldStatus::Ok
    })
}

/// Derive the 64-byte link session key (`signing(32) || encryption(32)`) into `out_key`. Both peers
/// call this with their own ephemeral X25519 private key and the peer's ephemeral X25519 public key
/// plus the shared `link_id`, and arrive at the identical key. The C++ owner stores `out_key` for the
/// link's lifetime (and MUST zeroize it on close). Stateless. `ErrInvalidArg` on null.
///
/// # Safety
/// `my_x25519_priv`/`peer_x25519_pub` 32 readable bytes each; `link_id` 16; `out_key` 64 writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_derive(
    my_x25519_priv: *const [u8; 32],
    peer_x25519_pub: *const [u8; 32],
    link_id: *const [u8; DESTINATION_LENGTH],
    out_key: *mut [u8; LINK_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if my_x25519_priv.is_null()
            || peer_x25519_pub.is_null()
            || link_id.is_null()
            || out_key.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let keys = LinkKeys::derive(
            unsafe { &*my_x25519_priv },
            unsafe { &*peer_x25519_pub },
            unsafe { &*link_id },
        );
        unsafe { *out_key = *keys.combined() };
        RsHandheldStatus::Ok
    })
}

/// Encrypt one link frame with the session `key` (64 bytes from [`rs_handheld_rns_link_derive`]). Output:
/// `IV(16) || AES-256-CBC(PKCS7(pt)) || HMAC-SHA256(32)`, written to `out`, `*out_len` its length.
/// `iv` (16) is caller entropy and MUST be fresh per frame (reuse leaks the CBC key-stream). `pt` may
/// be null iff `pt_len == 0`. `ErrCapacity` if too large / `out` too small.
///
/// # Safety
/// `key` 64 readable bytes; `pt` `pt_len` readable (or null iff 0); `iv` 16; `out` `out_cap`
/// writable; `out_len` writable. `out` must NOT overlap inputs.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_link_encrypt(
    key: *const [u8; LINK_KEY_LENGTH],
    pt: *const u8,
    pt_len: usize,
    iv: *const [u8; 16],
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if key.is_null() || iv.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if pt.is_null() && pt_len != 0 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let plaintext = if pt_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(pt, pt_len) }
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let keys = LinkKeys::from_combined(unsafe { &*key });
        match rns_link::link_encrypt(&keys, plaintext, unsafe { &*iv }, out_slice) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(rns_lite_core::crypto::CryptoError::PlaintextTooLong)
            | Err(rns_lite_core::crypto::CryptoError::OutputTooSmall) => {
                RsHandheldStatus::ErrCapacity
            }
            Err(_) => RsHandheldStatus::ErrCrypto,
        }
    })
}

/// Decrypt one link frame with the session `key`. Writes the plaintext to `out`, `*out_len` its
/// length. Every malformed/forged frame returns `ErrCrypto` (no padding/HMAC oracle). `ErrCapacity`
/// if `out` is too small.
///
/// # Safety
/// `key` 64 readable bytes; `data` `data_len` readable; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_link_decrypt(
    key: *const [u8; LINK_KEY_LENGTH],
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if key.is_null() || data.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let keys = LinkKeys::from_combined(unsafe { &*key });
        match rns_link::link_decrypt(&keys, data, out_slice) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(rns_lite_core::crypto::CryptoError::OutputTooSmall) => {
                RsHandheldStatus::ErrCapacity
            }
            Err(_) => RsHandheldStatus::ErrCrypto,
        }
    })
}
