//! Caller-owned live voice state. No codec/PCM allocations occur in the bridge.
use super::*;
use lxst_embedded::{
    CallRole, EndReason, Event, Profile, ProfileSet, Session, SessionConfig, Signal,
    TelephonyAction,
};
use zeroize::Zeroize;

const CODEC_MAGIC: u32 = 0x56434431;
const SESSION_MAGIC: u32 = 0x56535331;
#[repr(C)]
struct CodecStorage {
    native: MaybeUninit<lxst_codec2::Codec>,
    profile: u32,
    failed: u32,
    magic: u32,
}
#[repr(C)]
struct SessionStorage {
    session: MaybeUninit<Session>,
    magic: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RsHandheldVoiceEvent {
    pub kind: u32,
    pub value: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RsHandheldVoiceResult {
    pub audio_generation: u32,
    pub status: u32,
    pub profile: u32,
    pub ended: u32, // 0 = live; otherwise EndReason + 1.
    pub flags: u32, // bit 0 media-ready, bit 1 capturing, bit 2 playback-admitted.
    pub event_count: u32,
    pub events: [RsHandheldVoiceEvent; 8],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RsHandheldVoiceFrame {
    pub offset: u16,
    pub length: u16,
    pub codec: u8,
    pub reserved: [u8; 3],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RsHandheldVoicePacket {
    pub signal_count: u32,
    pub frame_count: u32,
    pub signals: [u32; 8],
    pub frames: [RsHandheldVoiceFrame; 4],
}
const _: () = {
    assert!(core::mem::size_of::<RsHandheldVoiceResult>() == 88);
    assert!(core::mem::size_of::<RsHandheldVoicePacket>() == 72);
    assert!(core::mem::size_of::<CodecStorage>() <= 32768);
    assert!(core::mem::size_of::<SessionStorage>() <= 160);
};
fn aligned<T>(pointer: *const u8) -> bool {
    !pointer.is_null() && (pointer as usize).is_multiple_of(core::mem::align_of::<T>())
}
fn profile_mode(value: u32) -> Option<(Profile, lxst_codec2::Mode)> {
    match value {
        0x20 => Some((Profile::BandwidthVeryLow, lxst_codec2::Mode::Rate1600)),
        0x30 => Some((Profile::BandwidthLow, lxst_codec2::Mode::Rate3200)),
        _ => None,
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_voice_codec_size() -> usize {
    core::mem::size_of::<CodecStorage>()
}
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_voice_codec_align() -> usize {
    core::mem::align_of::<CodecStorage>()
}
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_voice_session_size() -> usize {
    core::mem::size_of::<SessionStorage>()
}
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_voice_session_align() -> usize {
    core::mem::align_of::<SessionStorage>()
}

/// # Safety
/// `storage` owns `capacity` aligned writable bytes, with no concurrent access.
/// Initialize in its final allocation. Reinitialization discards all predictors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_init(
    storage: *mut u8,
    capacity: usize,
    profile: u32,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<CodecStorage>(storage) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if capacity < rs_handheld_voice_codec_size() {
            return RsHandheldStatus::ErrCapacity;
        }
        let Some((_, mode)) = profile_mode(profile) else {
            return RsHandheldStatus::ErrUnsupported;
        };
        let state = storage.cast::<CodecStorage>();
        // SAFETY: caller-owned final storage. Fields are initialized separately
        // so the large native state never exists as a by-value temporary.
        unsafe {
            addr_of_mut!((*state).magic).write(0);
            lxst_codec2::Codec::initialise(&mut *addr_of_mut!((*state).native), mode);
            addr_of_mut!((*state).profile).write(profile);
            addr_of_mut!((*state).failed).write(0);
            addr_of_mut!((*state).magic).write(CODEC_MAGIC);
        }
        RsHandheldStatus::Ok
    })
}
/// # Safety
/// `storage` is initialized live codec storage; PCM and output regions are valid
/// for their counts, aligned, writable as appropriate and mutually disjoint.
/// Read `out_len` only on success; a codec failure requires reinitialization.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_encode(
    storage: *mut u8,
    pcm: *const i16,
    samples: usize,
    out: *mut u8,
    capacity: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<CodecStorage>(storage) || pcm.is_null() || out.is_null() || out_len.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: caller provides initialized codec storage and disjoint buffers.
        let state = unsafe { &mut *storage.cast::<CodecStorage>() };
        if state.magic != CODEC_MAGIC || state.failed != 0 {
            return RsHandheldStatus::ErrNotReady;
        }
        let Some((profile, _)) = profile_mode(state.profile) else {
            return RsHandheldStatus::ErrNotReady;
        };
        if samples != profile.sample_frames_per_packet() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = if profile == Profile::BandwidthVeryLow {
            65
        } else {
            81
        };
        if capacity < bytes {
            return RsHandheldStatus::ErrCapacity;
        }
        let native = unsafe { state.native.assume_init_mut() };
        let mut codec = lxst_embedded::Codec2PacketCodec::new(profile, native).unwrap();
        match codec.encode_into(
            unsafe { core::slice::from_raw_parts(pcm, samples) },
            unsafe { core::slice::from_raw_parts_mut(out, bytes) },
        ) {
            Ok(length) => {
                unsafe { *out_len = length };
                RsHandheldStatus::Ok
            }
            Err(_) => {
                state.failed = 1;
                RsHandheldStatus::ErrInternal
            }
        }
    })
}
/// # Safety
/// Same exclusive storage/buffer contract as encode. Payload includes the mode
/// byte. Wrong length/mode/capacity leaves codec state and output unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_decode(
    storage: *mut u8,
    payload: *const u8,
    length: usize,
    pcm: *mut i16,
    capacity: usize,
    out_samples: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<CodecStorage>(storage)
            || payload.is_null()
            || pcm.is_null()
            || out_samples.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let state = unsafe { &mut *storage.cast::<CodecStorage>() };
        if state.magic != CODEC_MAGIC || state.failed != 0 {
            return RsHandheldStatus::ErrNotReady;
        }
        let Some((profile, _)) = profile_mode(state.profile) else {
            return RsHandheldStatus::ErrNotReady;
        };
        let (bytes, mode) = if profile == Profile::BandwidthVeryLow {
            (65, 4)
        } else {
            (81, 6)
        };
        if length != bytes || unsafe { *payload } != mode {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let samples = profile.sample_frames_per_packet();
        if capacity < samples {
            return RsHandheldStatus::ErrCapacity;
        }
        let native = unsafe { state.native.assume_init_mut() };
        let mut codec = lxst_embedded::Codec2PacketCodec::new(profile, native).unwrap();
        match codec.decode_into(
            unsafe { core::slice::from_raw_parts(payload, length) },
            unsafe { core::slice::from_raw_parts_mut(pcm, samples) },
        ) {
            Ok(length) => {
                unsafe { *out_samples = length };
                RsHandheldStatus::Ok
            }
            Err(_) => {
                state.failed = 1;
                RsHandheldStatus::ErrInternal
            }
        }
    })
}
/// Encode one native frame (160/320 samples to 8 bytes), without a mode header.
/// The worker aggregates exactly the negotiated profile's frame count.
/// # Safety
/// Same exclusive initialized storage and disjoint buffer contract as encode.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_encode_frame(
    storage: *mut u8,
    pcm: *const i16,
    samples: usize,
    out: *mut u8,
    capacity: usize,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<CodecStorage>(storage) || pcm.is_null() || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let state = unsafe { &mut *storage.cast::<CodecStorage>() };
        if state.magic != CODEC_MAGIC || state.failed != 0 {
            return RsHandheldStatus::ErrNotReady;
        }
        let native = unsafe { state.native.assume_init_mut() };
        if samples != native.mode().samples() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if capacity < native.mode().bytes() {
            return RsHandheldStatus::ErrCapacity;
        }
        match native.encode(
            unsafe { core::slice::from_raw_parts(pcm, samples) },
            unsafe { core::slice::from_raw_parts_mut(out, 8) },
        ) {
            Ok(()) => RsHandheldStatus::Ok,
            Err(_) => {
                state.failed = 1;
                RsHandheldStatus::ErrInternal
            }
        }
    })
}
/// Decode one native eight-byte frame to the negotiated 160/320 sample window.
/// # Safety
/// Same exclusive initialized storage and disjoint buffer contract as decode.
/// Caller validates the aggregate packet's codec, mode and full length first.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_decode_frame(
    storage: *mut u8,
    payload: *const u8,
    length: usize,
    pcm: *mut i16,
    capacity: usize,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<CodecStorage>(storage) || payload.is_null() || pcm.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let state = unsafe { &mut *storage.cast::<CodecStorage>() };
        if state.magic != CODEC_MAGIC || state.failed != 0 {
            return RsHandheldStatus::ErrNotReady;
        }
        let native = unsafe { state.native.assume_init_mut() };
        if length != 8 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let samples = native.mode().samples();
        if capacity < samples {
            return RsHandheldStatus::ErrCapacity;
        }
        match native.decode(
            unsafe { core::slice::from_raw_parts(payload, length) },
            unsafe { core::slice::from_raw_parts_mut(pcm, samples) },
        ) {
            Ok(()) => RsHandheldStatus::Ok,
            Err(_) => {
                state.failed = 1;
                RsHandheldStatus::ErrInternal
            }
        }
    })
}
/// # Safety
/// `storage` is exclusive aligned writable storage of at least codec_size bytes.
/// No encoder/decoder may retain access. Clears PCM histories before owner free.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_codec_clear(storage: *mut u8) {
    if aligned::<CodecStorage>(storage) {
        // SAFETY: no Drop resources; MaybeUninit admits every byte pattern.
        unsafe {
            core::slice::from_raw_parts_mut(storage, rs_handheld_voice_codec_size()).zeroize()
        };
    }
}
fn end_value(reason: EndReason) -> u32 {
    match reason {
        EndReason::Local => 1,
        EndReason::Remote => 2,
        EndReason::Rejected => 3,
        EndReason::Busy => 4,
        EndReason::Timeout => 5,
        EndReason::ProfileUnsupported => 6,
        EndReason::AudioUnavailable => 7,
        EndReason::RouteLost => 8,
    }
}
fn end_reason(value: u32) -> Option<EndReason> {
    Some(match value {
        1 => EndReason::Local,
        2 => EndReason::Remote,
        3 => EndReason::Rejected,
        4 => EndReason::Busy,
        5 => EndReason::Timeout,
        6 => EndReason::ProfileUnsupported,
        7 => EndReason::AudioUnavailable,
        8 => EndReason::RouteLost,
        _ => return None,
    })
}
fn event_view(event: &Event) -> RsHandheldVoiceEvent {
    let (kind, value) = match event {
        Event::Capture(on) => (20, u32::from(*on)),
        Event::FlushMedia => (21, 0),
        Event::Ended(reason) => (22, end_value(*reason)),
        Event::Transition(action) => match action {
            TelephonyAction::SendSignal(signal) => (1, signal.wire_value()),
            TelephonyAction::IdentifyLocalIdentity => (2, 0),
            TelephonyAction::SelectProfile(profile) => (3, profile.wire_value()),
            TelephonyAction::PrepareDialingPipelines => (4, 0),
            TelephonyAction::ResetDialingPipelines => (5, 0),
            TelephonyAction::OpenAudioPipelines => (6, 0),
            TelephonyAction::StartAudioPipelines => (7, 0),
            TelephonyAction::StartDialTone => (8, 0),
            TelephonyAction::Terminate(status) => (9, status.map_or(u32::MAX, |v| v.wire_value())),
            TelephonyAction::TeardownLink => (10, 0),
            TelephonyAction::RingIncomingCall => (11, 0),
            TelephonyAction::SwitchProfile(profile) => (12, profile.wire_value()),
            TelephonyAction::IgnoreSignal(signal) => (13, signal.wire_value()),
        },
    };
    RsHandheldVoiceEvent { kind, value }
}
/// # Safety
/// Exclusive final storage with the queried size/alignment. The embedding owner
/// has already admitted local device directions, codec and the exact route.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_session_init(
    storage: *mut u8,
    capacity: usize,
    incoming: u32,
    preferred: u32,
    allowed: u32,
    capture: u32,
    playback: u32,
    now_ms: u64,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<SessionStorage>(storage)
            || incoming > 1
            || capture > 1
            || playback > 1
            || allowed == 0
            || allowed & !3 != 0
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if capacity < rs_handheld_voice_session_size() {
            return RsHandheldStatus::ErrCapacity;
        }
        let Some((preferred, _)) = profile_mode(preferred) else {
            return RsHandheldStatus::ErrUnsupported;
        };
        let profiles = match allowed {
            1 => ProfileSet::only(Profile::BandwidthVeryLow),
            2 => ProfileSet::only(Profile::BandwidthLow),
            _ => ProfileSet::CODEC2,
        };
        let session = match Session::new(
            if incoming == 1 {
                CallRole::Incoming
            } else {
                CallRole::Outgoing
            },
            SessionConfig {
                allowed: profiles,
                preferred,
                capture: capture == 1,
                playback: playback == 1,
            },
            now_ms,
        ) {
            Ok(value) => value,
            Err(_) => return RsHandheldStatus::ErrUnsupported,
        };
        // SAFETY: all fields are initialized in caller storage before publication.
        unsafe {
            storage.cast::<SessionStorage>().write(SessionStorage {
                session: MaybeUninit::new(session),
                magic: SESSION_MAGIC,
            })
        };
        RsHandheldStatus::Ok
    })
}
/// # Safety
/// Live exclusive initialized session storage and a disjoint writable result.
/// PeerVerified is only authorized after cryptographic Link identity validation;
/// callers fence every operation by session/view/interface generation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_session_apply(
    storage: *mut u8,
    operation: u32,
    argument: u32,
    extra: u32,
    now_ms: u64,
    out: *mut RsHandheldVoiceResult,
) -> RsHandheldStatus {
    guard(|| {
        if !aligned::<SessionStorage>(storage) || out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let owner = unsafe { &mut *storage.cast::<SessionStorage>() };
        if owner.magic != SESSION_MAGIC {
            return RsHandheldStatus::ErrNotReady;
        }
        let session = unsafe { owner.session.assume_init_mut() };
        let events = match operation {
            1 => session.link_established(),
            2 => session.peer_verified(now_ms),
            3 => session.answer(now_ms),
            4 => session.receive_signal(Signal::from_wire(argument), now_ms),
            5 if extra <= 1 => session.audio_ready(argument, extra == 1),
            6 if argument <= 1 => session.set_transmitting(argument == 1, now_ms),
            7 => session.tick(now_ms),
            9 => match Profile::from_wire(argument) {
                Some(profile) => session.receive_profile_media(profile, now_ms),
                None => return RsHandheldStatus::ErrInvalidArg,
            },
            8 => match end_reason(argument) {
                Some(reason) => session.end(reason),
                None => return RsHandheldStatus::ErrInvalidArg,
            },
            _ => return RsHandheldStatus::ErrInvalidArg,
        };
        let mut result = RsHandheldVoiceResult {
            audio_generation: session.audio_generation(),
            status: session.status().wire_value(),
            profile: session.profile().map_or(0, |p| p.wire_value()),
            ended: session.ended().map_or(0, end_value),
            flags: u32::from(session.media_ready())
                | (u32::from(session.transmitting()) << 1)
                | (u32::from(session.playback_allowed()) << 2),
            event_count: events.as_slice().len() as u32,
            ..Default::default()
        };
        for (out, event) in result.events.iter_mut().zip(events.as_slice()) {
            *out = event_view(event);
        }
        unsafe { *out = result };
        RsHandheldStatus::Ok
    })
}
/// # Safety
/// `data` reads `length` bytes, `out` is a disjoint writable packet view. Views
/// borrow the input through offsets; the caller copies media before returning.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_packet_view(
    data: *const u8,
    length: usize,
    out: *mut RsHandheldVoicePacket,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || length > lxst_embedded::wire::MAX_PACKET_BYTES {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts(data, length) };
        let packet = match lxst_embedded::wire::Packet::decode(bytes) {
            Ok(value) => value,
            Err(_) => return RsHandheldStatus::ErrInvalidArg,
        };
        let mut view = RsHandheldVoicePacket {
            signal_count: packet.signals().len() as u32,
            frame_count: packet.frames().len() as u32,
            ..Default::default()
        };
        for (target, signal) in view.signals.iter_mut().zip(packet.signals()) {
            *target = signal.wire_value();
        }
        for (target, frame) in view.frames.iter_mut().zip(packet.frames()) {
            *target = RsHandheldVoiceFrame {
                offset: (frame.payload.as_ptr() as usize - data as usize) as u16,
                length: frame.payload.len() as u16,
                codec: frame.codec.wire_id(),
                reserved: [0; 3],
            };
        }
        unsafe { *out = view };
        RsHandheldStatus::Ok
    })
}
/// # Safety
/// Input slices and required outputs are valid, aligned and mutually disjoint.
/// A null signal/payload pointer is permitted only with a zero count/length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_voice_packet_encode(
    signals: *const u32,
    count: usize,
    codec: u32,
    payload: *const u8,
    length: usize,
    out: *mut u8,
    capacity: usize,
    out_len: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if out.is_null()
            || out_len.is_null()
            || count > 8
            || (count != 0 && signals.is_null())
            || (length != 0 && payload.is_null())
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if length >= lxst_embedded::wire::MAX_FRAME_BYTES {
            return RsHandheldStatus::ErrCapacity;
        }
        let mut values = [Signal::Raw(0); 8];
        if count != 0 {
            for (value, signal) in values
                .iter_mut()
                .zip(unsafe { core::slice::from_raw_parts(signals, count) })
            {
                *value = Signal::from_wire(*signal);
            }
        }
        let codec = match u8::try_from(codec)
            .ok()
            .and_then(|v| lxst_embedded::CodecKind::from_wire(v).ok())
        {
            Some(value) => value,
            None => return RsHandheldStatus::ErrUnsupported,
        };
        let bytes = if length == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(payload, length) }
        };
        let frames = [lxst_embedded::wire::Frame {
            codec,
            payload: bytes,
        }];
        let frames = if length == 0 {
            &frames[..0]
        } else {
            &frames[..]
        };
        match lxst_embedded::wire::encode(&values[..count], frames, unsafe {
            core::slice::from_raw_parts_mut(
                out,
                capacity.min(lxst_embedded::wire::MAX_PACKET_BYTES),
            )
        }) {
            Ok(length) => {
                unsafe { *out_len = length };
                RsHandheldStatus::Ok
            }
            Err(lxst_embedded::wire::Error::OutputCapacity) => RsHandheldStatus::ErrCapacity,
            Err(_) => RsHandheldStatus::ErrInvalidArg,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{Layout, alloc_zeroed, dealloc};
    struct Storage {
        pointer: *mut u8,
        layout: Layout,
    }
    impl Storage {
        fn new(size: usize, alignment: usize) -> Self {
            let layout = Layout::from_size_align(size, alignment).unwrap();
            let pointer = unsafe { alloc_zeroed(layout) };
            assert!(!pointer.is_null());
            Self { pointer, layout }
        }
    }
    impl Drop for Storage {
        fn drop(&mut self) {
            unsafe { dealloc(self.pointer, self.layout) };
        }
    }
    #[test]
    fn streamed_native_frames_match_whole_packet_and_preserve_error_outputs() {
        for (profile, count, native) in [(0x20, 2560, 320), (0x30, 1600, 160)] {
            let batch = Storage::new(
                rs_handheld_voice_codec_size(),
                rs_handheld_voice_codec_align(),
            );
            let stream = Storage::new(
                rs_handheld_voice_codec_size(),
                rs_handheld_voice_codec_align(),
            );
            let pcm: Vec<i16> = (0..count)
                .map(|n| ((n % 81) as i32 * 300 - 12000) as i16)
                .collect();
            let mut expected = [0; 81];
            let mut actual = [0; 81];
            let mut length = 0;
            unsafe {
                assert_eq!(
                    rs_handheld_voice_codec_init(batch.pointer, batch.layout.size(), profile),
                    RsHandheldStatus::Ok
                );
                assert_eq!(
                    rs_handheld_voice_codec_init(stream.pointer, stream.layout.size(), profile),
                    RsHandheldStatus::Ok
                );
                assert_eq!(
                    rs_handheld_voice_codec_encode(
                        batch.pointer,
                        pcm.as_ptr(),
                        count,
                        expected.as_mut_ptr(),
                        81,
                        &mut length
                    ),
                    RsHandheldStatus::Ok
                );
                for (index, frame) in pcm.chunks_exact(native).enumerate() {
                    assert_eq!(
                        rs_handheld_voice_codec_encode_frame(
                            stream.pointer,
                            frame.as_ptr(),
                            native - 1,
                            actual.as_mut_ptr().add(1 + index * 8),
                            8
                        ),
                        RsHandheldStatus::ErrInvalidArg
                    );
                    assert_eq!(
                        rs_handheld_voice_codec_encode_frame(
                            stream.pointer,
                            frame.as_ptr(),
                            native,
                            actual.as_mut_ptr().add(1 + index * 8),
                            8
                        ),
                        RsHandheldStatus::Ok
                    );
                }
                actual[0] = expected[0];
                assert_eq!(&actual[..length], &expected[..length]);
                let mut decoded = vec![0; count];
                let mut out_count = 0;
                assert_eq!(
                    rs_handheld_voice_codec_decode(
                        batch.pointer,
                        expected.as_ptr(),
                        length,
                        decoded.as_mut_ptr(),
                        count,
                        &mut out_count
                    ),
                    RsHandheldStatus::Ok
                );
                let mut streamed = vec![123; count];
                for (index, frame) in streamed.chunks_exact_mut(native).enumerate() {
                    assert_eq!(
                        rs_handheld_voice_codec_decode_frame(
                            stream.pointer,
                            actual.as_ptr().add(1 + index * 8),
                            7,
                            frame.as_mut_ptr(),
                            native
                        ),
                        RsHandheldStatus::ErrInvalidArg
                    );
                    assert!(frame.iter().all(|v| *v == 123));
                    assert_eq!(
                        rs_handheld_voice_codec_decode_frame(
                            stream.pointer,
                            actual.as_ptr().add(1 + index * 8),
                            8,
                            frame.as_mut_ptr(),
                            native - 1
                        ),
                        RsHandheldStatus::ErrCapacity
                    );
                    assert_eq!(
                        rs_handheld_voice_codec_decode_frame(
                            stream.pointer,
                            actual.as_ptr().add(1 + index * 8),
                            8,
                            frame.as_mut_ptr(),
                            native
                        ),
                        RsHandheldStatus::Ok
                    );
                }
                assert_eq!(decoded, streamed);
                rs_handheld_voice_codec_clear(batch.pointer);
                rs_handheld_voice_codec_clear(stream.pointer);
            }
        }
    }
    #[test]
    fn voice_codec_ffi_checks_admission_and_roundtrips_profiles_in_final_storage() {
        let storage = Storage::new(
            rs_handheld_voice_codec_size(),
            rs_handheld_voice_codec_align(),
        );
        let mut packet = [0xa5; 90];
        let mut length = 999;
        let input = [0_i16; 2560];
        let mut output = [123_i16; 2568];
        let mut samples = 999;
        unsafe {
            assert_eq!(
                rs_handheld_voice_codec_init(storage.pointer.add(1), storage.layout.size(), 0x20),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!(
                rs_handheld_voice_codec_init(storage.pointer, storage.layout.size() - 1, 0x20),
                RsHandheldStatus::ErrCapacity
            );
            assert_eq!(
                rs_handheld_voice_codec_init(storage.pointer, storage.layout.size(), 0x10),
                RsHandheldStatus::ErrUnsupported
            );
            for (profile, count, bytes) in [(0x20, 2560, 65), (0x30, 1600, 81)] {
                assert_eq!(
                    rs_handheld_voice_codec_init(storage.pointer, storage.layout.size(), profile),
                    RsHandheldStatus::Ok
                );
                assert_eq!(
                    rs_handheld_voice_codec_encode(
                        storage.pointer,
                        input.as_ptr(),
                        count - 1,
                        packet.as_mut_ptr(),
                        packet.len(),
                        &mut length
                    ),
                    RsHandheldStatus::ErrInvalidArg
                );
                assert_eq!(
                    rs_handheld_voice_codec_encode(
                        storage.pointer,
                        input.as_ptr(),
                        count,
                        packet.as_mut_ptr(),
                        bytes - 1,
                        &mut length
                    ),
                    RsHandheldStatus::ErrCapacity
                );
                assert_eq!(
                    rs_handheld_voice_codec_encode(
                        storage.pointer,
                        input.as_ptr(),
                        count,
                        packet.as_mut_ptr(),
                        packet.len(),
                        &mut length
                    ),
                    RsHandheldStatus::Ok
                );
                assert_eq!(length, bytes);
                assert_eq!(
                    rs_handheld_voice_codec_decode(
                        storage.pointer,
                        packet.as_ptr(),
                        length - 1,
                        output.as_mut_ptr(),
                        output.len(),
                        &mut samples
                    ),
                    RsHandheldStatus::ErrInvalidArg
                );
                assert_eq!(
                    rs_handheld_voice_codec_decode(
                        storage.pointer,
                        packet.as_ptr(),
                        length,
                        output.as_mut_ptr(),
                        output.len(),
                        &mut samples
                    ),
                    RsHandheldStatus::Ok
                );
                assert_eq!(samples, count);
                output.fill(123);
                packet.fill(0xa5);
                rs_handheld_voice_codec_clear(storage.pointer);
                assert!(
                    core::slice::from_raw_parts(storage.pointer, storage.layout.size())
                        .iter()
                        .all(|b| *b == 0)
                );
                assert_eq!(
                    rs_handheld_voice_codec_encode(
                        storage.pointer,
                        input.as_ptr(),
                        count,
                        packet.as_mut_ptr(),
                        packet.len(),
                        &mut length
                    ),
                    RsHandheldStatus::ErrNotReady
                );
            }
        }
    }
    #[test]
    fn voice_packet_ffi_retains_bounded_offsets_and_outputs_on_failure() {
        let signals = [3, 4, 0x11f];
        let payload = [4; 65];
        let mut packet = [0xa5; 128];
        let mut length = 999;
        let mut view = RsHandheldVoicePacket::default();
        unsafe {
            assert_eq!(
                rs_handheld_voice_packet_encode(
                    signals.as_ptr(),
                    signals.len(),
                    2,
                    payload.as_ptr(),
                    payload.len(),
                    packet.as_mut_ptr(),
                    4,
                    &mut length
                ),
                RsHandheldStatus::ErrCapacity
            );
            assert_eq!(length, 999);
            assert_eq!(packet, [0xa5; 128]);
            assert_eq!(
                rs_handheld_voice_packet_encode(
                    signals.as_ptr(),
                    signals.len(),
                    2,
                    payload.as_ptr(),
                    payload.len(),
                    packet.as_mut_ptr(),
                    packet.len(),
                    &mut length
                ),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                rs_handheld_voice_packet_view(packet.as_ptr(), length, &mut view),
                RsHandheldStatus::Ok
            );
            assert_eq!(view.signal_count, 3);
            assert_eq!(&view.signals[..3], &signals);
            assert_eq!(view.frame_count, 1);
            let frame = view.frames[0];
            assert_eq!(frame.codec, 2);
            assert_eq!(
                &packet[frame.offset as usize..(frame.offset + frame.length) as usize],
                &payload
            );
            view.signal_count = 999;
            assert_eq!(
                rs_handheld_voice_packet_view(packet.as_ptr(), length - 1, &mut view),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!(view.signal_count, 999);
        }
    }
    #[test]
    fn voice_session_ffi_requires_verified_answer_and_press_and_preserves_event_order() {
        let storage = Storage::new(
            rs_handheld_voice_session_size(),
            rs_handheld_voice_session_align(),
        );
        let mut result = RsHandheldVoiceResult::default();
        unsafe {
            assert_eq!(
                rs_handheld_voice_session_init(
                    storage.pointer,
                    storage.layout.size(),
                    1,
                    0x20,
                    1,
                    1,
                    1,
                    0
                ),
                RsHandheldStatus::Ok
            );
            for (op, arg, extra) in [(1, 0, 0), (2, 0, 0), (3, 0, 0), (5, 1, 1), (4, 6, 0)] {
                assert_eq!(
                    rs_handheld_voice_session_apply(
                        storage.pointer,
                        op,
                        arg,
                        extra,
                        1,
                        &mut result
                    ),
                    RsHandheldStatus::Ok
                );
                assert_eq!(result.flags & 2, 0);
            }
            assert_eq!(result.flags & 1, 1);
            assert_eq!(
                rs_handheld_voice_session_apply(storage.pointer, 6, 1, 0, 2, &mut result),
                RsHandheldStatus::Ok
            );
            assert_eq!(result.flags & 2, 2);
            assert_eq!(result.events[1].kind, 20);
            assert_eq!(result.events[1].value, 1);
            assert_eq!(
                rs_handheld_voice_session_apply(storage.pointer, 8, 8, 0, 3, &mut result),
                RsHandheldStatus::Ok
            );
            assert_eq!(result.events[0].kind, 20);
            assert_eq!(result.events[0].value, 0);
            assert_eq!(result.ended, 8);
            assert_eq!(result.flags, 0);
        }
    }
}
