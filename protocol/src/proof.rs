//! Delivery proofs and duplicate suppression.

use super::*;

/// Max proof bytes: explicit = packet_hash(32) + signature(64). Implicit is 64.
pub const RS_HANDHELD_PROOF_MAX: usize = rns_lite_core::proof::PROOF_EXPLICIT_LEN;

/// Build a proof of receipt for a packet whose full 32-byte hash is `packet_hash`, signed by the
/// active identity (the receiver). `implicit != 0` → `signature(64)`; else `packet_hash(32) ||
/// signature(64)`. Writes the proof into `out` (`out_cap >= 64`, or `>= 96` for explicit) and its
/// length to `*out_len`. The sender validates this with `rs_handheld_rns_proof_validate` → DELIVERED.
/// `ErrNotReady` if no identity is loaded; `ErrCapacity` if `out` is too small.
///
/// # Safety
/// `ctx` live with a loaded identity; `packet_hash` 32 readable bytes; `out` `out_cap` writable;
/// `out_len` a writable `usize`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_proof_build(
    ctx: *const RsHandheldRns,
    packet_hash: *const [u8; 32],
    implicit: i32,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || packet_hash.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match rns_proof::build_proof(id, unsafe { &*packet_hash }, implicit != 0, out_slice) {
            Ok(n) => {
                // SAFETY: out_len non-null + writable per the contract.
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(ProofError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
        }
    })
}

/// Validate a proof of receipt for `packet_hash`, signed by the identity whose 64-byte public key is
/// `prover_public_key`. Writes 1/0 to `*out_valid`. Stateless (no context). Accepts an implicit (64)
/// or explicit (96, hash-prefixed) proof. `ErrInvalidArg` on null.
///
/// BINDING CONTRACT — the caller is responsible (this only proves "`prover_public_key` signed
/// `packet_hash`"): pass `packet_hash` = the ORIGINAL sent packet's own 32-byte hash and
/// `prover_public_key` = THAT message's recipient identity's public key. For an IMPLICIT proof
/// (no embedded hash) iterate each outstanding sent packet's `(hash, recipient_key)` and accept
/// DELIVERED only on the first that validates; for an EXPLICIT proof match its hash prefix to the
/// pending receipt first. Never mark an arbitrary pending message DELIVERED on a bare valid proof.
///
/// # Safety
/// `prover_public_key` 64 readable bytes; `packet_hash` 32; `proof` `proof_len` readable bytes;
/// `out_valid` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_proof_validate(
    prover_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    packet_hash: *const [u8; 32],
    proof: *const u8,
    proof_len: usize,
    out_valid: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if prover_public_key.is_null()
            || packet_hash.is_null()
            || proof.is_null()
            || out_valid.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let proof = unsafe { core::slice::from_raw_parts(proof, proof_len) };
        let valid = rns_proof::validate_proof(
            unsafe { &*prover_public_key },
            unsafe { &*packet_hash },
            proof,
        );
        unsafe { *out_valid = valid as i32 };
        RsHandheldStatus::Ok
    })
}

/// Record an inbound message id for duplicate suppression. Writes 1 to `*out_is_new` if `message_id`
/// is NEW (the caller should process + store it) or 0 if already seen (drop it, do not double-store).
/// Bounded FIFO set (rsDeck `MAX_SEEN_IDS = 100`). `ErrInvalidArg` on null.
///
/// IN-MEMORY ONLY: the set is empty after `rs_handheld_rns_init` and does NOT persist. For the cross-reboot
/// dedup contract (a retry of an already-stored message must not double-store), the caller MUST seed
/// it at startup from persistent storage via `rs_handheld_rns_seed_seen_message` (replaying the recently-
/// stored ids), mirroring the C++ `loadRecentMessageIds(MAX_SEEN_IDS)` boot step.
///
/// # Safety
/// `ctx` live; `message_id` 32 readable bytes; `out_is_new` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_seen_message(
    ctx: *mut RsHandheldRns,
    message_id: *const [u8; 32],
    out_is_new: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || message_id.is_null() || out_is_new.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract.
        let is_new = unsafe { &mut *ctx }
            .seen
            .insert_if_new(unsafe { &*message_id });
        unsafe { *out_is_new = is_new as i32 };
        RsHandheldStatus::Ok
    })
}

/// Check duplicate membership without recording the id. Call before persistence,
/// then seed only after a successful durable write. A rejected write remains retryable.
///
/// # Safety
/// `ctx` live; `message_id` 32 readable bytes; `out_seen` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_has_seen_message(
    ctx: *const RsHandheldRns,
    message_id: *const [u8; 32],
    out_seen: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || message_id.is_null() || out_seen.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null pointers with the readable/writable sizes above.
        unsafe { *out_seen = (*ctx).seen.contains(&*message_id) as i32 };
        RsHandheldStatus::Ok
    })
}

/// Seed the seen-message set with an already-known/stored id at startup (no `is_new` output) — call
/// once per id from persistent storage after `rs_handheld_rns_init`, replaying `loadRecentMessageIds(
/// MAX_SEEN_IDS)`, so a post-reboot retry of a stored message is recognised as a duplicate. Without
/// this seed the set is volatile and the cross-reboot dedup contract does not hold. `ErrInvalidArg`
/// on null.
///
/// # Safety
/// `ctx` live; `message_id` 32 readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_seed_seen_message(
    ctx: *mut RsHandheldRns,
    message_id: *const [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || message_id.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null per the contract.
        let _ = unsafe { &mut *ctx }
            .seen
            .insert_if_new(unsafe { &*message_id });
        RsHandheldStatus::Ok
    })
}
