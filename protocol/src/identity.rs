//! Identity loading, creation, and hash derivation.

use super::*;
// Byte-exact with Python Reticulum via `rns-lite-core` (rsReticulumLite; see vectors/).

/// Validate a raw 64-byte identity and derive its hashes WITHOUT a context.
/// `out_identity_hash` (16) is required; `out_public_key` (64) is optional (may be null).
///
/// # Safety
/// `private_key` must point to 64 readable bytes; `out_identity_hash` to 16 writable bytes;
/// `out_public_key`, if non-null, to 64 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_validate_identity(
    private_key: *const [u8; PRIVATE_KEY_LENGTH],
    out_identity_hash: *mut [u8; DESTINATION_LENGTH],
    out_public_key: *mut [u8; PUBLIC_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if private_key.is_null() || out_identity_hash.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null and correctly sized per the contract.
        let id = LocalIdentity::from_private_key(unsafe { &*private_key });
        unsafe { *out_identity_hash = *id.identity_hash() };
        if !out_public_key.is_null() {
            unsafe { *out_public_key = *id.public_key() };
        }
        RsHandheldStatus::Ok
    })
}

/// Derive an identity from caller-supplied entropy: the 64 bytes become the raw private key
/// (any 64 bytes form a valid Reticulum identity). Rust generates no randomness here (no_std);
/// the caller supplies platform entropy. Writes the private key + identity hash.
///
/// # Safety
/// `entropy` / `out_private_key` must be 64 bytes; `out_identity_hash` 16 bytes; all non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_create_identity(
    entropy: *const [u8; PRIVATE_KEY_LENGTH],
    out_private_key: *mut [u8; PRIVATE_KEY_LENGTH],
    out_identity_hash: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if entropy.is_null() || out_private_key.is_null() || out_identity_hash.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null and correctly sized per the contract.
        let id = LocalIdentity::from_private_key(unsafe { &*entropy });
        unsafe {
            *out_private_key = *id.private_key();
            *out_identity_hash = *id.identity_hash();
        }
        RsHandheldStatus::Ok
    })
}

/// Load a raw 64-byte identity into the context (becomes the active identity).
///
/// # Safety
/// `ctx` must be a live context from init; `private_key` 64 readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_load_identity(
    ctx: *mut RsHandheldRns,
    private_key: *const [u8; PRIVATE_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        // SAFETY: per the contract.
        match (unsafe { ctx.as_mut() }, unsafe { private_key.as_ref() }) {
            (Some(ctx), Some(prv)) => {
                let id = LocalIdentity::from_private_key(prv);
                // Own-announce/path-request guard: registered dests belong to the ACTIVE
                // identity, so an identity (re)load resets the set before registering.
                if let Some(mut node) = ctx.node {
                    // SAFETY: node targets the caller-owned open_transport buffer, live per ABI.
                    let node = unsafe { node.as_mut() };
                    node.clear_own_destinations();
                    let _ = node.register_own_destination(id.lxmf_delivery_hash());
                }
                // Every ratchet/control blob is identity-bound. Reset before
                // installing even the same identity; boot then seeds verified
                // state explicitly. Old private keys are zeroized on assignment.
                ctx.ratchet_ring = RatchetRing::new();
                ctx.peer_ratchets = PeerRatchets::new();
                ctx.announce_wire = AnnounceWireState::new();
                ctx.pending_ratchet = None;
                ctx.pending_announce_wire = None;
                ctx.own_path_request = None;
                ctx.identity = Some(id);
                RsHandheldStatus::Ok
            }
            _ => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Write the active identity hash (16 bytes). `ERR_NOT_READY` if no identity is loaded.
///
/// # Safety
/// `ctx` live; `out` 16 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_identity_hash(
    ctx: *const RsHandheldRns,
    out: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        // SAFETY: per the contract.
        match (unsafe { ctx.as_ref() }, unsafe { out.as_mut() }) {
            (Some(ctx), Some(out)) => match &ctx.identity {
                Some(id) => {
                    *out = *id.identity_hash();
                    RsHandheldStatus::Ok
                }
                None => RsHandheldStatus::ErrNotReady,
            },
            _ => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Write the active identity's `lxmf.delivery` destination hash (16 bytes).
/// `ERR_NOT_READY` if no identity is loaded.
///
/// # Safety
/// `ctx` live; `out` 16 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_destination_hash(
    ctx: *const RsHandheldRns,
    out: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        // SAFETY: per the contract.
        match (unsafe { ctx.as_ref() }, unsafe { out.as_mut() }) {
            (Some(ctx), Some(out)) => match &ctx.identity {
                Some(id) => {
                    *out = id.lxmf_delivery_hash();
                    RsHandheldStatus::Ok
                }
                None => RsHandheldStatus::ErrNotReady,
            },
            _ => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Export the active identity's raw 64-byte private key. `ERR_NOT_READY` if none loaded.
///
/// # Safety
/// `ctx` live; `out` 64 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_export_identity(
    ctx: *const RsHandheldRns,
    out: *mut [u8; PRIVATE_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        // SAFETY: per the contract.
        match (unsafe { ctx.as_ref() }, unsafe { out.as_mut() }) {
            (Some(ctx), Some(out)) => match &ctx.identity {
                Some(id) => {
                    *out = *id.private_key();
                    RsHandheldStatus::Ok
                }
                None => RsHandheldStatus::ErrNotReady,
            },
            _ => RsHandheldStatus::ErrInvalidArg,
        }
    })
}
