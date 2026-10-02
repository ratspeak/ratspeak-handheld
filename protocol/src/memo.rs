//! Recorded audio uses LXMF fields and native Codec2 frames, never LXST packets.
//! The embedding storage/audio owners decide when to record, persist and play.
use super::*;

pub const RS_HANDHELD_MEMO_MODE: u8 = lxmf_codec::AM_CODEC2_700C;
pub const RS_HANDHELD_MEMO_RECORD_BYTES: usize = 1500;
pub const RS_HANDHELD_MEMO_PLAY_BYTES: usize = 3000;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RsHandheldMemoInfo {
    pub frames: u32,
    pub duration_ms: u32,
    pub sample_rate: u32,
    pub frame_samples: u32,
    pub frame_bytes: u32,
    /// The existing codec owner accepts this internal profile. It is NOT an
    /// LXMF mode and must never be prepended to the stored audio bytes.
    pub codec_profile: u32,
}

/// Inspect supported headerless native audio before allocating/starting playback.
/// Only 700C, a positive integral frame count and <=30 seconds are supported.
/// The separate recorder limit is 15 seconds. This validates framing/duration,
/// not the perceptual quality or trustworthiness of the recorded sound.
/// # Safety
/// `out` is aligned and writable; it stays unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_memo_inspect(
    mode: u8,
    byte_length: usize,
    out: *mut RsHandheldMemoInfo,
) -> RsHandheldStatus {
    if out.is_null() {
        return RsHandheldStatus::ErrInvalidArg;
    }
    if mode != RS_HANDHELD_MEMO_MODE {
        return RsHandheldStatus::ErrUnsupported;
    }
    let native = lxst_codec2::Mode::Rate700C;
    if byte_length == 0 || byte_length % native.bytes() != 0 {
        return RsHandheldStatus::ErrInvalidArg;
    }
    if byte_length > RS_HANDHELD_MEMO_PLAY_BYTES {
        return RsHandheldStatus::ErrCapacity;
    }
    let frames = byte_length / native.bytes();
    unsafe {
        *out = RsHandheldMemoInfo {
            frames: frames as u32,
            duration_ms: (frames * native.samples() / 8) as u32,
            sample_rate: 8000,
            frame_samples: native.samples() as u32,
            frame_bytes: native.bytes() as u32,
            codec_profile: 0x10,
        };
    }
    RsHandheldStatus::Ok
}

/// Exact full packed size, without recipient stamp or propagation wrapping.
/// # Safety
/// `out_size` is aligned and writable; errors leave it unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_audio_packed_size(
    title_len: usize,
    content_len: usize,
    mode: u8,
    audio_len: usize,
    out_size: *mut usize,
) -> RsHandheldStatus {
    if out_size.is_null() {
        return RsHandheldStatus::ErrInvalidArg;
    }
    match lxmf_codec::packed_len_with_audio(title_len, content_len, mode, audio_len) {
        Ok(n) => {
            unsafe {
                *out_size = n;
            }
            RsHandheldStatus::Ok
        }
        Err(_) => RsHandheldStatus::ErrCapacity,
    }
}

/// Build a native audio-bearing LXMF message for Direct or existing PN wrapping.
/// Audio is covered by the ID/signature. No codec validation occurs here: a
/// stored unsupported mode can be retained/forwarded without claiming playback.
/// The exact whole-message Resource bound is enforced before allocation/signing.
/// # Safety
/// `ctx` is live; public key is 64 readable bytes; byte inputs are readable for
/// their lengths (NULL only for zero length). Outputs are aligned/writable for
/// their stated sizes. Every input/output region is disjoint. Scalar outputs
/// are unchanged on failure; read the packed buffer only on success.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_build_audio(
    ctx: *const RsHandheldRns,
    recipient_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    timestamp: f64,
    title: *const u8,
    title_len: usize,
    content: *const u8,
    content_len: usize,
    mode: u8,
    audio: *const u8,
    audio_len: usize,
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
            || (title.is_null() && title_len != 0)
            || (content.is_null() && content_len != 0)
            || (audio.is_null() && audio_len != 0)
            || !timestamp.is_finite()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let needed =
            match lxmf_codec::packed_len_with_audio(title_len, content_len, mode, audio_len) {
                Ok(n) if n <= lxmf::LXMF_PACKED_MAX && n <= out_cap => n,
                _ => return RsHandheldStatus::ErrCapacity,
            };
        let Some(identity) = (unsafe { &*ctx }).identity.as_ref() else {
            return RsHandheldStatus::ErrNotReady;
        };
        // Size checks precede slicing, including hostile usize lengths.
        let bytes = |p: *const u8, n| {
            if n == 0 {
                &[][..]
            } else {
                unsafe { core::slice::from_raw_parts(p, n) }
            }
        };
        let Some(mut scratch) = box_zeroed::<[u8; lxmf::LXMF_PACKED_MAX]>() else {
            return RsHandheldStatus::ErrInternal;
        };
        let mut destination = [0; 16];
        let mut id = [0; 32];
        match lxmf_codec::build_link_with_audio(
            identity,
            unsafe { &*recipient_public_key },
            timestamp,
            bytes(title, title_len),
            bytes(content, content_len),
            lxmf_codec::AudioField {
                mode,
                bytes: bytes(audio, audio_len),
            },
            unsafe { core::slice::from_raw_parts_mut(out, needed) },
            scratch.as_mut_slice(),
            &mut destination,
            &mut id,
        ) {
            Ok(n) => {
                unsafe {
                    *out_len = n;
                    *out_dest_hash = destination;
                    *out_message_id = id;
                }
                RsHandheldStatus::Ok
            }
            Err(LxmfError::TooLong | LxmfError::OutputTooSmall) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrInternal,
        }
    })
}

/// Audio field state: 0 absent, 1 well-framed, 2 malformed. Unsupported modes
/// remain well-framed; `memo_inspect` separately decides local playback support.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RsHandheldAudioView {
    pub state: u32,
    pub mode: u32,
    pub offset: u32,
    pub length: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RsHandheldMediaView {
    pub message: RsHandheldLxmfView,
    pub audio: RsHandheldAudioView,
}
const _: () = {
    assert!(core::mem::size_of::<RsHandheldMemoInfo>() == 24);
    assert!(core::mem::size_of::<RsHandheldAudioView>() == 16);
    assert!(core::mem::size_of::<RsHandheldMediaView>() == 96);
    assert!(core::mem::offset_of!(RsHandheldMediaView, audio) == 80);
};
fn media_view(
    view: LxmfView<'_>,
    base: usize,
    length: usize,
) -> Result<RsHandheldMediaView, RsHandheldStatus> {
    let offset = |bytes: &[u8]| -> Result<u32, RsHandheldStatus> {
        let at = (bytes.as_ptr() as usize)
            .checked_sub(base)
            .ok_or(RsHandheldStatus::ErrInternal)?;
        if at > length || bytes.len() > length - at {
            return Err(RsHandheldStatus::ErrInternal);
        }
        u32::try_from(at).map_err(|_| RsHandheldStatus::ErrInternal)
    };
    let audio = match lxmf_codec::audio_field(view.fields) {
        Ok(None) => RsHandheldAudioView::default(),
        Ok(Some(field)) => RsHandheldAudioView {
            state: 1,
            mode: field.mode as u32,
            offset: offset(field.bytes)?,
            length: field.bytes.len() as u32,
        },
        Err(_) => RsHandheldAudioView {
            state: 2,
            ..Default::default()
        },
    };
    Ok(RsHandheldMediaView {
        message: RsHandheldLxmfView {
            message_id: view.message_id,
            source_hash: view.source_hash,
            timestamp: view.timestamp,
            title_offset: offset(view.title)?,
            title_len: view.title.len() as u32,
            content_offset: offset(view.content)?,
            content_len: view.content.len() as u32,
            is_reaction: lxmf::lxmf_is_reaction(view.fields) as i32,
        },
        audio,
    })
}

/// Authenticate a full Link/Resource message and expose its text/audio spans.
/// Malformed audio is reported separately from message authentication. No proof,
/// persistence, playback or pointer is retained by this function.
/// # Safety
/// Inputs have their declared readable lengths; context is live, out is aligned
/// and writable. All regions are disjoint. Spans borrow the exact immutable data
/// and are valid only on success. Output is unchanged on authentication failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse_media_view(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    source_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    out: *mut RsHandheldMediaView,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null() || data.is_null() || source_public_key.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if data_len > lxmf::LXMF_PACKED_MAX {
            return RsHandheldStatus::ErrCapacity;
        }
        let Some(mut scratch) = box_zeroed::<[u8; lxmf::LXMF_PACKED_MAX]>() else {
            return RsHandheldStatus::ErrInternal;
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let view = match lxmf::validated_direct_view(
            unsafe { &*ctx },
            data,
            unsafe { &*source_public_key },
            scratch.as_mut_slice(),
        ) {
            Ok(view) => view,
            Err(error) => return error,
        };
        match media_view(view, data.as_ptr() as usize, data.len()) {
            Ok(view) => {
                unsafe {
                    *out = view;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => error,
        }
    })
}

/// Authenticate opportunistic media using the exact hint from `peek_source_hint`
/// and the caller's recalled source key, as in `lxmf_parse_hint`. All spans refer to the
/// caller-owned plaintext scratch, never the encrypted input or a Rust local.
/// # Safety
/// Context is live; inputs readable, all outputs aligned/writable
/// and disjoint. `plain` has at least MAX_ECIES_PLAINTEXT bytes. On failure its
/// contents are unspecified and MUST NOT be consumed. Metadata outputs remain
/// unchanged. On success retain plaintext unchanged until the storage owner has
/// copied/admitted every referenced span.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_lxmf_parse_hint_media(
    ctx: *const RsHandheldRns,
    data: *const u8,
    data_len: usize,
    key_hint: u8,
    source_public_key: *const [u8; PUBLIC_KEY_LENGTH],
    plain: *mut u8,
    plain_cap: usize,
    out: *mut RsHandheldMediaView,
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || data.is_null()
            || plain.is_null()
            || out.is_null()
            || source_public_key.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if data_len > rns_lite_core::constants::MDU
            || plain_cap < rns_lite_core::crypto::MAX_ECIES_PLAINTEXT
        {
            return RsHandheldStatus::ErrCapacity;
        }
        let c = unsafe { &*ctx };
        let Some(identity) = c.identity.as_ref() else {
            return RsHandheldStatus::ErrNotReady;
        };
        let data = unsafe { core::slice::from_raw_parts(data, data_len) };
        let scratch = unsafe {
            core::slice::from_raw_parts_mut(plain, rns_lite_core::crypto::MAX_ECIES_PLAINTEXT)
        };
        let hint = if key_hint == RS_HANDHELD_LXMF_BASE_KEY_HINT {
            None
        } else {
            Some(key_hint as usize)
        };
        let view = match lxmf_codec::parse_opportunistic_ratchet_hint(
            identity,
            c.ratchet_ring.private_keys(),
            hint,
            data,
            unsafe { &*source_public_key },
            scratch,
        ) {
            Ok(view) => view,
            Err(e) => return map_parse_error(e),
        };
        match media_view(
            view,
            plain as usize,
            rns_lite_core::crypto::MAX_ECIES_PLAINTEXT,
        ) {
            Ok(view) => {
                unsafe {
                    *out = view;
                }
                RsHandheldStatus::Ok
            }
            Err(e) => e,
        }
    })
}
fn map_parse_error(error: LxmfError) -> RsHandheldStatus {
    match error {
        LxmfError::Crypto
        | LxmfError::SignatureInvalid
        | LxmfError::SourceHashMismatch
        | LxmfError::DestinationMismatch => RsHandheldStatus::ErrCrypto,
        LxmfError::MalformedPayload | LxmfError::OutputTooSmall => RsHandheldStatus::ErrInvalidArg,
        LxmfError::TooLong => RsHandheldStatus::ErrCapacity,
    }
}

#[cfg(test)]
#[path = "memo_tests.rs"]
mod tests;
