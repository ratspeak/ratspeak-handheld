//! LXMF message encoding, validation, and destination encryption.

use super::*;

/// Inline caps for the decoded message event. Sized to the FULL single-frame payload budget so the
/// `> cap` check can never wrongly reject a message that already fit one frame + passed validation
/// (title + content together are <= MAX_LXMF_PAYLOAD; either alone is <= it).
pub const RS_HANDHELD_LXMF_TITLE_MAX: usize = lxmf_lite_core::lxmf::MAX_LXMF_PAYLOAD;
pub const RS_HANDHELD_LXMF_CONTENT_MAX: usize = lxmf_lite_core::lxmf::MAX_LXMF_PAYLOAD;
pub const RS_HANDHELD_LXMF_BASE_KEY_HINT: u8 = 0xff;
const _: () =
    assert!(rns_lite_core::ratchet::RATCHET_RING_MAX < RS_HANDHELD_LXMF_BASE_KEY_HINT as usize);

/// A validated, decrypted inbound single-frame LXMF message, for the C++ MessageStore. `#[repr(C)]`;
/// `title`/`content` hold `*_len` valid bytes (the remainder is zeroed).
#[repr(C)]
pub struct RsHandheldLxmfMessage {
    pub message_id: [u8; 32],
    pub source_hash: [u8; DESTINATION_LENGTH],
    pub timestamp: f64,
    pub title_len: u32,
    pub title: [u8; RS_HANDHELD_LXMF_TITLE_MAX],
    pub content_len: u32,
    pub content: [u8; RS_HANDHELD_LXMF_CONTENT_MAX],
    /// 1 if the message carries a reaction (standard `FIELD_REACTION` 0x40 or the legacy
    /// Ratspeak envelope). The C++ engine proofs it but skips store/UI.
    pub is_reaction: u8,
}

pub(super) fn fill_lxmf_message(
    view: LxmfView<'_>,
    message: &mut RsHandheldLxmfMessage,
) -> RsHandheldStatus {
    if view.title.len() > RS_HANDHELD_LXMF_TITLE_MAX
        || view.content.len() > RS_HANDHELD_LXMF_CONTENT_MAX
    {
        return RsHandheldStatus::ErrCapacity;
    }
    message.message_id = view.message_id;
    message.source_hash = view.source_hash;
    message.timestamp = view.timestamp;
    message.title = [0u8; RS_HANDHELD_LXMF_TITLE_MAX];
    message.title[..view.title.len()].copy_from_slice(view.title);
    message.title_len = view.title.len() as u32;
    message.content = [0u8; RS_HANDHELD_LXMF_CONTENT_MAX];
    message.content[..view.content.len()].copy_from_slice(view.content);
    message.content_len = view.content.len() as u32;
    message.is_reaction = lxmf_is_reaction(view.fields) as u8;
    RsHandheldStatus::Ok
}

/// Reaction detection over the raw signature-covered `fields` element: standard
/// `FIELD_REACTION` (0x40, LXMF 1.0.1) or the legacy Ratspeak envelope (`FIELD_CUSTOM_TYPE`
/// 0xFB == "ratspeak.reaction", or "ratspeak.chat.v1"/"v2" whose `FIELD_CUSTOM_DATA` 0xFC
/// bin-wrapped map carries kind == "reaction"). Mirrors the micro backend's
/// `lxmfPayloadIsRatspeakReaction` so both backends drop the same messages.
pub(super) fn lxmf_is_reaction(fields: &[u8]) -> bool {
    use lxmf_lite_core::lxmf::{field_value, map_bytes_value, value_as_bytes};
    const FIELD_REACTION: u8 = 0x40;
    const FIELD_CUSTOM_TYPE: u8 = 0xFB;
    const FIELD_CUSTOM_DATA: u8 = 0xFC;
    if field_value(fields, FIELD_REACTION).is_some() {
        return true;
    }
    let Some(custom_type) = field_value(fields, FIELD_CUSTOM_TYPE).and_then(value_as_bytes) else {
        return false;
    };
    if custom_type == b"ratspeak.reaction" {
        return true;
    }
    if custom_type != b"ratspeak.chat.v1" && custom_type != b"ratspeak.chat.v2" {
        return false;
    }
    field_value(fields, FIELD_CUSTOM_DATA)
        .and_then(value_as_bytes)
        .and_then(|data| map_bytes_value(data, b"kind"))
        .and_then(value_as_bytes)
        .is_some_and(|kind| kind == b"reaction")
}

// ABI pins: the caps must match the C header `#define`s (303), and the f64 `timestamp` must sit at
// offset 48 (message_id[32] + source_hash[16]) — its 8-byte alignment introduces no hidden padding,
// so the Rust `#[repr(C)]` and the C struct agree byte-for-byte.
const _: () = {
    assert!(RS_HANDHELD_LXMF_TITLE_MAX == 303 && RS_HANDHELD_LXMF_CONTENT_MAX == 303);
    assert!(core::mem::offset_of!(RsHandheldLxmfMessage, timestamp) == 48);
};

/// Build a single-frame OPPORTUNISTIC LXMF message from the active identity (the source) to
/// `recipient_public_key` (the recipient's 64-byte public key, from their announce). Writes the
/// ECIES-encrypted RNS packet payload into `out` (the bytes to transmit to `*out_dest_hash`, the
/// recipient's lxmf.delivery destination), `*out_len` its length, and the 32-byte `*out_message_id`.
///
/// `timestamp` is the LXMF message timestamp. `wall_secs` (`0` if unavailable) and current-boot
/// `uptime_ms` are separate local clocks used only to expire the selected peer ratchet.
/// `ephemeral_priv` (32) + `iv` (16) are caller entropy
/// (`no_std`: no RNG) — fix them for deterministic vectors; on-device use platform randomness, fresh
/// per message. `title`/`content` may be null iff their length is 0. `out` must not overlap inputs.
///
/// `ErrNotReady` if no identity is loaded; `ErrCapacity` if the message is too large for a single
/// frame or `out` is too small; `ErrInvalidArg` on null/contract violations; `ErrCrypto` on failure.
///
/// # Safety
/// `ctx` live with a loaded identity; `recipient_public_key` 64 readable bytes; `ephemeral_priv` 32;
/// `iv` 16; `title`/`content` their lengths (or null iff 0); `out` `out_cap` writable; `out_len`,
/// `out_dest_hash` (16), `out_message_id` (32) writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_build(
    ctx: *const RsHandheldRns,
    recipient_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    timestamp: f64,
    wall_secs: u64,
    uptime_ms: u64,
    title: *const u8,
    title_len: usize,
    content: *const u8,
    content_len: usize,
    ephemeral_priv: *const [u8; 32],
    iv: *const [u8; 16],
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_dest_hash: *mut [u8; DESTINATION_LENGTH],
    out_message_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || recipient_public_key.is_null()
            || ephemeral_priv.is_null()
            || iv.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_dest_hash.is_null()
            || out_message_id.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if (title.is_null() && title_len != 0) || (content.is_null() && content_len != 0) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: pointers non-null + correctly sized per the contract.
        let title = if title_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(title, title_len) }
        };
        let content = if content_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(content, content_len) }
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        let mut dest = [0u8; DESTINATION_LENGTH];
        let mut mid = [0u8; 32];
        // Prefer the recipient's remembered announce ratchet (upstream Destination.encrypt);
        // base identity key when none is known. `timestamp` is host wall time — the expiry clock.
        let recipient = unsafe { &*recipient_public_key };
        let recipient_dest = {
            let idh = rns_lite_core::identity::identity_hash(recipient);
            rns_lite_core::identity::destination_hash_from_parts(
                &rns_lite_core::identity::name_hash(rns_lite_core::identity::LXMF_DELIVERY_NAME),
                Some(&idh),
            )
        };
        let peer_ratchet = unsafe { &*ctx }.peer_ratchets.get(
            &recipient_dest,
            RatchetClock::from_host(wall_secs, uptime_ms / 1000),
        );
        match lxmf_codec::build_opportunistic_ratchet(
            id,
            recipient,
            peer_ratchet.as_ref(),
            timestamp,
            title,
            content,
            unsafe { &*ephemeral_priv },
            unsafe { &*iv },
            out_slice,
            &mut dest,
            &mut mid,
        ) {
            Ok(n) => {
                // SAFETY: out pointers non-null + writable per the contract.
                unsafe {
                    *out_len = n;
                    *out_dest_hash = dest;
                    *out_message_id = mid;
                }
                RsHandheldStatus::Ok
            }
            Err(LxmfError::TooLong | LxmfError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(LxmfError::Crypto) => RsHandheldStatus::ErrCrypto,
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Decrypt + validate an inbound single-frame OPPORTUNISTIC LXMF packet payload (`data`/`data_len`,
/// addressed to the active identity) signed by the identity whose 64-byte public key is
/// `source_public_key` (known from the source's announce). Fills `*out_message` for the C++
/// MessageStore.
///
/// `ErrNotReady` if no identity is loaded; `ErrCrypto` if decryption, the signature, or the
/// source-hash binding fails; `ErrCapacity` if the title/content exceed the event caps;
/// `ErrInvalidArg` on null or malformed input.
///
/// # Safety
/// `ctx` live with a loaded identity; `data` `data_len` readable bytes; `source_public_key` 64
/// readable bytes; `out_message` a writable, aligned `RsHandheldLxmfMessage`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    source_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out_message: *mut RsHandheldLxmfMessage,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || source_public_key.is_null() || out_message.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
        // Ring first (peers encrypt to our announced ratchet), base key fallback — never enforced.
        let ring = unsafe { &*ctx }.ratchet_ring.private_keys();
        let view: LxmfView = match lxmf_codec::parse_opportunistic_ratchet(
            id,
            ring,
            data,
            unsafe { &*source_public_key },
            &mut scratch,
        ) {
            Ok((v, _ring_index)) => v,
            Err(
                LxmfError::Crypto | LxmfError::SignatureInvalid | LxmfError::SourceHashMismatch,
            ) => return RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                return RsHandheldStatus::ErrInvalidArg;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        fill_lxmf_message(view, unsafe { &mut *out_message })
    })
}

/// Final opportunistic parse using exactly the key hint returned by
/// `lxmf_peek_source_hint`. `0xff` means base key; `0..63` means that retained
/// ring index. Incorrect or out-of-range hints fail closed without rescanning.
///
/// # Safety
/// `ctx` is live; inputs name their declared readable sizes; `out_message` is writable/aligned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse_hint(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    key_hint: u8,
    source_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out_message: *mut RsHandheldLxmfMessage,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || source_public_key.is_null() || out_message.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &*ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let hint = if key_hint == RS_HANDHELD_LXMF_BASE_KEY_HINT {
            None
        } else {
            Some(key_hint as usize)
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
        let view = match lxmf_codec::parse_opportunistic_ratchet_hint(
            id,
            c.ratchet_ring.private_keys(),
            hint,
            data,
            unsafe { &*source_public_key },
            &mut scratch,
        ) {
            Ok(view) => view,
            Err(
                LxmfError::Crypto | LxmfError::SignatureInvalid | LxmfError::SourceHashMismatch,
            ) => return RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                return RsHandheldStatus::ErrInvalidArg;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        fill_lxmf_message(view, unsafe { &mut *out_message })
    })
}

/// Decrypt, resolve the embedded source through Rust-owned destination memory, and validate an
/// inbound single-frame OPPORTUNISTIC LXMF payload in one call. Resolution checks the durable
/// known-destination table first and the volatile path table second. If neither contains the
/// source key, returns `ErrSourceUnknown` without treating the payload as delivered.
///
/// On success `out_resolved_public_key` receives the exact key used for signature/source binding,
/// so the host can persist the same authenticated sender identity with the decoded message.
///
/// # Safety
/// `ctx` is live with a loaded identity and open node; `data` is `data_len` readable bytes;
/// `out_message` and `out_resolved_public_key` are writable and aligned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse_auto(
    ctx: *mut RsHandheldRns,
    data: *const u8,
    data_len: usize,
    out_message: *mut RsHandheldLxmfMessage,
    out_resolved_public_key: *mut [u8; PUBLIC_KEY_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || data.is_null()
            || out_message.is_null()
            || out_resolved_public_key.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &mut *ctx };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };

        let (source, key_hint) = {
            let id = match &c.identity {
                Some(id) => id,
                None => return RsHandheldStatus::ErrNotReady,
            };
            let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
            match lxmf_codec::peek_source_opportunistic_ratchet(
                id,
                c.ratchet_ring.private_keys(),
                data,
                &mut scratch,
            ) {
                Ok(pair) => pair,
                Err(LxmfError::Crypto) => return RsHandheldStatus::ErrCrypto,
                Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                    return RsHandheldStatus::ErrInvalidArg;
                }
                Err(_) => return RsHandheldStatus::ErrInternal,
            }
        };

        let source_public_key = {
            let node = match c.node {
                Some(mut p) => unsafe { p.as_mut() },
                None => return RsHandheldStatus::ErrNotReady,
            };
            match node.known_destination_recall(&source) {
                Some(public_key) => public_key,
                None => match node.paths.known_public_key(&source) {
                    Some(public_key) => *public_key,
                    None => return RsHandheldStatus::ErrSourceUnknown,
                },
            }
        };

        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
        let view = match lxmf_codec::parse_opportunistic_ratchet_hint(
            id,
            c.ratchet_ring.private_keys(),
            key_hint,
            data,
            &source_public_key,
            &mut scratch,
        ) {
            Ok(view) => view,
            Err(
                LxmfError::Crypto | LxmfError::SignatureInvalid | LxmfError::SourceHashMismatch,
            ) => return RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                return RsHandheldStatus::ErrInvalidArg;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let status = fill_lxmf_message(view, unsafe { &mut *out_message });
        if status == RsHandheldStatus::Ok {
            unsafe { *out_resolved_public_key = source_public_key };
        }
        status
    })
}

/// Decrypt-only PEEK of an inbound OPPORTUNISTIC payload's embedded 16-byte source hash,
/// so C++ can recall the source public key BEFORE the full validating `lxmf_parse` (Python
/// `Identity.recall` flow — the source is unknown at arrival). Fail-closed: reveals only the hash,
/// validates + stores nothing. `ErrNotReady` if no identity; `ErrCrypto` on decrypt failure;
/// `ErrInvalidArg` on null/malformed.
///
/// # Safety
/// `ctx` live with a loaded identity; `data` `data_len` readable bytes; `out_source_hash` 16 writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_peek_source(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    out_source_hash: *mut [u8; DESTINATION_LENGTH],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || out_source_hash.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
        let ring = unsafe { &*ctx }.ratchet_ring.private_keys();
        match lxmf_codec::peek_source_opportunistic_ratchet(id, ring, data, &mut scratch) {
            Ok((src, _ring_index)) => {
                unsafe { *out_source_hash = src };
                RsHandheldStatus::Ok
            }
            Err(LxmfError::Crypto) => RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                RsHandheldStatus::ErrInvalidArg
            }
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Ratchet-aware source peek that also returns the authenticated exact-key
/// hint for `lxmf_parse_hint` (`0xff` = base key, otherwise ring index).
///
/// # Safety
/// `ctx` is live; `data` names `data_len` readable bytes; both outputs are writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_peek_source_hint(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    out_source_hash: *mut [u8; DESTINATION_LENGTH],
    out_key_hint: *mut u8,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || out_source_hash.is_null() || out_key_hint.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &*ctx };
        let id = match &c.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
        match lxmf_codec::peek_source_opportunistic_ratchet(
            id,
            c.ratchet_ring.private_keys(),
            data,
            &mut scratch,
        ) {
            Ok((source, ring_index)) => {
                let hint = match ring_index {
                    Some(index) => match u8::try_from(index) {
                        Ok(index) if index < RS_HANDHELD_LXMF_BASE_KEY_HINT => index,
                        _ => return RsHandheldStatus::ErrInternal,
                    },
                    None => RS_HANDHELD_LXMF_BASE_KEY_HINT,
                };
                unsafe {
                    *out_source_hash = source;
                    *out_key_hint = hint;
                }
                RsHandheldStatus::Ok
            }
            Err(LxmfError::Crypto) => RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                RsHandheldStatus::ErrInvalidArg
            }
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Max FULL packed LXMF message (dest||source||sig||payload) the link/resource path carries — the
/// resource plaintext bound; a message larger than this cannot be delivered and is refused upstream.
pub(super) const LXMF_PACKED_MAX: usize = rns_resource::DATA_MAX;

/// Build a FULL packed LXMF message for LINK/RESOURCE (DIRECT) delivery: `dest(16) ||
/// source(16) || signature(64) || msgpack payload` — no ECIES wrap (the link session crypto
/// replaces it). Byte-identical to Python `LXMessage.pack()`. Transmit this either as a single
/// link data packet (link-encrypted) when it fits the link MDU, or as a resource. Writes the packed
/// bytes to `out`, `*out_len` its length, `*out_dest_hash` (16) the recipient lxmf.delivery dest,
/// `*out_message_id` (32). `timestamp` caller-supplied; `title`/`content` may be null iff len 0.
/// `ErrNotReady` if no identity; `ErrCapacity` if too large / `out` too small.
///
/// # Safety
/// `ctx` live with a loaded identity; `recipient_public_key` 64; `title`/`content` their lengths
/// (or null iff 0); `out` `out_cap` writable; `out_len`, `out_dest_hash` (16), `out_message_id` (32)
/// writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_build_link(
    ctx: *const RsHandheldRns,
    recipient_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    timestamp: f64,
    title: *const u8,
    title_len: usize,
    content: *const u8,
    content_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    out_dest_hash: *mut [u8; DESTINATION_LENGTH],
    out_message_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || recipient_public_key.is_null()
            || out.is_null()
            || out_len.is_null()
            || out_dest_hash.is_null()
            || out_message_id.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if (title.is_null() && title_len != 0) || (content.is_null() && content_len != 0) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        // SAFETY: non-null + sized per the contract.
        let title = if title_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(title, title_len) }
        };
        let content = if content_len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(content, content_len) }
        };
        let out_slice = unsafe { core::slice::from_raw_parts_mut(out, out_cap) };
        // Scratch off the task stack (heap-visible to [HEART]); big messages otherwise blow the
        // loopTask stack. box_zeroed: Box::new would materialise the array on the stack.
        let mut scratch: Box<[u8; LXMF_PACKED_MAX]> = match box_zeroed() {
            Some(b) => b,
            None => return RsHandheldStatus::ErrInternal,
        };
        let mut dest = [0u8; DESTINATION_LENGTH];
        let mut mid = [0u8; 32];
        match lxmf_codec::build_link(
            id,
            unsafe { &*recipient_public_key },
            timestamp,
            title,
            content,
            out_slice,
            scratch.as_mut_slice(),
            &mut dest,
            &mut mid,
        ) {
            Ok(n) => {
                unsafe {
                    *out_len = n;
                    *out_dest_hash = dest;
                    *out_message_id = mid;
                }
                RsHandheldStatus::Ok
            }
            Err(LxmfError::TooLong | LxmfError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Validate a FULL packed LXMF message received over a link / assembled from a resource:
/// `dest(16) || source(16) || signature(64) || msgpack payload`. `me` must be the delivery dest
/// (`ErrCrypto` on mismatch/forgery); `source_public_key` is recalled by the caller from
/// `data[16..32]`. Writes the decoded fields into caller buffers (title/content can exceed the
/// single-frame event caps for link/resource messages, so they are NOT the fixed
/// `RsHandheldLxmfMessage` — size them to `RS_HANDHELD_RESOURCE_DATA_MAX`). Truncates title/content
/// to the provided capacities (returns the true length in `*out_*_len`; caller sizes to DATA_MAX).
/// `*out_is_reaction` receives 1 if the message carries a reaction (standard 0x40 or legacy
/// Ratspeak envelope) — the caller proofs it but skips store/UI.
///
/// # Safety
/// `ctx` live with a loaded identity; `data` `data_len` readable; `source_public_key` 64;
/// `out_message_id` (32), `out_source_hash` (16), `out_timestamp` writable; `title`/`content` buffers
/// their capacities writable; `out_title_len`/`out_content_len` writable.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse_link(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    source_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out_message_id: *mut [u8; 32],
    out_source_hash: *mut [u8; DESTINATION_LENGTH],
    out_timestamp: *mut f64,
    title: *mut u8,
    title_cap: usize,
    out_title_len: *mut usize,
    content: *mut u8,
    content_cap: usize,
    out_content_len: *mut usize,
    out_is_reaction: *mut i32,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || data.is_null()
            || source_public_key.is_null()
            || out_message_id.is_null()
            || out_source_hash.is_null()
            || out_timestamp.is_null()
            || out_title_len.is_null()
            || out_content_len.is_null()
            || out_is_reaction.is_null()
            || (title.is_null() && title_cap != 0)
            || (content.is_null() && content_cap != 0)
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: `ctx` valid per the contract.
        let id = match &unsafe { &*ctx }.identity {
            Some(id) => id,
            None => return RsHandheldStatus::ErrNotReady,
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        // Scratch (64 + payload) off the task stack. box_zeroed: Box::new would
        // materialise the array on the stack.
        let mut scratch: Box<[u8; LXMF_PACKED_MAX]> = match box_zeroed() {
            Some(b) => b,
            None => return RsHandheldStatus::ErrInternal,
        };
        let view = match lxmf_codec::parse_link(
            id,
            data,
            unsafe { &*source_public_key },
            scratch.as_mut_slice(),
        ) {
            Ok(v) => v,
            Err(
                LxmfError::Crypto
                | LxmfError::SignatureInvalid
                | LxmfError::SourceHashMismatch
                | LxmfError::DestinationMismatch,
            ) => return RsHandheldStatus::ErrCrypto,
            Err(LxmfError::MalformedPayload | LxmfError::OutputTooSmall) => {
                return RsHandheldStatus::ErrInvalidArg;
            }
            Err(_) => return RsHandheldStatus::ErrInternal,
        };
        let tn = view.title.len().min(title_cap);
        let cn = view.content.len().min(content_cap);
        // SAFETY: out pointers non-null + sized per the contract.
        unsafe {
            *out_message_id = view.message_id;
            *out_source_hash = view.source_hash;
            *out_timestamp = view.timestamp;
            if tn != 0 {
                core::slice::from_raw_parts_mut(title, tn).copy_from_slice(&view.title[..tn]);
            }
            if cn != 0 {
                core::slice::from_raw_parts_mut(content, cn).copy_from_slice(&view.content[..cn]);
            }
            *out_title_len = view.title.len();
            *out_content_len = view.content.len();
            *out_is_reaction = lxmf_is_reaction(view.fields) as i32;
        }
        RsHandheldStatus::Ok
    })
}
