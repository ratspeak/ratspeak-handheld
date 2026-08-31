//! Bounded Resource transfer over an established link.

use super::*;
//
// Byte-exact with Python RNS 1.3.8 `RNS.Resource` (pinned vectors + live interop) via the lite
// resource layer (adapted from the trusted rns-protocol). SCOPE SPLIT (mirrors proof/link): the
// Rust side provides the resource MECHANISM — advertisement codec, blob encryption + part
// chunking/hashing, part request/serve matching, bounded reassembly, delivery proof. The C++
// owner drives the transfer STATE MACHINE: when to advertise, request, retransmit, grow/shrink
// the window (it has the clock), time out, and clean up — and it holds the link session key
// (from `rs_handheld_rns_link_derive`) that both `resource_advertise_build` and
// `resource_assemble` take per call (Rust holds no key material between calls; the C++ zeroizes
// the key when the link closes).
//
// One outbound + one inbound transfer per context (the lite staged single-resource scope; a new
// build/accept replaces the previous transfer). Parts ride RESOURCE-context packets on the link
// UNENCRYPTED at the packet layer — the blob is already one Token ciphertext; the ADV, part
// requests and proof travel link-encrypted (`rs_handheld_rns_link_encrypt`) with contexts
// RESOURCE_ADV / RESOURCE_REQ / RESOURCE_PRF. Packet framing stays C++-owned (like announce).
//
// Honest-subset bounds (fail-closed, from the lite layer): <= RS_HANDHELD_RESOURCE_MAX_PARTS
// parts / RS_HANDHELD_RESOURCE_DATA_MAX payload; UNCOMPRESSED only (no bz2 on the fleet);
// single segment; plain transfers (no request/response resources); encrypted-only. Oversized or
// unsupported advertisements are refused at accept time, like any declined transfer.

/// Link plaintext MDU (the budget for link-encrypted payloads such as the packed ADV).
pub const RS_HANDHELD_LINK_MDU: usize = LINK_MDU;
/// Resource part payload size (raw ciphertext chunk; parts use the RAW packet budget).
pub const RS_HANDHELD_RESOURCE_SDU: usize = rns_resource::SDU;
/// Max parts per resource (lite MCU bound; the full hashmap always fits one ADV).
pub const RS_HANDHELD_RESOURCE_MAX_PARTS: usize = rns_resource::MAX_PARTS;
/// Max encrypted-blob (transfer) size = MAX_PARTS * SDU.
pub const RS_HANDHELD_RESOURCE_TRANSFER_MAX: usize = rns_resource::TRANSFER_MAX;
/// Max plaintext payload per resource.
pub const RS_HANDHELD_RESOURCE_DATA_MAX: usize = rns_resource::DATA_MAX;
/// Worst-case packed advertisement size.
pub const RS_HANDHELD_RESOURCE_ADV_MAX: usize = rns_resource::ADV_PACKED_MAX;
/// Worst-case part-request size at the lite bounds.
pub const RS_HANDHELD_RESOURCE_REQUEST_MAX: usize = rns_resource::REQUEST_MAX;
/// Delivery proof length: resource_hash(32) || proof(32).
pub const RS_HANDHELD_RESOURCE_PROOF_LEN: usize = rns_resource::PROOF_LEN;
/// Caller-entropy tag mixed into every resource hash (ADV `r` field).
pub const RS_HANDHELD_RESOURCE_RANDOM_HASH_LEN: usize = rns_resource::RANDOM_HASH_SIZE;

// ABI pins: these MUST equal the C header `#define`s.
const _: () = {
    assert!(RS_HANDHELD_LINK_MDU == 431);
    assert!(RS_HANDHELD_RESOURCE_SDU == 464);
    assert!(RS_HANDHELD_RESOURCE_MAX_PARTS == 8);
    assert!(RS_HANDHELD_RESOURCE_TRANSFER_MAX == 3712);
    assert!(RS_HANDHELD_RESOURCE_DATA_MAX == 3659);
    assert!(RS_HANDHELD_RESOURCE_ADV_MAX == 192);
    assert!(RS_HANDHELD_RESOURCE_REQUEST_MAX == 65);
    assert!(RS_HANDHELD_RESOURCE_PROOF_LEN == 64);
    assert!(RS_HANDHELD_RESOURCE_RANDOM_HASH_LEN == 4);
};

/// Map a lite resource error onto the C status set. `MapHashCollision` -> `ErrRetry` (caller
/// retries with fresh `random_hash`, mirroring Python's internal re-roll loop).
pub(super) fn resource_status(e: ResourceError) -> RsHandheldStatus {
    match e {
        ResourceError::TooLarge | ResourceError::OutputTooSmall => RsHandheldStatus::ErrCapacity,
        ResourceError::MapHashCollision => RsHandheldStatus::ErrRetry,
        ResourceError::CompressedUnsupported
        | ResourceError::SplitUnsupported
        | ResourceError::MetadataUnsupported
        | ResourceError::RequestResponseUnsupported
        | ResourceError::EncryptionRequired => RsHandheldStatus::ErrUnsupported,
        ResourceError::InvalidAdvertisement | ResourceError::InvalidRequest => {
            RsHandheldStatus::ErrInvalidArg
        }
        ResourceError::Incomplete | ResourceError::InvalidState => RsHandheldStatus::ErrNotReady,
        ResourceError::HashMismatch | ResourceError::Corrupt | ResourceError::Crypto(_) => {
            RsHandheldStatus::ErrCrypto
        }
    }
}

/// SENDER: open an outbound resource for `data` and emit its packed advertisement. Token-encrypts
/// `random_hash || data` with the 64-byte link session `key`, chunks the ciphertext into SDU
/// parts, derives the hashmap, and writes the msgpack ADV (send it link-encrypted with packet
/// context RESOURCE_ADV) to `out_adv`/`*out_adv_len`; `*out_num_parts` receives the part count and
/// `out_resource_hash` (optional, 32) the advertised resource hash. Replaces any previous outbound
/// transfer on this context. `random_hash` (4) + `iv` (16) are caller entropy, fresh per resource
/// (fixed only for vectors). `ErrCapacity` if `data_len > RS_HANDHELD_RESOURCE_DATA_MAX` or
/// `out_adv` is too small; `ErrRetry` on a map-hash collision (retry with fresh `random_hash`);
/// `ErrCrypto` on encryption failure.
///
/// # Safety
/// `ctx` live; `key` 64 readable bytes; `data` `data_len` readable (or null iff 0); `random_hash`
/// 4 readable; `iv` 16 readable; `out_adv` `out_adv_cap` writable; `out_adv_len`/`out_num_parts`
/// writable; `out_resource_hash` null or 32 writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_resource_advertise_build(
    ctx: *mut RsHandheldRns,
    key: *const [u8; LINK_KEY_LENGTH],
    data: *const u8,
    data_len: usize,
    random_hash: *const [u8; RS_HANDHELD_RESOURCE_RANDOM_HASH_LEN],
    iv: *const [u8; 16],
    out_adv: *mut u8,
    out_adv_cap: usize,
    out_adv_len: *mut usize,
    out_num_parts: *mut u32,
    out_resource_hash: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || key.is_null()
            || random_hash.is_null()
            || iv.is_null()
            || out_adv.is_null()
            || out_adv_len.is_null()
            || out_num_parts.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if data.is_null() && data_len != 0 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let data = if data_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(data, data_len) }
        };
        let keys = LinkKeys::from_combined(unsafe { &*key });
        // Allocate the resource (with its ~3.7 KiB buffer) directly on the heap and build
        // in place. A by-value `OutboundResource::build` would put ~7.7 KiB on the caller's
        // stack (the loopTask), overflowing it. alloc_zeroed is sound here: every
        // field of OutboundResource is a zeroable POD, so all-zero is a valid value.
        let mut out = match box_zeroed::<OutboundResource>() {
            Some(b) => b,
            None => return RsHandheldStatus::ErrInternal,
        };
        if let Err(e) = out.build_into(data, &keys, unsafe { &*random_hash }, unsafe { &*iv }) {
            return resource_status(e);
        }
        let adv_slice = unsafe { core::slice::from_raw_parts_mut(out_adv, out_adv_cap) };
        let n = match out.advertisement().pack(adv_slice) {
            Ok(n) => n,
            Err(e) => return resource_status(e),
        };
        // SAFETY: out pointers non-null + writable per the contract.
        unsafe {
            *out_adv_len = n;
            *out_num_parts = out.num_parts() as u32;
            if !out_resource_hash.is_null() {
                *out_resource_hash = *out.resource_hash();
            }
        }
        // SAFETY: `ctx` valid per the contract.
        unsafe { &mut *ctx }.resource_out = Some(out);
        RsHandheldStatus::Ok
    })
}

/// SENDER: copy part `index` (raw ciphertext chunk) of the open outbound resource into `out`.
/// Send it on the link as a RESOURCE-context packet WITHOUT further encryption (the blob is
/// already one Token ciphertext). `ErrNotReady` if no outbound transfer is open; `ErrInvalidArg`
/// if `index >= num_parts`; `ErrCapacity` if `out` is too small (parts are at most
/// `RS_HANDHELD_RESOURCE_SDU` bytes).
///
/// # Safety
/// `ctx` live; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_part_emit(
    ctx: *const RsHandheldRns,
    index: u32,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let res = match &unsafe { &*ctx }.resource_out {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let part = match res.part(index as usize) {
            Some(p) => p,
            None => return RsHandheldStatus::ErrInvalidArg,
        };
        if part.len() > out_cap {
            return RsHandheldStatus::ErrCapacity;
        }
        // SAFETY: non-null + sufficient capacity checked above.
        let dst = unsafe { core::slice::from_raw_parts_mut(out, part.len()) };
        dst.copy_from_slice(part);
        unsafe { *out_len = part.len() };
        RsHandheldStatus::Ok
    })
}

/// SENDER: parse an inbound RESOURCE_REQ payload (link-decrypted by the caller) and resolve the
/// part indices to (re)send, in wire order. `out_indices` receives up to
/// `RS_HANDHELD_RESOURCE_MAX_PARTS` u32 indices, `*out_count` how many are valid; emit each via
/// `rs_handheld_rns_resource_part_emit`. A request addressed to a different resource hash yields
/// `*out_count = 0` with `RS_HANDHELD_OK` (Python routes requests by hash and ignores
/// non-matching ones). `ErrNotReady` if no outbound transfer is open; `ErrInvalidArg` on a
/// malformed request.
///
/// # Safety
/// `ctx` live; `req` `req_len` readable bytes; `out_indices` RS_HANDHELD_RESOURCE_MAX_PARTS
/// writable u32s; `out_count` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_request_serve(
    ctx: *const RsHandheldRns,
    req: *const u8,
    req_len: usize,
    out_indices: *mut u32,
    out_count: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || req.is_null() || out_indices.is_null() || out_count.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let res = match &unsafe { &*ctx }.resource_out {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let req = unsafe { core::slice::from_raw_parts(req, req_len) };
        let view = match PartRequestView::parse(req) {
            Ok(v) => v,
            Err(e) => return resource_status(e),
        };
        let (indices, count) = res.requested_parts(&view);
        let dst =
            unsafe { core::slice::from_raw_parts_mut(out_indices, RS_HANDHELD_RESOURCE_MAX_PARTS) };
        for (d, &i) in dst.iter_mut().zip(indices.iter()).take(count) {
            *d = i as u32;
        }
        unsafe { *out_count = count as u32 };
        RsHandheldStatus::Ok
    })
}

/// SENDER: check a delivery proof (link-decrypted RESOURCE_PRF payload) against the open outbound
/// resource. Writes 1/0 to `*out_valid`; 1 means the receiver reassembled the exact payload ->
/// the C++ owner marks the transfer DELIVERED and closes it. A wrong-length or non-matching proof
/// is `*out_valid = 0` with `RS_HANDHELD_OK`. `ErrNotReady` if no outbound transfer is open.
///
/// # Safety
/// `ctx` live; `proof` `proof_len` readable bytes; `out_valid` a writable i32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_proof_validate(
    ctx: *const RsHandheldRns,
    proof: *const u8,
    proof_len: usize,
    out_valid: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || proof.is_null() || out_valid.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let res = match &unsafe { &*ctx }.resource_out {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let proof = unsafe { core::slice::from_raw_parts(proof, proof_len) };
        unsafe { *out_valid = res.validate_proof(proof) as i32 };
        RsHandheldStatus::Ok
    })
}

/// SENDER: close the open outbound transfer (delivered, failed or timed out — the C++ owner
/// decides when). Frees the ~3.7 KiB buffer. No-op if none is open.
///
/// # Safety
/// `ctx` live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_outbound_close(
    ctx: *mut RsHandheldRns,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        unsafe { &mut *ctx }.resource_out = None;
        RsHandheldStatus::Ok
    })
}

/// Read the segment hash of a syntactically valid, link-decrypted RESOURCE_ADV, including an
/// advertisement whose hashmap exceeds the endpoint capacity. Does not allocate or change a
/// transfer. The host uses this hash in an encrypted RESOURCE_RCL when declining the resource.
///
/// # Safety
/// `adv` points to `adv_len` readable bytes; `out_resource_hash` is 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_advertisement_hash(
    adv: *const u8,
    adv_len: usize,
    out_resource_hash: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if adv.is_null() || out_resource_hash.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: readable/writable bounds are part of the caller contract.
        let bytes = unsafe { core::slice::from_raw_parts(adv, adv_len) };
        match ResourceAdv::rejection_hash(bytes) {
            Ok(hash) => {
                // SAFETY: non-null and writable per the contract.
                unsafe { *out_resource_hash = hash };
                RsHandheldStatus::Ok
            }
            Err(e) => resource_status(e),
        }
    })
}

/// RECEIVER: parse an advertisement payload (link-decrypted RESOURCE_ADV) and accept the
/// transfer, replacing any previous inbound transfer on this context. Enforces the lite
/// honest-subset rules fail-closed: `ErrUnsupported` for compressed / multi-segment / metadata /
/// request-response / unencrypted resources, `ErrCapacity` for resources beyond the lite bounds
/// (the host must notify the peer with RESOURCE_RCL), `ErrInvalidArg` for a malformed or internally inconsistent
/// ADV. On `RS_HANDHELD_OK`, `*out_num_parts`/`*out_transfer_size`/`*out_data_size` receive the
/// advertised geometry and `out_resource_hash` (optional, 32) the resource hash; the C++ owner
/// then drives request/ingest rounds.
///
/// # Safety
/// `ctx` live; `adv` `adv_len` readable bytes; `out_num_parts`/`out_transfer_size`/
/// `out_data_size` writable u32s; `out_resource_hash` null or 32 writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_advertise_accept(
    ctx: *mut RsHandheldRns,
    adv: *const u8,
    adv_len: usize,
    out_num_parts: *mut u32,
    out_transfer_size: *mut u32,
    out_data_size: *mut u32,
    out_resource_hash: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || adv.is_null()
            || out_num_parts.is_null()
            || out_transfer_size.is_null()
            || out_data_size.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null + correctly sized per the contract.
        let adv = unsafe { core::slice::from_raw_parts(adv, adv_len) };
        let parsed = match ResourceAdv::parse(adv) {
            Ok(a) => a,
            Err(e) => return resource_status(e),
        };
        // Heap-allocate (zeroed) then accept in place — a by-value from_advertisement would
        // put the ~3.7 KiB buffer on the stack. alloc_zeroed is sound: all
        // fields are zeroable (InboundState discriminant 0 == Transferring; and
        // from_advertisement_into overwrites state/window regardless).
        let mut inbound = match box_zeroed::<InboundResource>() {
            Some(b) => b,
            None => return RsHandheldStatus::ErrInternal,
        };
        if let Err(e) = inbound.from_advertisement_into(&parsed) {
            return resource_status(e);
        }
        // SAFETY: out pointers non-null + writable per the contract.
        unsafe {
            *out_num_parts = inbound.num_parts() as u32;
            *out_transfer_size = parsed.transfer_size;
            *out_data_size = parsed.data_size;
            if !out_resource_hash.is_null() {
                *out_resource_hash = *inbound.resource_hash();
            }
        }
        // SAFETY: `ctx` valid per the contract.
        unsafe { &mut *ctx }.resource_in = Some(inbound);
        RsHandheldStatus::Ok
    })
}

/// RECEIVER: ingest one received part (raw RESOURCE-context packet payload). Writes 1/0 to
/// `*out_new` (1 = filled a previously-empty slot; duplicates, tampered parts and out-of-window
/// arrivals are 0 — not an error, mirroring the reference's silent drop) and 1/0 to
/// `*out_complete` (all parts present -> call `rs_handheld_rns_resource_assemble`).
/// `ErrNotReady` if no inbound transfer is open.
///
/// # Safety
/// `ctx` live; `part` `part_len` readable bytes; `out_new`/`out_complete` writable i32s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_part_ingest(
    ctx: *mut RsHandheldRns,
    part: *const u8,
    part_len: usize,
    out_new: *mut i32,
    out_complete: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || part.is_null() || out_new.is_null() || out_complete.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let res = match &mut unsafe { &mut *ctx }.resource_in {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let part = unsafe { core::slice::from_raw_parts(part, part_len) };
        let is_new = res.receive_part(part);
        unsafe {
            *out_new = is_new as i32;
            *out_complete = res.is_complete() as i32;
        }
        RsHandheldStatus::Ok
    })
}

/// RECEIVER: emit a RESOURCE_REQ for the missing parts in the current receive window (send it
/// link-encrypted with packet context RESOURCE_REQ). The request covers at most `window` parts;
/// after the batch arrives, call this again until `*out_complete` from part_ingest is 1. The
/// RETRY/TIMEOUT cadence is C++-owned. `ErrNotReady` if no inbound transfer is open, or if the
/// transfer is already complete/assembled/corrupt (nothing left to request); `ErrCapacity` if
/// `out` is too small (size it `RS_HANDHELD_RESOURCE_REQUEST_MAX`).
///
/// # Safety
/// `ctx` live; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_request_build(
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
        let res = match &unsafe { &*ctx }.resource_in {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match res.build_part_request(out_slice) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(e) => resource_status(e),
        }
    })
}

/// RECEIVER: grow the receive/request window after a completed batch. `rate_bytes_per_sec` is the
/// observed transfer rate, or 0 when unknown (neutral: tier counters untouched). The C++ owner
/// calls this when a requested batch completes (it has the clock); the window bounds the next
/// `resource_request_build`. `ErrNotReady` if no inbound transfer is open.
///
/// # Safety
/// `ctx` live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_window_grow(
    ctx: *mut RsHandheldRns,
    rate_bytes_per_sec: u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        match &mut unsafe { &mut *ctx }.resource_in {
            Some(r) => {
                r.window.grow(rate_bytes_per_sec as usize);
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// RECEIVER: shrink the receive/request window after a request timeout (C++-owned timing).
/// `ErrNotReady` if no inbound transfer is open.
///
/// # Safety
/// `ctx` live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_window_shrink(
    ctx: *mut RsHandheldRns,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        match &mut unsafe { &mut *ctx }.resource_in {
            Some(r) => {
                r.window.shrink();
                RsHandheldStatus::Ok
            }
            None => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// RECEIVER: reassemble the completed transfer — Token-decrypt the concatenated parts with the
/// 64-byte link session `key`, verify the resource hash over the plaintext, and copy the payload
/// into `out`/`*out_len`. Then emit the delivery proof via `rs_handheld_rns_resource_proof_build`.
/// `ErrNotReady` if no inbound transfer is open or parts are missing; `ErrCapacity` if `out_cap`
/// is below this transfer's WORST-CASE plaintext — the advertised `data_size` rounded up to the
/// AES block (up to `data_size + 15`): the check runs before the one-shot decrypt, so it must
/// cover everything the Token blob could unpad to; do NOT size `out` to `data_size` exactly,
/// `RS_HANDHELD_RESOURCE_DATA_MAX` always fits — the transfer stays intact, retry with a larger
/// buffer; `ErrCrypto` if decryption or the hash check fails (the transfer is then CORRUPT and
/// must be closed).
///
/// # Safety
/// `ctx` live; `key` 64 readable bytes; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_assemble(
    ctx: *mut RsHandheldRns,
    key: *const [u8; LINK_KEY_LENGTH],
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || key.is_null() || out.is_null() || out_len.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let res = match &mut unsafe { &mut *ctx }.resource_in {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // Capacity check BEFORE the in-place decrypt (assemble is one-shot): bound by the
        // largest plaintext this transfer's Token blob can hold (the advertised data_size's
        // padded block minus the PKCS7 minimum and the embedded random hash), so a too-small
        // `out` never consumes the transfer.
        let padded = (res.data_size() + rns_resource::RANDOM_HASH_SIZE) / 16 * 16 + 16;
        let max_data = padded - 1 - rns_resource::RANDOM_HASH_SIZE;
        if out_cap < max_data {
            return RsHandheldStatus::ErrCapacity;
        }
        let keys = LinkKeys::from_combined(unsafe { &*key });
        let n = match res.assemble(&keys) {
            Ok(n) => n,
            Err(e) => return resource_status(e),
        };
        let data = res.data().expect("assembled resource has data");
        // SAFETY: non-null + capacity checked above (n <= max_data <= out_cap).
        let dst = unsafe { core::slice::from_raw_parts_mut(out, n) };
        dst.copy_from_slice(data);
        unsafe { *out_len = n };
        RsHandheldStatus::Ok
    })
}

/// RECEIVER: write the 64-byte delivery proof (`resource_hash || SHA-256(data || resource_hash)`)
/// for the assembled transfer; send it link-encrypted as a PROOF packet with context RESOURCE_PRF.
/// `ErrNotReady` if no inbound transfer is open or it is not assembled yet; `ErrCapacity` if
/// `out_cap < RS_HANDHELD_RESOURCE_PROOF_LEN`.
///
/// # Safety
/// `ctx` live; `out` `out_cap` writable; `out_len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_proof_build(
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
        let res = match &unsafe { &*ctx }.resource_in {
            Some(r) => r,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + correctly sized per the contract.
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        match res.build_proof(out_slice) {
            Ok(n) => {
                unsafe { *out_len = n };
                RsHandheldStatus::Ok
            }
            Err(e) => resource_status(e),
        }
    })
}

/// RECEIVER: close the open inbound transfer (assembled and stored, failed or timed out — the
/// C++ owner decides when). Frees the ~3.8 KiB buffer. No-op if none is open.
///
/// # Safety
/// `ctx` live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_resource_inbound_close(
    ctx: *mut RsHandheldRns,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        unsafe { &mut *ctx }.resource_in = None;
        RsHandheldStatus::Ok
    })
}
