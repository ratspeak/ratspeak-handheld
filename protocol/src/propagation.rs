//! Checked propagation metadata and client primitives at the C boundary.

use super::*;

/// Build a bounded Link `/get` request. operation 0=list, 1=fetch, 2=purge.
/// Fetch includes no haves; purge must be authorized by durable local storage.
///
/// # Safety
/// `transient_id` reads 32 bytes (null only for list); `out` writes `capacity`
/// bytes; `length` is writable. Regions are disjoint. No output on refusal.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_get_request(
    timestamp: f64,
    operation: u8,
    transient_id: *const [u8; 32],
    limit_bytes: u16,
    out: *mut u8,
    capacity: usize,
    length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if out.is_null()
            || length.is_null()
            || (operation != 0 && transient_id.is_null())
            || operation > 2
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        use lxmf_lite_core::propagation::{GetRequest, build_get_request};
        // SAFETY: checked readable transient ID for the non-list operations.
        let request = match operation {
            0 => GetRequest::List,
            1 => GetRequest::Fetch {
                transient_id: unsafe { &*transient_id },
                limit_bytes,
            },
            _ => GetRequest::Purge {
                durable_transient_id: unsafe { &*transient_id },
            },
        };
        // SAFETY: checked writable caller buffer.
        let output = unsafe { core::slice::from_raw_parts_mut(out, capacity) };
        match build_get_request(timestamp, request, output) {
            Ok(n) => {
                unsafe {
                    *length = n;
                }
                RsHandheldStatus::Ok
            }
            Err(LxmfError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

/// Match the complete RESPONSE envelope and return a borrowed encoded-value span.
///
/// # Safety
/// `data` reads `length` bytes, `request_id` reads 16; outputs write usize values.
/// All regions are non-null and disjoint. Input must remain live/unchanged while
/// its returned span is used. Malformed/oversized input changes no output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_response_view(
    data: *const u8,
    length: usize,
    request_id: *const [u8; 16],
    offset: *mut usize,
    value_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null()
            || request_id.is_null()
            || offset.is_null()
            || value_length.is_null()
            || length > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: caller's checked readable bounded input and request ID.
        let input = unsafe { core::slice::from_raw_parts(data, length) };
        let Ok(value) = lxmf_lite_core::propagation::response_data(input, unsafe { &*request_id })
        else {
            return RsHandheldStatus::ErrInvalidArg;
        };
        // SAFETY: caller's checked writable output values.
        unsafe {
            *offset = value.as_ptr() as usize - data as usize;
            *value_length = value.len();
        }
        RsHandheldStatus::Ok
    })
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RsHandheldPropagationNode {
    pub timebase: u64,
    pub transfer_limit_kb: u64,
    pub sync_limit_kb: u64,
    pub enabled: u8,
    pub stamp_cost: u8,
    pub stamp_flex: u8,
    pub peering_cost: u8,
    /// UTF-8, at most 16 scalar values, with NUL termination; untrusted controls removed.
    pub name: [u8; 64],
}

/// Decode metadata from an already authenticated propagation announce. No
/// caller output is published on failure. Names are bounded UI values only.
///
/// # Safety
/// `data` is readable for `length` bytes and `out` is writable and aligned;
/// neither pointer may be null and their regions must not overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_propagation_node(
    data: *const u8,
    length: usize,
    out: *mut RsHandheldPropagationNode,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || length > RS_HANDHELD_ANNOUNCE_MAX_APP_DATA {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: caller's bounded readable bytes, checked pointers.
        let data = unsafe { core::slice::from_raw_parts(data, length) };
        let Some(node) = lxmf_lite_core::propagation::parse_node_announce(data) else {
            return RsHandheldStatus::ErrInvalidArg;
        };
        let mut result = RsHandheldPropagationNode {
            timebase: node.timebase,
            transfer_limit_kb: node.transfer_limit_kb,
            sync_limit_kb: node.sync_limit_kb,
            enabled: u8::from(node.enabled),
            stamp_cost: node.stamp_cost,
            stamp_flex: node.stamp_flex,
            peering_cost: node.peering_cost,
            name: [0; 64],
        };
        if let Some(name) = node.name.and_then(|n| core::str::from_utf8(n).ok()) {
            let mut offset = 0;
            for ch in name.chars().filter(|ch| {
                !ch.is_control() && !matches!(*ch, '\u{200b}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2060}'..='\u{206f}' | '\u{feff}')
            }).take(16) {
                let n = ch.len_utf8();
                if offset + n >= result.name.len() { break; }
                ch.encode_utf8(&mut result.name[offset..offset + n]);
                offset += n;
            }
        }
        // SAFETY: caller's writable aligned output, published only on success.
        unsafe {
            *out = result;
        }
        RsHandheldStatus::Ok
    })
}

/// Return known recipient cost metadata. `known=0` for absent/malformed data;
/// `known=1,cost=0` for an explicit nil cost. Never infer a free PN from this.
///
/// # Safety
/// Non-null `data` is readable for `length` bytes; `known` and `cost` are writable
/// bytes and all regions are non-overlapping. `data` may be null only for length 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_delivery_cost(
    data: *const u8,
    length: usize,
    known: *mut u8,
    cost: *mut u8,
) -> RsHandheldStatus {
    guard(|| {
        if (data.is_null() && length != 0)
            || known.is_null()
            || cost.is_null()
            || length > RS_HANDHELD_ANNOUNCE_MAX_APP_DATA
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let data = if length == 0 {
            &[]
        } else {
            // SAFETY: caller's readable bytes, checked pointers.
            unsafe { core::slice::from_raw_parts(data, length) }
        };
        let value = lxmf_lite_core::propagation::delivery_stamp_cost(data);
        // SAFETY: caller's writable output bytes.
        unsafe {
            *known = u8::from(value.is_some());
            *cost = value.flatten().unwrap_or(0);
        }
        RsHandheldStatus::Ok
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rpc_ffi_publishes_only_complete_bound_outputs() {
        let mut out = [0xAA; 96];
        let mut n = 999;
        unsafe {
            assert_eq!(
                rs_handheld_lxmf_get_request(
                    1.0,
                    1,
                    core::ptr::null(),
                    3500,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut n
                ),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!(
                rs_handheld_lxmf_get_request(
                    1.0,
                    0,
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    30,
                    &mut n
                ),
                RsHandheldStatus::ErrCapacity
            );
            assert_eq!(out, [0xAA; 96]);
            assert_eq!(n, 999);
            assert_eq!(
                rs_handheld_lxmf_get_request(
                    1.0,
                    0,
                    core::ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    out.len(),
                    &mut n
                ),
                RsHandheldStatus::Ok
            );
            assert_eq!(n, 31);
            let mut reply = [
                0x92, 0xc4, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90,
            ];
            let mut offset = 777;
            let mut length = 888;
            assert_eq!(
                rs_handheld_lxmf_response_view(
                    reply.as_ptr(),
                    reply.len(),
                    &[1; 16],
                    &mut offset,
                    &mut length
                ),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!((offset, length), (777, 888));
            assert_eq!(
                rs_handheld_lxmf_response_view(
                    reply.as_ptr(),
                    reply.len(),
                    &[0; 16],
                    &mut offset,
                    &mut length
                ),
                RsHandheldStatus::Ok
            );
            assert_eq!((offset, length), (19, 1));
            reply[19] = 0xc1;
            assert_eq!(
                rs_handheld_lxmf_response_view(
                    reply.as_ptr(),
                    reply.len(),
                    &[0; 16],
                    &mut offset,
                    &mut length
                ),
                RsHandheldStatus::ErrInvalidArg
            );
        }
    }

    #[test]
    fn response_resource_ffi_keeps_existing_transfer_on_refusal() {
        unsafe {
            let mut ctx = core::ptr::null_mut();
            assert_eq!(rs_handheld_rns_init(&mut ctx), RsHandheldStatus::Ok);
            let mut identity = [0xAA; 128];
            assert_eq!(
                rs_handheld_rns_link_identify(ctx, &[1; 16], &mut identity),
                RsHandheldStatus::ErrNotReady
            );
            assert_eq!(identity, [0xAA; 128]);
            assert_eq!(
                rs_handheld_rns_load_identity(ctx, &[0x25; 64]),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                rs_handheld_rns_link_identify(ctx, &[1; 16], &mut identity),
                RsHandheldStatus::Ok
            );
            let keys = LinkKeys::from_combined(&[0x34; 64]);
            let sender = OutboundResource::build(b"response", &keys, &[3; 4], &[4; 16]).unwrap();
            let mut adv = sender.advertisement();
            adv.flags.is_response = true;
            adv.request_id_len = 16;
            adv.request_id[..16].fill(0x42);
            let mut bytes = [0; rns_resource::ADV_PACKED_MAX];
            let n = adv.pack(&mut bytes).unwrap();
            let mut id = [0; 16];
            assert_eq!(
                rs_handheld_rns_resource_response_id(bytes.as_ptr(), n, &mut id),
                RsHandheldStatus::Ok
            );
            assert_eq!(id, [0x42; 16]);
            let (mut parts, mut transfer, mut size) = (99, 99, 99);
            let mut hash = [0xAA; 32];
            assert_eq!(
                rs_handheld_rns_resource_advertise_accept(
                    ctx,
                    bytes.as_ptr(),
                    n,
                    &mut parts,
                    &mut transfer,
                    &mut size,
                    &mut hash
                ),
                RsHandheldStatus::ErrUnsupported
            );
            assert_eq!(
                rs_handheld_rns_resource_response_accept(
                    ctx,
                    bytes.as_ptr(),
                    n,
                    &mut parts,
                    &mut transfer,
                    &mut size,
                    &mut hash,
                    &[0; 16]
                ),
                RsHandheldStatus::ErrUnsupported
            );
            assert_eq!((parts, transfer, size), (99, 99, 99));
            assert_eq!(hash, [0xAA; 32]);
            assert_eq!(
                rs_handheld_rns_resource_response_accept(
                    ctx,
                    bytes.as_ptr(),
                    n,
                    &mut parts,
                    &mut transfer,
                    &mut size,
                    &mut hash,
                    &id
                ),
                RsHandheldStatus::Ok
            );
            let generation = (*ctx).resource_generations[1];
            let old_hash = hash;
            adv.flags.compressed = true;
            let n = adv.pack(&mut bytes).unwrap();
            assert_eq!(
                rs_handheld_rns_resource_response_accept(
                    ctx,
                    bytes.as_ptr(),
                    n,
                    &mut parts,
                    &mut transfer,
                    &mut size,
                    &mut hash,
                    &id
                ),
                RsHandheldStatus::ErrUnsupported
            );
            assert_eq!((*ctx).resource_generations[1], generation);
            assert_eq!(
                *(*ctx).resource_in.as_ref().unwrap().resource_hash(),
                old_hash
            );
            rs_handheld_rns_shutdown(ctx);
        }
    }
}
