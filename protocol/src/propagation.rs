//! Checked propagation metadata and client primitives at the C boundary.

use super::*;

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
