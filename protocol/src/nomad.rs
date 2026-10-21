//! NomadNet node destination derivation and page-request building.
//!
//! NomadNet is a sibling Reticulum application to LXMF — a BBS-style page server
//! identified by the `nomadnetwork.node` aspect, not an LXMF concept at all. Unlike
//! learning a peer's node from a live `nomadnetwork.node` announce, the destination hash
//! needed to reach it is derivable directly from a peer's ALREADY-known public key (e.g.
//! from their `lxmf.delivery` announce/contact): a Reticulum destination hash is a pure
//! function of (aspect name, identity public key) — no private key, no live announce of
//! that specific aspect required.
//!
//! The page-request wire format here matches the real `RNS.Link.request()` / rns-link's
//! `prepare_request` (msgpack `[timestamp, path_hash, data]` under context REQUEST), which
//! is NOT the same framing as LXMF-propagation's own `/get` request in `propagation.rs` —
//! a different sibling application with its own bespoke operation/transient-id shape.

use super::*;
use rns_lite_core::identity::{destination_hash_from_name, identity_hash as rns_identity_hash};
use rns_lite_core::wire::truncated_hash;

const NOMAD_NODE_ASPECT: &str = "nomadnetwork.node";

/// Derive the `nomadnetwork.node` destination hash reachable from a peer's known public
/// key. Pure hash computation — no private key, no live announce of this aspect needed.
///
/// # Safety
/// `public_key` reads 64 bytes; `out` writes 16 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_nomad_node_hash(
    public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if public_key.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: non-null and correctly sized per the contract.
        let id_hash = rns_identity_hash(unsafe { &*public_key });
        unsafe { *out = destination_hash_from_name(NOMAD_NODE_ASPECT, Some(&id_hash)) };
        RsHandheldStatus::Ok
    })
}

/// Bounded page/path length this firmware will ever request (NomadNet paths are short,
/// e.g. `/page/index.mu`); sizes the caller's stack buffer.
pub const RS_HANDHELD_NOMAD_PATH_MAX: usize = 128;
// The C header hardcodes this; a silent drift would undersize the caller's stack buffer.
const _: () = assert!(RS_HANDHELD_NOMAD_PATH_MAX == 128);

/// Fixed header size (array marker + float64 timestamp + bin8-16 path hash) before the
/// trailing `data` element.
const NOMAD_REQUEST_HEADER_LEN: usize = 28;

/// Build a Reticulum Link `request()` plaintext: msgpack `[timestamp, truncated_hash(path),
/// data]` — the real wire format a NomadNet node expects under context `REQUEST` (0x09),
/// matching `RNS.Link.request(path, data)`. `data` is caller-supplied, ALREADY
/// msgpack-encoded bytes (e.g. an encoded field-value map for a form submission); pass
/// `data_len == 0` (and `data` may be null) for no form data, which encodes msgpack Nil.
///
/// Returns `ErrCapacity` if `out` is too small, `ErrInvalidArg` if `path_len` is zero or
/// exceeds `RS_HANDHELD_NOMAD_PATH_MAX`, `timestamp` is not finite/non-negative, or a
/// pointer/length pairing is malformed.
///
/// # Safety
/// `path` reads `path_len` bytes; `data` reads `data_len` bytes (or is null iff
/// `data_len == 0`); `out` writes `out_cap` bytes; `out_len` is a writable `usize`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_nomad_request_build(
    timestamp: f64,
    path: *const u8,
    path_len: usize,
    data: *const u8,
    data_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if path.is_null()
            || out.is_null()
            || out_len.is_null()
            || (data.is_null() && data_len != 0)
            || path_len == 0
            || path_len > RS_HANDHELD_NOMAD_PATH_MAX
            || !timestamp.is_finite()
            || timestamp < 0.0
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let total = NOMAD_REQUEST_HEADER_LEN + data_len.max(1);
        if out_cap < total {
            return RsHandheldStatus::ErrCapacity;
        }
        // SAFETY: non-null, correctly sized per the contract.
        let path_bytes = unsafe { core::slice::from_raw_parts(path, path_len) };
        let path_hash = truncated_hash(path_bytes);
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        out_slice[0] = 0x93; // fixarray, 3 elements
        out_slice[1] = 0xcb; // float64
        out_slice[2..10].copy_from_slice(&timestamp.to_be_bytes());
        out_slice[10..12].copy_from_slice(&[0xc4, 16]); // bin8, length 16
        out_slice[12..28].copy_from_slice(&path_hash);
        if data_len == 0 {
            out_slice[28] = 0xc0; // nil
        } else {
            // SAFETY: non-null, correctly sized per the contract.
            let data_bytes = unsafe { core::slice::from_raw_parts(data, data_len) };
            out_slice[28..28 + data_len].copy_from_slice(data_bytes);
        }
        unsafe { *out_len = total };
        RsHandheldStatus::Ok
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};

    fn manual_nomad_hash(public_key: &[u8; PUBLIC_KEY_LENGTH]) -> [u8; DESTINATION_LENGTH] {
        let identity_hash: [u8; 16] = Sha256::digest(public_key)[..16].try_into().unwrap();
        let name_hash: [u8; 10] = Sha256::digest(NOMAD_NODE_ASPECT.as_bytes())[..10]
            .try_into()
            .unwrap();
        let mut material = [0u8; 10 + 16];
        material[..10].copy_from_slice(&name_hash);
        material[10..].copy_from_slice(&identity_hash);
        Sha256::digest(material)[..16].try_into().unwrap()
    }

    #[test]
    fn nomad_node_hash_matches_manual_computation() {
        let pubkey = [7u8; PUBLIC_KEY_LENGTH];
        let mut out = [0u8; DESTINATION_LENGTH];
        assert_eq!(
            unsafe { rs_handheld_rns_nomad_node_hash(&pubkey, &mut out) },
            RsHandheldStatus::Ok
        );
        assert_eq!(out, manual_nomad_hash(&pubkey));
    }

    #[test]
    fn nomad_node_hash_differs_for_different_keys_and_rejects_null() {
        let mut a = [0u8; DESTINATION_LENGTH];
        let mut b = [0u8; DESTINATION_LENGTH];
        let key_a = [1u8; PUBLIC_KEY_LENGTH];
        let key_b = [2u8; PUBLIC_KEY_LENGTH];
        assert_eq!(
            unsafe { rs_handheld_rns_nomad_node_hash(&key_a, &mut a) },
            RsHandheldStatus::Ok
        );
        assert_eq!(
            unsafe { rs_handheld_rns_nomad_node_hash(&key_b, &mut b) },
            RsHandheldStatus::Ok
        );
        assert_ne!(a, b);
        assert_eq!(
            unsafe { rs_handheld_rns_nomad_node_hash(core::ptr::null(), &mut a) },
            RsHandheldStatus::ErrInvalidArg
        );
    }

    #[test]
    fn nomad_request_build_rejects_invalid_args() {
        let mut out = [0u8; 256];
        let mut len = 0usize;
        let path = b"/page/index.mu";
        // Zero-length path.
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    1.0,
                    path.as_ptr(),
                    0,
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::ErrInvalidArg
        );
        // Path too long.
        let long_path = [b'a'; RS_HANDHELD_NOMAD_PATH_MAX + 1];
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    1.0,
                    long_path.as_ptr(),
                    long_path.len(),
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::ErrInvalidArg
        );
        // Non-finite / negative timestamp.
        for bad in [f64::NAN, f64::INFINITY, -1.0] {
            assert_eq!(
                unsafe {
                    rs_handheld_rns_nomad_request_build(
                        bad,
                        path.as_ptr(),
                        path.len(),
                        core::ptr::null(),
                        0,
                        out.as_mut_ptr(),
                        out.len(),
                        &mut len,
                    )
                },
                RsHandheldStatus::ErrInvalidArg
            );
        }
        // data null but data_len nonzero.
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    1.0,
                    path.as_ptr(),
                    path.len(),
                    core::ptr::null(),
                    4,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::ErrInvalidArg
        );
    }

    #[test]
    fn nomad_request_build_reports_capacity_and_preserves_frame() {
        let path = b"/page/index.mu";
        let mut out = [0u8; 20]; // smaller than the 28-byte fixed header alone
        let mut len = 0usize;
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    1.0,
                    path.as_ptr(),
                    path.len(),
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::ErrCapacity
        );
    }

    #[test]
    fn nomad_request_build_byte_layout_nil_data() {
        let path = b"/page/index.mu";
        let mut out = [0u8; 64];
        let mut len = 0usize;
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    1.5,
                    path.as_ptr(),
                    path.len(),
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::Ok
        );
        assert_eq!(len, 29);
        assert_eq!(out[0], 0x93);
        assert_eq!(out[1], 0xcb);
        assert_eq!(&out[2..10], &1.5f64.to_be_bytes());
        assert_eq!(&out[10..12], &[0xc4, 16]);
        let expected_hash: [u8; 16] = Sha256::digest(path.as_slice())[..16].try_into().unwrap();
        assert_eq!(&out[12..28], &expected_hash);
        assert_eq!(out[28], 0xc0);
    }

    #[test]
    fn nomad_request_build_byte_layout_with_data() {
        let path = b"/page/index.mu";
        let data = [
            0x81u8, 0xa4, b'n', b'a', b'm', b'e', 0xa5, b'A', b'l', b'i', b'c', b'e',
        ]; // {"name": "Alice"}
        let mut out = [0u8; 64];
        let mut len = 0usize;
        assert_eq!(
            unsafe {
                rs_handheld_rns_nomad_request_build(
                    2.0,
                    path.as_ptr(),
                    path.len(),
                    data.as_ptr(),
                    data.len(),
                    out.as_mut_ptr(),
                    out.len(),
                    &mut len,
                )
            },
            RsHandheldStatus::Ok
        );
        assert_eq!(len, 28 + data.len());
        assert_eq!(&out[28..28 + data.len()], &data[..]);
    }
}
