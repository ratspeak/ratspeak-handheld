//! Device admission for the standard identity-derived LXST telephone endpoint.
use super::*;
use rns_lite_core::identity::destination_hash_from_name;

pub(super) const VOICE_DESTINATION_NAME: &str = "lxst.telephony";

/// Derive the standard telephone destination from a public identity.
/// # Safety
/// Both pointers are non-null, valid for their array sizes and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_destination(
    public_key: *const [u8; 64],
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if public_key.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let identity = rns_lite_core::identity::identity_hash(unsafe { &*public_key });
        unsafe { *out = destination_hash_from_name(VOICE_DESTINATION_NAME, Some(&identity)) };
        RsHandheldStatus::Ok
    })
}

/// Register/unregister telephone admission without modifying delivery ownership.
/// The caller must stop its voice Links before disabling. Defaults to disabled;
/// identity replacement also disables it, requiring explicit reconfiguration.
/// # Safety
/// `ctx` is live and exclusively owned; `enabled` is zero or one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_enable(
    ctx: *mut RsHandheldRns,
    enabled: u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || enabled > 1 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let ctx = unsafe { &mut *ctx };
        let Some(id) = &ctx.identity else {
            return RsHandheldStatus::ErrNotReady;
        };
        if let Some(mut node) = ctx.node {
            let node = unsafe { node.as_mut() };
            node.clear_own_destinations();
            let _ = node.register_own_destination(id.lxmf_delivery_hash());
            if enabled != 0
                && !node.register_own_destination(id.destination_hash(VOICE_DESTINATION_NAME))
            {
                ctx.voice_enabled = false;
                return RsHandheldStatus::ErrCapacity;
            }
        }
        ctx.voice_enabled = enabled != 0;
        ctx.own_path_request = None;
        RsHandheldStatus::Ok
    })
}

/// Build a standard telephone announce packet (empty app data, no ratchet), as
/// in the trusted full Rust TelephonyEndpoint. `response` selects PATH_RESPONSE.
/// # Safety
/// Context and inputs are valid, output has `capacity` bytes, all are disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_announce(
    ctx: *const RsHandheldRns,
    seed: *const [u8; 5],
    order: u64,
    response: u32,
    out: *mut u8,
    capacity: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || seed.is_null() || out.is_null() || out_len.is_null() || response > 1 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let ctx = unsafe { &*ctx };
        let Some(id) = &ctx.identity else {
            return RsHandheldStatus::ErrNotReady;
        };
        if !ctx.voice_enabled {
            return RsHandheldStatus::ErrNotReady;
        }
        let random = compose_random_hash(unsafe { &*seed }, order);
        let mut payload = [0; 148];
        let length = match id.create_announce_named(
            VOICE_DESTINATION_NAME,
            &random,
            None,
            &[],
            &mut payload,
        ) {
            Ok(length) => length,
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let dest = id.destination_hash(VOICE_DESTINATION_NAME);
        unsafe {
            rs_handheld_rns_packet_build(
                0,
                PacketType::Announce as i32,
                DestinationType::Single as i32,
                if response == 1 {
                    PacketContext::PathResponse.to_byte()
                } else {
                    PacketContext::None.to_byte()
                },
                core::ptr::null(),
                &dest,
                payload.as_ptr(),
                length,
                out,
                capacity,
                out_len,
            )
        }
    })
}

/// Consume a self path request, retaining which local endpoint it addresses:
/// endpoint 0 = delivery; 1 = telephone. Invoke immediately after ingest action 10.
/// # Safety
/// All pointers are valid for the declared sizes and mutually disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_take_own_path_request(
    ctx: *mut RsHandheldRns,
    out_tag: *mut [u8; 16],
    out_length: *mut usize,
    out_endpoint: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || out_tag.is_null() || out_length.is_null() || out_endpoint.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let Some(request) = (unsafe { &mut *ctx }).own_path_request.take() else {
            return RsHandheldStatus::ErrNotReady;
        };
        unsafe {
            *out_tag = request.tag;
            *out_length = request.tag_len;
            *out_endpoint = u32::from(request.voice);
        }
        RsHandheldStatus::Ok
    })
}

/// Canonical LXMF destination for the verified telephone peer's public identity.
/// # Safety
/// Pointers are valid for their array sizes, non-null, and mutually disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_peer_destination(
    public_key: *const [u8; 64],
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if public_key.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let identity = rns_lite_core::identity::identity_hash(unsafe { &*public_key });
        unsafe { *out = destination_hash_from_name(LXMF_DELIVERY_NAME, Some(&identity)) };
        RsHandheldStatus::Ok
    })
}
