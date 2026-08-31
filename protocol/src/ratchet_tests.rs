use super::*;

fn ctx_with_identity(prv: &[u8; PRIVATE_KEY_LENGTH]) -> *mut RsHandheldRns {
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(ctx, prv) },
        RsHandheldStatus::Ok
    );
    ctx
}

fn announce_status(
    ctx: *mut RsHandheldRns,
    announce_order: u64,
    ratchet: *const [u8; RATCHET_LEN],
) -> RsHandheldStatus {
    let mut payload = [0u8; 600];
    let mut payload_len = 0usize;
    let mut destination = [0u8; DESTINATION_LENGTH];
    unsafe {
        rs_handheld_rns_announce(
            ctx,
            &[0xA5; RANDOM_SEED_LEN],
            announce_order,
            ratchet,
            core::ptr::null(),
            0,
            payload.as_mut_ptr(),
            payload.len(),
            &mut payload_len,
            &mut destination,
        )
    }
}

#[test]
fn persistence_transactions_gate_every_advertised_ratchet() {
    let ctx = ctx_with_identity(&[0x70; PRIVATE_KEY_LENGTH]);
    let mut ratchet_blob = [0u8; RS_HANDHELD_RATCHET_RING_BLOB_MAX];
    let mut ratchet_len = 0usize;
    let mut ratchet_action = -1;
    let mut candidate = [0u8; RATCHET_LEN];
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_prepare(
                ctx,
                &[0x71; RATCHET_LEN],
                1_000,
                0,
                ratchet_blob.as_mut_ptr(),
                ratchet_blob.len(),
                &mut ratchet_len,
                &mut ratchet_action,
                &mut candidate,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ratchet_action, RATCHET_PREP_ROTATED);
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_current(ctx, &mut [0u8; RATCHET_LEN]) },
        RsHandheldStatus::ErrNotReady,
        "prepare must not install secret key material"
    );

    let mut state_blob = [0u8; RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX];
    let mut state_len = 0usize;
    let mut wire_value = 0u64;
    let mut ready = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_state_prepare(
                ctx,
                1_000,
                state_blob.as_mut_ptr(),
                state_blob.len(),
                &mut state_len,
                &mut wire_value,
                &mut ready,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!((wire_value, ready), (1_000, 1));

    // A failed durable write is represented by abandoning prepared bytes. Base-key
    // availability remains, but the new public ratchet cannot be put on the wire.
    assert_eq!(
        announce_status(ctx, wire_value, core::ptr::null()),
        RsHandheldStatus::Ok
    );
    assert_eq!(
        announce_status(ctx, wire_value, &candidate),
        RsHandheldStatus::ErrNotReady
    );

    // Persisting only the ordering state is still insufficient: the matching private key
    // must have been committed too.
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_state_commit(
                ctx,
                state_blob.as_ptr(),
                state_len,
                &mut wire_value,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        announce_status(ctx, wire_value, &candidate),
        RsHandheldStatus::ErrNotReady
    );

    let mut tampered = ratchet_blob;
    tampered[ratchet_len - 1] ^= 0x80;
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_commit(ctx, tampered.as_ptr(), ratchet_len, &mut candidate)
        },
        RsHandheldStatus::ErrCrypto
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_commit(ctx, ratchet_blob.as_ptr(), ratchet_len, &mut candidate)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        announce_status(ctx, wire_value, &candidate),
        RsHandheldStatus::Ok
    );

    // A second prepare supersedes the first pending transaction. Neither candidate changes
    // the live key until the exact most-recent signed bytes are committed.
    let due = 1_000 + rns_lite_core::ratchet::RATCHET_INTERVAL_SECS;
    let mut stale_blob = [0u8; RS_HANDHELD_RATCHET_RING_BLOB_MAX];
    let mut stale_len = 0usize;
    let mut stale_action = -1;
    let mut stale_public = [0u8; RATCHET_LEN];
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_prepare(
                ctx,
                &[0x72; RATCHET_LEN],
                due,
                0,
                stale_blob.as_mut_ptr(),
                stale_blob.len(),
                &mut stale_len,
                &mut stale_action,
                &mut stale_public,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(stale_action, RATCHET_PREP_ROTATED);
    assert_eq!(
        announce_status(ctx, wire_value, &stale_public),
        RsHandheldStatus::ErrNotReady
    );

    let mut latest_blob = [0u8; RS_HANDHELD_RATCHET_RING_BLOB_MAX];
    let mut latest_len = 0usize;
    let mut latest_action = -1;
    let mut latest_public = [0u8; RATCHET_LEN];
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_prepare(
                ctx,
                &[0x73; RATCHET_LEN],
                due,
                0,
                latest_blob.as_mut_ptr(),
                latest_blob.len(),
                &mut latest_len,
                &mut latest_action,
                &mut latest_public,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(latest_action, RATCHET_PREP_ROTATED);
    assert_ne!(latest_public, stale_public);
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_commit(ctx, stale_blob.as_ptr(), stale_len, &mut candidate)
        },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_commit(ctx, latest_blob.as_ptr(), latest_len, &mut candidate)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(candidate, latest_public);
    assert_eq!(
        announce_status(ctx, wire_value, &stale_public),
        RsHandheldStatus::ErrNotReady,
        "only the committed current ratchet may be advertised"
    );

    ratchet_blob.fill(0);
    tampered.fill(0);
    stale_blob.fill(0);
    latest_blob.fill(0);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn announce_order_state_is_monotonic_identity_bound_and_restart_safe() {
    let identity = [0x74; PRIVATE_KEY_LENGTH];
    let ctx = ctx_with_identity(&identity);
    let mut blob = [0u8; RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX];
    let mut len = 0usize;
    let mut value = 0u64;
    let mut ready = 0;

    let prepare = |ctx,
                   wall,
                   blob: &mut [u8; RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX],
                   len: &mut usize,
                   value: &mut u64,
                   ready: &mut i32| unsafe {
        rs_handheld_rns_announce_state_prepare(
            ctx,
            wall,
            blob.as_mut_ptr(),
            blob.len(),
            len,
            value,
            ready,
        )
    };
    assert_eq!(
        prepare(ctx, 500, &mut blob, &mut len, &mut value, &mut ready),
        RsHandheldStatus::Ok
    );
    assert_eq!(
        (len, value, ready),
        (RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX, 500, 1)
    );
    assert_eq!(unsafe { &*ctx }.announce_wire.last_value(), None);
    assert_eq!(
        unsafe { rs_handheld_rns_announce_state_commit(ctx, blob.as_ptr(), len, &mut value) },
        RsHandheldStatus::Ok
    );

    let persisted = blob;
    for wall in [500, 499] {
        len = usize::MAX;
        ready = -1;
        assert_eq!(
            prepare(ctx, wall, &mut blob, &mut len, &mut value, &mut ready),
            RsHandheldStatus::Ok
        );
        assert_eq!((len, value, ready), (0, 500, 0));
    }

    // No-clock mode prepares a durable logical successor, without changing live state.
    assert_eq!(
        prepare(ctx, 0, &mut blob, &mut len, &mut value, &mut ready),
        RsHandheldStatus::Ok
    );
    assert_eq!((value, ready), (501, 1));
    assert_eq!(unsafe { &*ctx }.announce_wire.last_value(), Some(500));

    let restored = ctx_with_identity(&identity);
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_state_seed(restored, persisted.as_ptr(), persisted.len())
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        prepare(restored, 0, &mut blob, &mut len, &mut value, &mut ready),
        RsHandheldStatus::Ok
    );
    assert_eq!((value, ready), (501, 1));
    assert_eq!(
        unsafe { rs_handheld_rns_announce_state_commit(restored, blob.as_ptr(), len, &mut value) },
        RsHandheldStatus::Ok
    );

    let other = ctx_with_identity(&[0x75; PRIVATE_KEY_LENGTH]);
    assert_eq!(
        unsafe { rs_handheld_rns_announce_state_seed(other, persisted.as_ptr(), persisted.len()) },
        RsHandheldStatus::ErrCrypto
    );
    let mut tampered = persisted;
    tampered[2] ^= 1;
    assert_eq!(
        unsafe { rs_handheld_rns_announce_state_seed(ctx, tampered.as_ptr(), tampered.len()) },
        RsHandheldStatus::ErrCrypto
    );
    assert_eq!(unsafe { &*ctx }.announce_wire.last_value(), Some(500));

    unsafe { rs_handheld_rns_shutdown(ctx) };
    unsafe { rs_handheld_rns_shutdown(restored) };
    unsafe { rs_handheld_rns_shutdown(other) };
}

#[test]
fn identity_reload_clears_all_identity_scoped_ratchet_state() {
    let ctx = ctx_with_identity(&[0x76; PRIVATE_KEY_LENGTH]);
    let (_current, action) = test_persist_commit_ratchet(ctx, &[0x77; 32], 700, 0);
    assert_eq!(action, RATCHET_PREP_ROTATED);
    assert_eq!(test_persist_commit_announce_wire(ctx, 700), 700);
    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchet_remember(
                ctx,
                &[0x78; DESTINATION_LENGTH],
                &[0x79; RATCHET_LEN],
                700,
                0,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);

    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(ctx, &[0x7A; PRIVATE_KEY_LENGTH]) },
        RsHandheldStatus::Ok
    );
    assert!(unsafe { &*ctx }.ratchet_ring.is_empty());
    assert!(unsafe { &*ctx }.peer_ratchets.is_empty());
    assert_eq!(unsafe { &*ctx }.announce_wire.last_value(), None);
    assert!(unsafe { &*ctx }.pending_ratchet.is_none());
    assert!(unsafe { &*ctx }.pending_announce_wire.is_none());
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_current(ctx, &mut [0u8; RATCHET_LEN]) },
        RsHandheldStatus::ErrNotReady
    );

    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn peer_seed_at_migrates_unknown_ages_and_rejects_flash_corruption_atomically() {
    let ctx = ctx_with_identity(&[0x7B; PRIVATE_KEY_LENGTH]);
    let destination = [0x7C; DESTINATION_LENGTH];
    let ratchet = [0x7D; RATCHET_LEN];

    // Legacy v1: version | count | destination | ratchet | age-seconds. A zero age is
    // deliberately unknown, so boot must anchor it to a real local clock and persist v2.
    let mut legacy = [0u8; 2 + DESTINATION_LENGTH + RATCHET_LEN + 8];
    legacy[0] = 1;
    legacy[1] = 1;
    legacy[2..18].copy_from_slice(&destination);
    legacy[18..50].copy_from_slice(&ratchet);
    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_seed_at(
                ctx,
                legacy.as_ptr(),
                legacy.len(),
                8_000,
                0,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);

    let mut persisted = [0u8; RS_HANDHELD_PEER_RATCHETS_BLOB_MAX];
    let mut persisted_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_export(
                ctx,
                persisted.as_mut_ptr(),
                persisted.len(),
                &mut persisted_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(persisted[0], 2);
    assert_eq!(
        PeerRatchets::deserialize(&persisted[..persisted_len])
            .unwrap()
            .get(&destination, RatchetClock::new(Some(8_000), 0)),
        Some(ratchet)
    );

    // Wall-anchored v2 data remains comparable after restart and needs no rewrite.
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_seed_at(
                ctx,
                persisted.as_ptr(),
                persisted_len,
                8_100,
                0,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 0);

    // A malformed flash read is validated in full before live state is touched.
    let mut corrupt = persisted;
    corrupt[1] = 0xff;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_seed_at(
                ctx,
                corrupt.as_ptr(),
                persisted_len,
                8_200,
                0,
                &mut changed,
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe { &*ctx }
            .peer_ratchets
            .get(&destination, RatchetClock::new(Some(8_200), 0)),
        Some(ratchet)
    );

    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ratchet_rotate_interval_and_signed_persistence_roundtrip() {
    let prv = [0x61u8; PRIVATE_KEY_LENGTH];
    let ctx = ctx_with_identity(&prv);
    let mut out = [0u8; RATCHET_LEN];

    // Never rotated: current is NotReady; the first rotate always fires.
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_current(ctx, &mut out) },
        RsHandheldStatus::ErrNotReady
    );
    let (first, first_action) = test_persist_commit_ratchet(ctx, &[0xA1; 32], 1_000, 0);
    assert_eq!(first_action, RATCHET_PREP_ROTATED);

    // Within the rotation interval: no rotation, same current key.
    let (within, within_action) = test_persist_commit_ratchet(ctx, &[0xA2; 32], 1_010, 0);
    assert_eq!(within_action, RATCHET_PREP_UNCHANGED);
    assert_eq!(within, first);

    // Interval elapsed: rotates to a new key; current follows.
    let due = 1_000 + rns_lite_core::ratchet::RATCHET_INTERVAL_SECS;
    let (second, second_action) = test_persist_commit_ratchet(ctx, &[0xA3; 32], due, 0);
    assert_eq!(second_action, RATCHET_PREP_ROTATED);
    assert_ne!(second, first);
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_current(ctx, &mut out) },
        RsHandheldStatus::Ok
    );
    assert_eq!(out, second);

    // Signed export/seed roundtrip into a fresh ctx with the SAME identity.
    let mut blob = [0u8; RS_HANDHELD_RATCHET_RING_BLOB_MAX];
    let mut blob_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_export(ctx, blob.as_mut_ptr(), blob.len(), &mut blob_len)
        },
        RsHandheldStatus::Ok
    );
    let restored = ctx_with_identity(&prv);
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_seed(restored, blob.as_ptr(), blob_len) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_current(restored, &mut out) },
        RsHandheldStatus::Ok
    );
    assert_eq!(out, second);

    // Tampered blob (or a different identity's blob) is rejected, ring untouched.
    let mut bad = blob;
    bad[4] ^= 0x01;
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_seed(restored, bad.as_ptr(), blob_len) },
        RsHandheldStatus::ErrCrypto
    );
    let other = ctx_with_identity(&[0x62u8; PRIVATE_KEY_LENGTH]);
    assert_eq!(
        unsafe { rs_handheld_rns_ratchet_seed(other, blob.as_ptr(), blob_len) },
        RsHandheldStatus::ErrCrypto
    );

    unsafe { rs_handheld_rns_shutdown(ctx) };
    unsafe { rs_handheld_rns_shutdown(restored) };
    unsafe { rs_handheld_rns_shutdown(other) };
}

#[test]
fn ratcheted_announce_requires_the_context_flag_on_the_wire() {
    // The trap this guards: the announce PAYLOAD carries the ratchet, but receivers only
    // look for it when the packet header's context flag is set. Framed with the flag clear,
    // a ratcheted announce is mis-parsed and fails validation at every peer.
    let ctx = ctx_with_identity(&[0x67u8; PRIVATE_KEY_LENGTH]);
    let (ratchet, action) = test_persist_commit_ratchet(ctx, &[0xE1; 32], 9_000, 0);
    assert_eq!(action, RATCHET_PREP_ROTATED);
    assert_eq!(test_persist_commit_announce_wire(ctx, 9_000), 9_000);

    let mut payload = [0u8; 600];
    let mut payload_len = 0usize;
    let mut dest = [0u8; DESTINATION_LENGTH];
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce(
                ctx,
                &[0xE2; 5],
                9_000,
                &ratchet,
                core::ptr::null(),
                0,
                payload.as_mut_ptr(),
                payload.len(),
                &mut payload_len,
                &mut dest,
            )
        },
        RsHandheldStatus::Ok
    );

    let mut frame = [0u8; 640];
    let mut frame_len = 0usize;
    let build = |flag: i32, frame: &mut [u8; 640], frame_len: &mut usize| unsafe {
        rs_handheld_rns_packet_build_flagged(
            0,
            1, // ANNOUNCE
            0, // SINGLE
            0,
            flag,
            core::ptr::null(),
            &dest,
            payload.as_ptr(),
            payload_len,
            frame.as_mut_ptr(),
            frame.len(),
            frame_len,
        )
    };

    // Flag SET: the announce validates and the ratchet is surfaced.
    assert_eq!(build(1, &mut frame, &mut frame_len), RsHandheldStatus::Ok);
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                frame.as_ptr().add(frame_len - payload_len),
                payload_len,
                &dest,
                1,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev.has_ratchet, 1);
    assert_eq!(ev.ratchet, ratchet);

    // Flag CLEAR (the bug): the same payload read as non-ratcheted fails validation.
    assert_eq!(build(0, &mut frame, &mut frame_len), RsHandheldStatus::Ok);
    let mut ev2: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                frame.as_ptr().add(frame_len - payload_len),
                payload_len,
                &dest,
                0,
                core::ptr::null(),
                &mut ev2,
            )
        },
        RsHandheldStatus::ErrCrypto
    );

    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn lxmf_build_prefers_seeded_peer_ratchet_and_parse_uses_ring() {
    let sender_prv = [0x63u8; PRIVATE_KEY_LENGTH];
    let recipient_prv = [0x64u8; PRIVATE_KEY_LENGTH];
    let sender = ctx_with_identity(&sender_prv);
    let recipient = ctx_with_identity(&recipient_prv);
    let recipient_id = LocalIdentity::from_private_key(&recipient_prv);
    let now: u64 = 10_000;

    // Recipient rotates its ring; sender learns the announced ratchet via a seeded table.
    let (announced, action) = test_persist_commit_ratchet(recipient, &[0xB1; 32], now, 0);
    assert_eq!(action, RATCHET_PREP_ROTATED);
    let mut table = PeerRatchets::new();
    table.remember(
        recipient_id.lxmf_delivery_hash(),
        announced,
        RatchetClock::new(Some(now), 0),
    );
    let mut tblob = [0u8; PEER_RATCHETS_BLOB_MAX];
    let tlen = table.serialize_into(&mut tblob).unwrap();
    assert_eq!(
        unsafe { rs_handheld_rns_peer_ratchets_seed(sender, tblob.as_ptr(), tlen) },
        RsHandheldStatus::Ok
    );

    // Retain a newer key locally after the sender learned the first one. The incoming frame
    // below must therefore decrypt at ring index 1, not merely prove index-0 behavior.
    let (_newer, newer_action) = test_persist_commit_ratchet(
        recipient,
        &[0xB2; 32],
        now + rns_lite_core::ratchet::RATCHET_INTERVAL_SECS,
        0,
    );
    assert_eq!(newer_action, RATCHET_PREP_ROTATED);

    // Sender builds: must target the ratchet, not the base key.
    let mut blob = [0u8; 600];
    let (mut n, mut dest, mut mid) = (0usize, [0u8; DESTINATION_LENGTH], [0u8; 32]);
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                sender,
                recipient_id.public_key(),
                now as f64,
                now,
                0,
                b"Hi".as_ptr(),
                2,
                b"ratchet path".as_ptr(),
                12,
                &[0xC1; 32],
                &[0xC2; 16],
                blob.as_mut_ptr(),
                blob.len(),
                &mut n,
                &mut dest,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );
    let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
    let base_x: [u8; 32] = recipient_id.private_key()[..32].try_into().unwrap();
    assert!(
        rns_lite_core::crypto::ecies_decrypt(
            &blob[..n],
            &base_x,
            recipient_id.identity_hash(),
            &mut scratch,
        )
        .is_err(),
        "blob must not open with the base key when a peer ratchet is known"
    );

    // Recipient's FFI parse succeeds via its ring; peek recalls the source first.
    let sender_id = LocalIdentity::from_private_key(&sender_prv);
    let mut src = [0u8; DESTINATION_LENGTH];
    let mut key_hint = RS_HANDHELD_LXMF_BASE_KEY_HINT;
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_peek_source_hint(
                recipient,
                blob.as_ptr(),
                n,
                &mut src,
                &mut key_hint,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(src, sender_id.lxmf_delivery_hash());
    assert_eq!(key_hint, 1);
    let mut msg: RsHandheldLxmfMessage = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_hint(
                recipient,
                blob.as_ptr(),
                n,
                key_hint,
                sender_id.public_key(),
                &mut msg,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(&msg.content[..msg.content_len as usize], b"ratchet path");
    assert_eq!(msg.message_id, mid);
    for wrong_hint in [0, RS_HANDHELD_LXMF_BASE_KEY_HINT, 63, 254] {
        assert_eq!(
            unsafe {
                rs_handheld_rns_lxmf_parse_hint(
                    recipient,
                    blob.as_ptr(),
                    n,
                    wrong_hint,
                    sender_id.public_key(),
                    &mut msg,
                )
            },
            RsHandheldStatus::ErrCrypto,
            "wrong hint {wrong_hint} must fail closed without a scan"
        );
    }

    // Fresh sender with an EMPTY peer table: base-key build still parses (fallback, never enforced).
    let plain_sender = ctx_with_identity(&[0x65u8; PRIVATE_KEY_LENGTH]);
    let plain_id = LocalIdentity::from_private_key(&[0x65u8; PRIVATE_KEY_LENGTH]);
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                plain_sender,
                recipient_id.public_key(),
                now as f64,
                now,
                0,
                b"Hi".as_ptr(),
                2,
                b"base path".as_ptr(),
                9,
                &[0xC3; 32],
                &[0xC4; 16],
                blob.as_mut_ptr(),
                blob.len(),
                &mut n,
                &mut dest,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_peek_source_hint(
                recipient,
                blob.as_ptr(),
                n,
                &mut src,
                &mut key_hint,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(key_hint, RS_HANDHELD_LXMF_BASE_KEY_HINT);
    assert_eq!(src, plain_id.lxmf_delivery_hash());
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_hint(
                recipient,
                blob.as_ptr(),
                n,
                key_hint,
                plain_id.public_key(),
                &mut msg,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(&msg.content[..msg.content_len as usize], b"base path");
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_hint(
                recipient,
                blob.as_ptr(),
                n,
                0,
                plain_id.public_key(),
                &mut msg,
            )
        },
        RsHandheldStatus::ErrCrypto
    );

    unsafe { rs_handheld_rns_shutdown(sender) };
    unsafe { rs_handheld_rns_shutdown(recipient) };
    unsafe { rs_handheld_rns_shutdown(plain_sender) };
}
