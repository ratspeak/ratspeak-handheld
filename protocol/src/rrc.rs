//! Bounded adaptation of Ratspeak `ratspeak-runtime/src/rrc.rs` (RRC v1).
//! No allocation, IO, clocks, randomness, or session ownership. A decoded view
//! borrows offsets into one authenticated Link packet. Unlike the desktop CBOR
//! tree this endpoint rejects indefinite containers, duplicate known envelope
//! keys, and nesting beyond four levels. Resource envelopes are not advertised.

use super::{RsHandheldStatus, guard};
#[path = "rrc_unicode.rs"]
mod unicode;

const MDU: usize = 431;
const MAX_DEPTH: usize = 4;

/// Domain-separated local RRC conversation key. kind 0 names a normalized UTF-8
/// room (1..64 bytes); kind 1 names a participant identity (exactly 16 bytes).
/// This private storage key is never used as an LXMF/Reticulum destination.
/// # Safety
/// Input readable for length; output writable for 16 bytes, non-aliasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_storage_key(
    kind: u8,
    data: *const u8,
    length: usize,
    out: *mut [u8; 16],
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null()
            || out.is_null()
            || kind > 1
            || length == 0
            || length > 64
            || (kind == 1 && length != 16)
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        use sha2::{Digest, Sha256};
        let bytes = unsafe { core::slice::from_raw_parts(data, length) };
        if kind == 0 && core::str::from_utf8(bytes).is_err() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mut hash = Sha256::new();
        hash.update(b"ratspeak.handheld.rrc.v1\0");
        hash.update([kind]);
        hash.update(bytes);
        let digest = hash.finalize();
        unsafe { (*out).copy_from_slice(&digest[..16]) };
        RsHandheldStatus::Ok
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Error {
    Invalid,
    Capacity,
    Unsupported,
}
type Result<T> = core::result::Result<T, Error>;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RrcSpan {
    pub offset: u16,
    pub length: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RrcMeta {
    pub kind: u64,
    pub timestamp_ms: u64,
    pub id: [u8; 8],
    pub source: [u8; 16],
    pub destination: [u8; 16],
    pub has_destination: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RrcView {
    pub meta: RrcMeta,
    pub room: RrcSpan,
    pub nickname: RrcSpan,
    pub body: RrcSpan, // complete encoded value; empty means absent
    pub text: RrcSpan, // body text only (body_kind == 1)
    pub body_kind: u8, // 0 absent, 1 text, 2 other CBOR
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RrcWelcome {
    pub name: RrcSpan,
    pub version: RrcSpan,
    pub limits: [u32; 5],
    pub limits_present: u8,
    pub capabilities: u8,
}

struct Reader<'a> {
    bytes: &'a [u8],
    pos: usize,
}
impl<'a> Reader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, pos: 0 }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        let end = self.pos.checked_add(n).ok_or(Error::Invalid)?;
        let value = self.bytes.get(self.pos..end).ok_or(Error::Invalid)?;
        self.pos = end;
        Ok(value)
    }
    fn head(&mut self) -> Result<(u8, u64)> {
        let byte = self.take(1)?[0];
        let n = match byte & 31 {
            n @ 0..=23 => u64::from(n),
            n @ 24..=27 => {
                let mut value = 0u64;
                for b in self.take(1usize << (n - 24))? {
                    value = (value << 8) | u64::from(*b);
                }
                value
            }
            _ => return Err(Error::Unsupported),
        };
        Ok((byte >> 5, n))
    }
    fn uint(&mut self) -> Result<u64> {
        let (major, value) = self.head()?;
        if major != 0 {
            return Err(Error::Invalid);
        }
        Ok(value)
    }
    fn container(&mut self, wanted: u8) -> Result<usize> {
        let (major, count) = self.head()?;
        if major != wanted || count > 64 {
            return Err(Error::Invalid);
        }
        Ok(count as usize)
    }
    fn span(&mut self, wanted: u8, max: usize) -> Result<RrcSpan> {
        let (major, n) = self.head()?;
        if major != wanted || n > max as u64 {
            return Err(Error::Invalid);
        }
        let span = RrcSpan {
            offset: self.pos as u16,
            length: n as u16,
        };
        let data = self.take(n as usize)?;
        if major == 3 {
            core::str::from_utf8(data).map_err(|_| Error::Invalid)?;
        }
        Ok(span)
    }
    fn fixed<const N: usize>(&mut self) -> Result<[u8; N]> {
        let span = self.span(2, N)?;
        if span.length as usize != N {
            return Err(Error::Invalid);
        }
        self.bytes[span.offset as usize..span.offset as usize + N]
            .try_into()
            .map_err(|_| Error::Invalid)
    }
    fn optional_text(&mut self, max: usize) -> Result<RrcSpan> {
        if self.bytes.get(self.pos) == Some(&0xf6) {
            self.pos += 1;
            Ok(RrcSpan::default())
        } else {
            self.span(3, max)
        }
    }
    fn skip(&mut self, depth: usize) -> Result<()> {
        if depth > MAX_DEPTH {
            return Err(Error::Unsupported);
        }
        let (major, n) = self.head()?;
        match major {
            0 | 1 | 7 => (),
            2 | 3 => {
                let n = usize::try_from(n).map_err(|_| Error::Invalid)?;
                let bytes = self.take(n)?;
                if major == 3 {
                    core::str::from_utf8(bytes).map_err(|_| Error::Invalid)?;
                }
            }
            4 | 5 => {
                if n > 64 {
                    return Err(Error::Unsupported);
                }
                for _ in 0..n * if major == 5 { 2 } else { 1 } {
                    self.skip(depth + 1)?;
                }
            }
            6 => self.skip(depth + 1)?,
            _ => return Err(Error::Invalid),
        }
        Ok(())
    }
}

fn decode(bytes: &[u8]) -> Result<RrcView> {
    if bytes.len() > MDU {
        return Err(Error::Capacity);
    }
    let mut reader = Reader::new(bytes);
    let count = reader.container(5)?;
    let mut seen = 0u16;
    let mut result = RrcView::default();
    for _ in 0..count {
        let key = reader.uint()?;
        if key <= 8 {
            if seen & (1 << key) != 0 {
                return Err(Error::Invalid);
            }
            seen |= 1 << key;
        }
        match key {
            0 => {
                if reader.uint()? != 1 {
                    return Err(Error::Unsupported);
                }
            }
            1 => result.meta.kind = reader.uint()?,
            2 => result.meta.id = reader.fixed()?,
            3 => result.meta.timestamp_ms = reader.uint()?,
            4 => result.meta.source = reader.fixed()?,
            5 => result.room = reader.optional_text(64)?,
            6 => {
                let start = reader.pos;
                if reader.bytes.get(start).is_some_and(|b| b >> 5 == 3) {
                    result.text = reader.span(3, MDU)?;
                    result.body_kind = 1;
                } else {
                    reader.skip(1)?;
                    result.body_kind = 2;
                }
                result.body = RrcSpan {
                    offset: start as u16,
                    length: (reader.pos - start) as u16,
                };
            }
            7 => result.nickname = reader.optional_text(32)?,
            8 => {
                result.meta.destination = reader.fixed()?;
                result.meta.has_destination = 1;
            }
            _ => reader.skip(1)?,
        }
    }
    if seen & 31 != 31 || reader.pos != bytes.len() {
        return Err(Error::Invalid);
    }
    Ok(result)
}

// Counting and writing share the exact encoder. Capacity failure never publishes
// partial bytes; callers can calculate limits using the actual envelope overhead.
struct Writer<'a> {
    out: Option<&'a mut [u8]>,
    pos: usize,
}
impl Writer<'_> {
    fn raw(&mut self, value: &[u8]) -> Result<()> {
        let end = self.pos.checked_add(value.len()).ok_or(Error::Capacity)?;
        if end > MDU {
            return Err(Error::Capacity);
        }
        if let Some(out) = self.out.as_mut() {
            out.get_mut(self.pos..end)
                .ok_or(Error::Capacity)?
                .copy_from_slice(value);
        }
        self.pos = end;
        Ok(())
    }
    fn head(&mut self, major: u8, value: u64) -> Result<()> {
        let bytes = value.to_be_bytes();
        let (tag, start) = match value {
            0..=23 => (value as u8, 8),
            24..=255 => (24, 7),
            256..=65535 => (25, 6),
            65536..=0xffff_ffff => (26, 4),
            _ => (27, 0),
        };
        self.raw(&[(major << 5) | tag])?;
        self.raw(&bytes[start..])
    }
    fn bytes(&mut self, major: u8, value: &[u8]) -> Result<()> {
        self.head(major, value.len() as u64)?;
        self.raw(value)
    }
    fn uint(&mut self, value: u64) -> Result<()> {
        self.head(0, value)
    }
}

fn encode_to(
    w: &mut Writer<'_>,
    meta: &RrcMeta,
    room: &[u8],
    nick: &[u8],
    body: &[u8],
    body_kind: u8,
) -> Result<()> {
    if room.len() > 64 || nick.len() > 32 || meta.has_destination > 1 || body_kind > 3 {
        return Err(Error::Invalid);
    }
    for text in [room, nick] {
        core::str::from_utf8(text).map_err(|_| Error::Invalid)?;
    }
    if body_kind == 1 || body_kind == 3 {
        core::str::from_utf8(body).map_err(|_| Error::Invalid)?;
    }
    if body_kind == 2 {
        let mut r = Reader::new(body);
        r.skip(1)?;
        if r.pos != body.len() {
            return Err(Error::Invalid);
        }
    }
    w.head(
        5,
        5 + u64::from(!room.is_empty())
            + u64::from(!nick.is_empty())
            + u64::from(body_kind != 0)
            + u64::from(meta.has_destination),
    )?;
    w.uint(0)?;
    w.uint(1)?;
    w.uint(1)?;
    w.uint(meta.kind)?;
    w.uint(2)?;
    w.bytes(2, &meta.id)?;
    w.uint(3)?;
    w.uint(meta.timestamp_ms)?;
    w.uint(4)?;
    w.bytes(2, &meta.source)?;
    if !room.is_empty() {
        w.uint(5)?;
        w.bytes(3, room)?;
    }
    if body_kind != 0 {
        w.uint(6)?;
        match body_kind {
            1 => w.bytes(3, body)?,
            2 => w.raw(body)?,
            3 => {
                if meta.kind != 1 {
                    return Err(Error::Invalid);
                }
                w.head(5, 3)?;
                w.uint(0)?;
                w.bytes(3, b"Ratspeak Handheld")?;
                w.uint(1)?;
                w.bytes(3, body)?;
                w.uint(2)?;
                w.head(5, 3)?;
                w.uint(0)?;
                w.raw(&[0xf4])?; // bounded packet client, no Resource envelope
                w.uint(1)?;
                w.raw(&[0xf5])?;
                w.uint(2)?;
                w.raw(&[0xf5])?;
            }
            _ => unreachable!(),
        }
    }
    if !nick.is_empty() {
        w.uint(7)?;
        w.bytes(3, nick)?;
    }
    if meta.has_destination != 0 {
        w.uint(8)?;
        w.bytes(2, &meta.destination)?;
    }
    Ok(())
}

fn welcome(bytes: &[u8]) -> Result<RrcWelcome> {
    let view = decode(bytes)?;
    if view.meta.kind != 2 || view.body.length == 0 {
        return Err(Error::Invalid);
    }
    let mut r = Reader {
        bytes,
        pos: view.body.offset as usize,
    };
    let count = r.container(5)?;
    let mut result = RrcWelcome::default();
    let mut seen = 0u8;
    for _ in 0..count {
        let key = r.uint()?;
        if key <= 3 {
            if seen & (1 << key) != 0 {
                return Err(Error::Invalid);
            }
            seen |= 1 << key;
        }
        match key {
            0 => result.name = r.optional_text(64)?,
            1 => result.version = r.optional_text(64)?,
            2 | 3 => {
                let count = r.container(5)?;
                let mut fields = 0u8;
                for _ in 0..count {
                    let field = r.uint()?;
                    if field <= 4 {
                        if fields & (1 << field) != 0 {
                            return Err(Error::Invalid);
                        }
                        fields |= 1 << field;
                    }
                    if key == 2 && field < 4 {
                        let value = r.take(1)?[0];
                        if value == 0xf5 {
                            result.capabilities |= 1 << field;
                        } else if value != 0xf4 {
                            return Err(Error::Invalid);
                        }
                    } else if key == 3 && field < 5 {
                        result.limits[field as usize] = r.uint()?.min(u64::from(u32::MAX)) as u32;
                        result.limits_present |= 1 << field;
                    } else {
                        r.skip(2)?;
                    }
                }
            }
            _ => r.skip(1)?,
        }
    }
    Ok(result)
}

fn status(error: Error) -> RsHandheldStatus {
    match error {
        Error::Invalid => RsHandheldStatus::ErrInvalidArg,
        Error::Capacity => RsHandheldStatus::ErrCapacity,
        Error::Unsupported => RsHandheldStatus::ErrUnsupported,
    }
}

/// Trim a nickname or trim/lowercase a room with the desktop client's Unicode
/// rules. Output excludes a NUL terminator. No normalization of '#' or spaces.
/// # Safety
/// Input is readable for length; output writable for capacity; out_length writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_normalize(
    data: *const u8,
    length: usize,
    nickname: u8,
    out: *mut u8,
    capacity: usize,
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || out_length.is_null() || length > MDU || nickname > 1 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts(data, length) };
        let Ok(text) = core::str::from_utf8(bytes) else {
            return RsHandheldStatus::ErrInvalidArg;
        };
        let text = text.trim();
        if text.is_empty() || (nickname != 0 && text.contains(['\n', '\r', '\0'])) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let mut normalized = [0u8; 64];
        let mut used = 0usize;
        if nickname != 0 {
            if text.len() > 32 {
                return RsHandheldStatus::ErrCapacity;
            }
            used = text.len();
            normalized[..used].copy_from_slice(text.as_bytes());
        } else {
            fn context_is_cased(mut chars: impl Iterator<Item = char>) -> bool {
                chars
                    .find(|c| {
                        if c.is_ascii() {
                            !matches!(c, '\'' | '.' | ':' | '^' | '`')
                        } else {
                            !unicode::case_ignorable::lookup(*c)
                        }
                    })
                    .is_some_and(|c| {
                        if c.is_ascii() {
                            c.is_ascii_alphabetic()
                        } else {
                            unicode::cased::lookup(c)
                        }
                    })
            }
            let mut emit = |ch: char| {
                let mut utf8 = [0u8; 4];
                let encoded = ch.encode_utf8(&mut utf8).as_bytes();
                if used + encoded.len() > normalized.len() {
                    return false;
                }
                normalized[used..used + encoded.len()].copy_from_slice(encoded);
                used += encoded.len();
                true
            };
            for (i, ch) in text.char_indices() {
                if ch == 'Σ' {
                    // Same contextual Final_Sigma rule as Rust str::to_lowercase,
                    // used by the trusted desktop normalize_room implementation.
                    let final_sigma = context_is_cased(&mut text[..i].chars().rev())
                        && !context_is_cased(&mut text[i + ch.len_utf8()..].chars());
                    if !emit(if final_sigma { 'ς' } else { 'σ' }) {
                        return RsHandheldStatus::ErrCapacity;
                    }
                } else {
                    for lowered in ch.to_lowercase() {
                        if !emit(lowered) {
                            return RsHandheldStatus::ErrCapacity;
                        }
                    }
                }
            }
        }
        unsafe { *out_length = used };
        if capacity < used {
            return RsHandheldStatus::ErrCapacity;
        }
        unsafe { core::slice::from_raw_parts_mut(out, used) }.copy_from_slice(&normalized[..used]);
        RsHandheldStatus::Ok
    })
}

/// Decode an RRC packet into checked, borrowed byte offsets. Output is untouched
/// on failure. Offsets are valid only while the original input remains unchanged.
/// # Safety
/// `data` is readable for `length` bytes; `out` is writable and does not alias it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_decode(
    data: *const u8,
    length: usize,
    out: *mut RrcView,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || length > MDU {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointer lengths and exclusive output are the caller's contract.
        match decode(unsafe { core::slice::from_raw_parts(data, length) }) {
            Ok(view) => {
                unsafe { *out = view };
                RsHandheldStatus::Ok
            }
            Err(e) => status(e),
        }
    })
}

/// Encode/count one bounded RRC envelope. body_kind: absent=0, UTF-8 text=1,
/// complete CBOR=2 (PONG echo), HELLO client-version=3. Empty room/nick omit them.
/// A null `out` counts only; otherwise output is untouched if capacity is too small.
/// # Safety
/// Every pointer is readable for its stated size (empty slices may be null);
/// `meta` readable, `out_length` writable, and optional `out` writable for capacity.
/// Output must not alias any input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_encode(
    meta: *const RrcMeta,
    room: *const u8,
    room_length: usize,
    nickname: *const u8,
    nickname_length: usize,
    body: *const u8,
    body_length: usize,
    body_kind: u8,
    out: *mut u8,
    capacity: usize,
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if meta.is_null()
            || out_length.is_null()
            || room_length > 64
            || nickname_length > 32
            || body_length > MDU
            || (room_length != 0 && room.is_null())
            || (nickname_length != 0 && nickname.is_null())
            || (body_length != 0 && body.is_null())
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: pointers/lengths checked above, non-overlap required by contract.
        let (meta, room, nick, body) = unsafe {
            (
                &*meta,
                if room_length == 0 {
                    &[][..]
                } else {
                    core::slice::from_raw_parts(room, room_length)
                },
                if nickname_length == 0 {
                    &[][..]
                } else {
                    core::slice::from_raw_parts(nickname, nickname_length)
                },
                if body_length == 0 {
                    &[][..]
                } else {
                    core::slice::from_raw_parts(body, body_length)
                },
            )
        };
        let mut count = Writer { out: None, pos: 0 };
        if let Err(e) = encode_to(&mut count, meta, room, nick, body, body_kind) {
            return status(e);
        }
        unsafe { *out_length = count.pos };
        if out.is_null() {
            return RsHandheldStatus::Ok;
        }
        if capacity < count.pos {
            return RsHandheldStatus::ErrCapacity;
        }
        let mut writer = Writer {
            out: Some(unsafe { core::slice::from_raw_parts_mut(out, capacity) }),
            pos: 0,
        };
        match encode_to(&mut writer, meta, room, nick, body, body_kind) {
            Ok(()) => RsHandheldStatus::Ok,
            Err(e) => status(e),
        }
    })
}

/// Decode authenticated WELCOME fields. Limits retain explicit zero values.
/// # Safety
/// Input readable for length; output writable, non-aliasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_welcome(
    data: *const u8,
    length: usize,
    out: *mut RrcWelcome,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || length > MDU {
            return RsHandheldStatus::ErrInvalidArg;
        }
        match welcome(unsafe { core::slice::from_raw_parts(data, length) }) {
            Ok(value) => {
                unsafe { *out = value };
                RsHandheldStatus::Ok
            }
            Err(e) => status(e),
        }
    })
}

/// Read a full JOINED/PARTED roster; validate all entries before exposing one.
/// Null/absent bodies have count zero. `index >= count` returns capacity.
/// # Safety
/// Input readable; `out_count` writable; optional `out_member` writable for 16 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_member(
    data: *const u8,
    length: usize,
    index: usize,
    out_member: *mut [u8; 16],
    out_count: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out_count.is_null() || length > MDU {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts(data, length) };
        let parse = || -> Result<(usize, [u8; 16])> {
            let view = decode(bytes)?;
            if view.meta.kind != 11 && view.meta.kind != 13 {
                return Err(Error::Invalid);
            }
            let mut r = Reader {
                bytes,
                pos: view.body.offset as usize,
            };
            if view.body.length == 0 || (view.body.length == 1 && bytes[r.pos] == 0xf6) {
                return Ok((0, [0; 16]));
            }
            let count = r.container(4)?;
            let mut member = [0; 16];
            for n in 0..count {
                let value = r.fixed()?;
                if n == index {
                    member = value;
                }
            }
            Ok((count, member))
        };
        match parse() {
            Ok((count, member)) => {
                unsafe { *out_count = count };
                if out_member.is_null() {
                    return RsHandheldStatus::Ok;
                }
                if index >= count {
                    return RsHandheldStatus::ErrCapacity;
                }
                unsafe { *out_member = member };
                RsHandheldStatus::Ok
            }
            Err(e) => status(e),
        }
    })
}

/// Extract `hub` from a validated RRC announce's CBOR app data; unknown fields
/// are skipped within the same bounded parser. Missing name is an empty span.
/// # Safety
/// Input readable; output writable, non-aliasing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rrc_announce_name(
    data: *const u8,
    length: usize,
    out: *mut RrcSpan,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || length > MDU {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts(data, length) };
        let parse = || -> Result<RrcSpan> {
            let mut r = Reader::new(bytes);
            let count = r.container(5)?;
            let mut result = RrcSpan::default();
            let mut seen = false;
            for _ in 0..count {
                let key = r.span(3, 64)?;
                if &bytes[key.offset as usize..key.offset as usize + key.length as usize] == b"hub"
                {
                    if seen {
                        return Err(Error::Invalid);
                    }
                    seen = true;
                    result = r.span(3, 64)?;
                } else {
                    r.skip(1)?;
                }
            }
            if r.pos != length {
                return Err(Error::Invalid);
            }
            Ok(result)
        };
        match parse() {
            Ok(value) => {
                unsafe { *out = value };
                RsHandheldStatus::Ok
            }
            Err(e) => status(e),
        }
    })
}
