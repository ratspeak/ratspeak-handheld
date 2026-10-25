use super::*;

struct Context(*mut RsHandheldRns);
impl Context {
    fn new(byte: u8) -> Self {
        let mut ctx = core::ptr::null_mut();
        unsafe {
            assert_eq!(rs_handheld_rns_init(&mut ctx), RsHandheldStatus::Ok);
            assert_eq!(
                rs_handheld_rns_load_identity(ctx, &[byte; 64]),
                RsHandheldStatus::Ok
            );
        }
        Self(ctx)
    }
}
impl Drop for Context {
    fn drop(&mut self) {
        unsafe { rs_handheld_rns_shutdown(self.0) }
    }
}
fn audio_message(ctx: &Context, bytes: &[u8]) -> Vec<u8> {
    let recipient = LocalIdentity::from_private_key(&[0x52; 64]);
    let mut output = vec![0; lxmf::LXMF_PACKED_MAX];
    let mut length = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build_audio(
                ctx.0,
                recipient.public_key(),
                1234567890.5,
                core::ptr::null(),
                0,
                b"Voice message".as_ptr(),
                13,
                RS_HANDHELD_MEMO_MODE,
                bytes.as_ptr(),
                bytes.len(),
                output.as_mut_ptr(),
                output.len(),
                &mut length,
                &mut [0; 16],
                &mut [0; 32],
            )
        },
        RsHandheldStatus::Ok
    );
    output.truncate(length);
    output
}

#[test]
fn clip_inspection_checks_mode_frame_boundaries_and_duration_without_mutating_errors() {
    let mut info = RsHandheldMemoInfo::default();
    for (length, frames, duration) in [(4, 1, 40), (1500, 375, 15000), (3000, 750, 30000)] {
        assert_eq!(
            unsafe { rs_handheld_memo_inspect(3, length, &mut info) },
            RsHandheldStatus::Ok
        );
        assert_eq!(
            info,
            RsHandheldMemoInfo {
                frames,
                duration_ms: duration,
                sample_rate: 8000,
                frame_samples: 320,
                frame_bytes: 4,
                codec_profile: 0x10
            }
        );
    }
    let saved = info;
    for (mode, length, error) in [
        (3, 0, RsHandheldStatus::ErrInvalidArg),
        (3, 3, RsHandheldStatus::ErrInvalidArg),
        (3, 3004, RsHandheldStatus::ErrCapacity),
        (0x10, 1500, RsHandheldStatus::ErrUnsupported),
        (3, usize::MAX - 3, RsHandheldStatus::ErrCapacity),
    ] {
        assert_eq!(
            unsafe { rs_handheld_memo_inspect(mode, length, &mut info) },
            error
        );
        assert_eq!(info, saved);
    }
    assert_eq!(
        unsafe { rs_handheld_memo_inspect(3, 4, core::ptr::null_mut()) },
        RsHandheldStatus::ErrInvalidArg
    );
}

#[test]
fn signed_media_view_matches_python_fixture_and_retains_input_relative_spans() {
    let tx = Context::new(0x51);
    let rx = Context::new(0x52);
    let audio: Vec<u8> = (0..1500).map(|i| (i % 251) as u8).collect();
    let output = audio_message(&tx, &audio);
    assert_eq!(
        output,
        include_bytes!("../../../rsLXMFLite/tests/fixtures/audio-700c.lxmf")
    );
    let source = LocalIdentity::from_private_key(&[0x51; 64]);
    let mut view = RsHandheldMediaView::default();
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_media_view(
                rx.0,
                output.as_ptr(),
                output.len(),
                source.public_key(),
                &mut view,
            )
        },
        RsHandheldStatus::Ok
    );
    let span = |off: u32, len: u32| &output[off as usize..(off + len) as usize];
    assert_eq!(view.audio.state, 1);
    assert_eq!(view.audio.mode, 3);
    assert_eq!(span(view.audio.offset, view.audio.length), audio);
    assert_eq!(
        span(view.message.content_offset, view.message.content_len),
        b"Voice message"
    );
    assert_eq!(view.message.source_hash, source.lxmf_delivery_hash());
    let prior = view.audio;
    let mut changed = output.clone();
    changed[view.audio.offset as usize] ^= 1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_media_view(
                rx.0,
                changed.as_ptr(),
                changed.len(),
                source.public_key(),
                &mut view,
            )
        },
        RsHandheldStatus::ErrCrypto
    );
    assert_eq!(view.audio, prior);
}

#[test]
fn audio_capacity_is_checked_before_body_access_and_scalars_are_not_published() {
    let tx = Context::new(0x51);
    let recipient = LocalIdentity::from_private_key(&[0x52; 64]);
    let mut size = 7;
    assert_eq!(
        unsafe { rs_handheld_lxmf_audio_packed_size(0, 13, 3, 1500, &mut size) },
        RsHandheldStatus::Ok
    );
    assert_eq!(size, 1630);
    assert_eq!(
        unsafe { rs_handheld_lxmf_audio_packed_size(0, 0, 3, usize::MAX, &mut size) },
        RsHandheldStatus::ErrCapacity
    );
    assert_eq!(size, 1630);
    let mut output = [0xa5; 16];
    let mut length = 9;
    let mut dest = [0xa5; 16];
    let mut mid = [0xa5; 32];
    for length_in in [1500, usize::MAX] {
        assert_eq!(
            unsafe {
                rs_handheld_rns_lxmf_build_audio(
                    tx.0,
                    recipient.public_key(),
                    1.0,
                    core::ptr::null(),
                    0,
                    core::ptr::null(),
                    0,
                    3,
                    b"x".as_ptr(),
                    length_in,
                    output.as_mut_ptr(),
                    output.len(),
                    &mut length,
                    &mut dest,
                    &mut mid,
                )
            },
            RsHandheldStatus::ErrCapacity
        );
        assert_eq!(length, 9);
        assert_eq!(dest, [0xa5; 16]);
        assert_eq!(mid, [0xa5; 32]);
        assert_eq!(output, [0xa5; 16]);
    }
}

#[test]
fn media_shape_errors_do_not_become_authentication_errors_or_audio_bytes() {
    for (fields, state) in [
        (&[0x80][..], 0),
        (&[0x81, 7, 0x92, 0x10, 0xc4, 0][..], 1),
        (&[0x81, 7, 0xc0][..], 2),
    ] {
        let view = LxmfView {
            message_id: [1; 32],
            source_hash: [2; 16],
            timestamp: 1.0,
            title: &fields[..0],
            content: &fields[..0],
            fields,
        };
        let media = media_view(view, fields.as_ptr() as usize, fields.len()).unwrap();
        assert_eq!(media.audio.state, state);
        if state == 2 {
            assert_eq!(media.audio.length, 0);
            assert_eq!(media.audio.offset, 0);
        }
    }
}

#[test]
fn opportunistic_media_spans_borrow_caller_scratch_and_bad_key_or_hint_publishes_nothing() {
    let tx = Context::new(0x51);
    let rx = Context::new(0x52);
    // 160ms fits opportunistic transport; such audio must not disappear on receive.
    let audio = [0x33; 16];
    let signed = audio_message(&tx, &audio);
    let source = LocalIdentity::from_private_key(&[0x51; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x52; 64]);
    let mut encrypted = [0; 500];
    let target = recipient.public_key()[..32].try_into().unwrap();
    let n = rns_lite_core::crypto::ecies_encrypt(
        &signed[16..],
        &target,
        recipient.identity_hash(),
        &[0x31; 32],
        &[0x32; 16],
        &mut encrypted,
    )
    .unwrap();
    let mut plain = [0; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
    let mut view = RsHandheldMediaView::default();
    let wrong = LocalIdentity::from_private_key(&[0x53; 64]);
    for (key, hint, error) in [
        (
            wrong.public_key(),
            RS_HANDHELD_LXMF_BASE_KEY_HINT,
            RsHandheldStatus::ErrCrypto,
        ),
        (source.public_key(), 64, RsHandheldStatus::ErrCrypto),
    ] {
        assert_eq!(
            unsafe {
                rs_handheld_rns_lxmf_parse_hint_media(
                    rx.0,
                    encrypted.as_ptr(),
                    n,
                    hint,
                    key,
                    plain.as_mut_ptr(),
                    plain.len(),
                    &mut view,
                )
            },
            error
        );
        assert_eq!(view.audio, RsHandheldAudioView::default());
        assert_eq!(view.message.message_id, [0; 32]);
    }
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_hint_media(
                rx.0,
                encrypted.as_ptr(),
                n,
                RS_HANDHELD_LXMF_BASE_KEY_HINT,
                source.public_key(),
                plain.as_mut_ptr(),
                plain.len(),
                &mut view,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(view.audio.state, 1);
    assert_eq!(
        &plain[view.audio.offset as usize..(view.audio.offset + view.audio.length) as usize],
        audio
    );
    assert_eq!(
        &plain[view.message.content_offset as usize
            ..(view.message.content_offset + view.message.content_len) as usize],
        b"Voice message"
    );
}

#[test]
fn opportunistic_media_uses_only_the_peeked_retained_ratchet() {
    let tx = Context::new(0x51);
    let rx = Context::new(0x52);
    let source = LocalIdentity::from_private_key(&[0x51; 64]);
    let recipient = LocalIdentity::from_private_key(&[0x52; 64]);
    let (old, _) = test_persist_commit_ratchet(rx.0, &[0x71; 32], 1000, 0);
    test_persist_commit_ratchet(
        rx.0,
        &[0x72; 32],
        1000 + rns_lite_core::ratchet::RATCHET_INTERVAL_SECS,
        0,
    );
    let audio = [0x44; 16];
    let signed = audio_message(&tx, &audio);
    let mut encrypted = [0; 500];
    let n = rns_lite_core::crypto::ecies_encrypt(
        &signed[16..],
        &old,
        recipient.identity_hash(),
        &[0x31; 32],
        &[0x32; 16],
        &mut encrypted,
    )
    .unwrap();
    let mut hint = RS_HANDHELD_LXMF_BASE_KEY_HINT;
    let mut hash = [0; 16];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_peek_source_hint(rx.0, encrypted.as_ptr(), n, &mut hash, &mut hint)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(hash, source.lxmf_delivery_hash());
    assert_eq!(hint, 1);
    let mut plain = [0; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
    let mut view = RsHandheldMediaView::default();
    for incorrect in [0, 63, 254, RS_HANDHELD_LXMF_BASE_KEY_HINT] {
        assert_eq!(
            unsafe {
                rs_handheld_rns_lxmf_parse_hint_media(
                    rx.0,
                    encrypted.as_ptr(),
                    n,
                    incorrect,
                    source.public_key(),
                    plain.as_mut_ptr(),
                    plain.len(),
                    &mut view,
                )
            },
            RsHandheldStatus::ErrCrypto
        );
        assert_eq!(view.message.message_id, [0; 32]);
    }
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_hint_media(
                rx.0,
                encrypted.as_ptr(),
                n,
                hint,
                source.public_key(),
                plain.as_mut_ptr(),
                plain.len(),
                &mut view,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        &plain[view.audio.offset as usize..(view.audio.offset + view.audio.length) as usize],
        audio
    );
}
