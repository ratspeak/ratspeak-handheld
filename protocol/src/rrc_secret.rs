//! Identity-bound room secrets adapted from the trusted desktop channels.rs
//! RSCHKEY v1 format. Encryption uses the already qualified Reticulum ECIES.
use super::*;
use rns_lite_core::{crypto, identity};
use zeroize::Zeroizing;

const MAGIC: &[u8; 9] = b"RSCHKEY\0\x01";
const HEADER: usize = 45;
const MAX_KEY: usize = 160;

/// Derive the control source only after checking this key's rrc.hub binding.
/// # Safety
/// All inputs readable at declared sizes; output writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_hub_identity(
    destination: *const [u8; 16],
    public_key: *const [u8; 64],
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if destination.is_null() || public_key.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let hash = identity::identity_hash(unsafe { &*public_key });
        if identity::destination_hash_from_name("rrc.hub", Some(&hash)) != unsafe { *destination } {
            return RsHandheldStatus::ErrCrypto;
        }
        unsafe { *out = hash };
        RsHandheldStatus::Ok
    })
}

/// Canonical LXMF delivery destination for a hub-attested participant identity.
/// This derives an address; it does not authenticate an LXMF recipient key.
/// # Safety
/// identity readable for 16 bytes, out writable for 16 bytes; both nonnull.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_destination_for_identity(
    identity_hash: *const [u8; 16],
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if identity_hash.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let hash =
            identity::destination_hash_from_name("lxmf.delivery", Some(unsafe { &*identity_hash }));
        unsafe {
            *out = hash;
        }
        RsHandheldStatus::Ok
    })
}

/// Canonical RRC hub address for a previously authenticated control source.
/// # Safety
/// identity readable for 16 bytes, out writable for 16 bytes; both nonnull.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_destination_for_identity(
    identity_hash: *const [u8; 16],
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if identity_hash.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let hash =
            identity::destination_hash_from_name("rrc.hub", Some(unsafe { &*identity_hash }));
        unsafe { *out = hash };
        RsHandheldStatus::Ok
    })
}

fn canonical(room: &[u8]) -> bool {
    let mut normalized = [0; 64];
    let mut length = 0;
    (unsafe {
        rs_handheld_rrc_normalize(
            room.as_ptr(),
            room.len(),
            0,
            normalized.as_mut_ptr(),
            normalized.len(),
            &mut length,
        )
    }) == RsHandheldStatus::Ok
        && normalized[..length] == *room
}

/// Seal a bounded room key to the loaded identity with desktop context binding.
/// # Safety
/// Context live; pointers valid for lengths and disjoint; entropy 48 fresh bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_seal_key(
    ctx: *const RsHandheldRns,
    hub: *const [u8; 16],
    room: *const u8,
    room_length: usize,
    key: *const u8,
    key_length: usize,
    entropy: *const [u8; 48],
    out: *mut u8,
    capacity: usize,
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if out_length.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        unsafe { *out_length = 0 };
        if ctx.is_null()
            || hub.is_null()
            || room.is_null()
            || key.is_null()
            || entropy.is_null()
            || out.is_null()
            || room_length == 0
            || room_length > 64
            || key_length == 0
            || key_length > MAX_KEY
            || capacity > 431
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let Some(id) = &(unsafe { &*ctx }).identity else {
            return RsHandheldStatus::ErrNotReady;
        };
        let room = unsafe { core::slice::from_raw_parts(room, room_length) };
        let key = unsafe { core::slice::from_raw_parts(key, key_length) };
        if !canonical(room) || core::str::from_utf8(key).is_err() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mut plain = Zeroizing::new([0; HEADER + 64 + MAX_KEY]);
        plain[..9].copy_from_slice(MAGIC);
        plain[9..25].copy_from_slice(id.identity_hash());
        plain[25..41].copy_from_slice(unsafe { &*hub });
        plain[41..43].copy_from_slice(&(room_length as u16).to_be_bytes());
        plain[43..45].copy_from_slice(&(key_length as u16).to_be_bytes());
        plain[HEADER..HEADER + room_length].copy_from_slice(room);
        let length = HEADER + room_length + key_length;
        plain[HEADER + room_length..length].copy_from_slice(key);
        let entropy = unsafe { &*entropy };
        let result = crypto::ecies_encrypt(
            &plain[..length],
            id.public_key()[..32].try_into().unwrap(),
            id.identity_hash(),
            entropy[..32].try_into().unwrap(),
            entropy[32..].try_into().unwrap(),
            unsafe { core::slice::from_raw_parts_mut(out, capacity) },
        );
        match result {
            Ok(length) => {
                unsafe { *out_length = length };
                RsHandheldStatus::Ok
            }
            Err(crypto::CryptoError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrCrypto,
        }
    })
}

/// Authenticate and unseal only for this identity, hub, normalized room and v1.
/// # Safety
/// Context live; all buffers readable/writable for declared lengths and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_unseal_key(
    ctx: *const RsHandheldRns,
    hub: *const [u8; 16],
    room: *const u8,
    room_length: usize,
    sealed: *const u8,
    sealed_length: usize,
    out: *mut u8,
    capacity: usize,
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if out_length.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        unsafe { *out_length = 0 };
        if ctx.is_null()
            || hub.is_null()
            || room.is_null()
            || sealed.is_null()
            || out.is_null()
            || room_length == 0
            || room_length > 64
            || sealed_length > 431
            || capacity > 431
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let Some(id) = &(unsafe { &*ctx }).identity else {
            return RsHandheldStatus::ErrNotReady;
        };
        let room = unsafe { core::slice::from_raw_parts(room, room_length) };
        if !canonical(room) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mut plain = Zeroizing::new([0; HEADER + 64 + MAX_KEY]);
        let Ok(length) = crypto::ecies_decrypt(
            unsafe { core::slice::from_raw_parts(sealed, sealed_length) },
            id.private_key()[..32].try_into().unwrap(),
            id.identity_hash(),
            plain.as_mut(),
        ) else {
            return RsHandheldStatus::ErrCrypto;
        };
        if length < HEADER
            || plain[..9] != *MAGIC
            || plain[9..25] != *id.identity_hash()
            || plain[25..41] != unsafe { *hub }
        {
            return RsHandheldStatus::ErrCrypto;
        }
        let room_len = u16::from_be_bytes([plain[41], plain[42]]) as usize;
        let key_len = u16::from_be_bytes([plain[43], plain[44]]) as usize;
        if room_len != room_length
            || key_len == 0
            || key_len > MAX_KEY
            || HEADER + room_len + key_len != length
            || plain[HEADER..HEADER + room_len] != *room
        {
            return RsHandheldStatus::ErrCrypto;
        }
        let key = &plain[HEADER + room_len..length];
        if core::str::from_utf8(key).is_err() {
            return RsHandheldStatus::ErrCrypto;
        }
        if capacity < key_len {
            return RsHandheldStatus::ErrCapacity;
        }
        unsafe {
            core::ptr::copy_nonoverlapping(key.as_ptr(), out, key_len);
            *out_length = key_len;
        }
        RsHandheldStatus::Ok
    })
}
