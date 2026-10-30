use super::*;

fn capture(
    ctx: *mut RsHandheldRns,
    id: [u8; 16],
    link: bool,
    context: PacketContext,
) -> [u8; RS_HANDHELD_TX_LIFETIME_BYTES] {
    let mut raw = [0u8; 20];
    raw[0] = if link { 0x0c } else { 0 };
    raw[2..18].copy_from_slice(&id);
    raw[18] = context.to_byte();
    raw[19] = 0xff;
    let mut token = [0u8; RS_HANDHELD_TX_LIFETIME_BYTES];
    assert_eq!(
        unsafe {
            rs_handheld_rns_capture_outbound_lifetime(
                ctx,
                raw.as_ptr(),
                raw.len(),
                1000,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    token
}

fn live(ctx: *mut RsHandheldRns, token: &[u8; RS_HANDHELD_TX_LIFETIME_BYTES], now: u64) -> bool {
    let mut value = -1;
    assert_eq!(
        unsafe { rs_handheld_rns_outbound_lifetime_is_live(ctx, token, 0, now, &mut value) },
        RsHandheldStatus::Ok
    );
    value == 1
}

#[test]
fn ffi_deferred_capture_preserves_birth_and_rejects_invalid_or_expired_windows() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let mut raw = [0u8; 20];
    raw[2..18].copy_from_slice(&[0x24; 16]);
    let mut token = [0xa5; RS_HANDHELD_TX_LIFETIME_BYTES];
    assert_eq!(
        unsafe {
            rs_handheld_rns_capture_outbound_lifetime_at(
                ctx,
                raw.as_ptr(),
                raw.len(),
                5000,
                1000,
                5000,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    let lifetime =
        rns_lite_core::transport::OutboundLifetime::from_token(token[..88].try_into().unwrap())
            .unwrap();
    assert_eq!((lifetime.enqueued_ms, lifetime.expires_ms), (1000, 6000));
    assert!(live(ctx, &token, 5999));
    assert!(!live(ctx, &token, 6000));
    let path = rns_lite_core::tables::PathEntry {
        destination_hash: [0x24; 16],
        next_hop: None,
        hops: 1,
        interface_id: 0,
        expires_ms: 5500,
        last_seen_ms: 1000,
        packet_hash: [0x11; 32],
        random_hash: [0x01; 10],
        public_key: [0x22; 64],
        bulk: false,
    };
    assert!(unsafe {
        (*ctx)
            .node
            .unwrap()
            .as_mut()
            .paths
            .insert_or_update(path, 5000)
    });
    assert_eq!(
        unsafe {
            rs_handheld_rns_capture_outbound_lifetime_at(
                ctx,
                raw.as_ptr(),
                raw.len(),
                5000,
                1000,
                5000,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    let dependent =
        rns_lite_core::transport::OutboundLifetime::from_token(token[..88].try_into().unwrap())
            .unwrap();
    assert_eq!((dependent.enqueued_ms, dependent.expires_ms), (1000, 5500));
    assert!(live(ctx, &token, 5499));
    assert!(!live(ctx, &token, 5500));
    for (now, born, wait, expected) in [
        (999, 1000, 5000, RsHandheldStatus::ErrInvalidArg),
        (5000, 1000, 0, RsHandheldStatus::ErrInvalidArg),
        (5000, 1000, 120001, RsHandheldStatus::ErrInvalidArg),
        (6000, 1000, 5000, RsHandheldStatus::ErrNotReady),
        (u64::MAX, u64::MAX - 1, 5, RsHandheldStatus::ErrInvalidArg),
    ] {
        token.fill(0xa5);
        assert_eq!(
            unsafe {
                rs_handheld_rns_capture_outbound_lifetime_at(
                    ctx,
                    raw.as_ptr(),
                    raw.len(),
                    now,
                    born,
                    wait,
                    &mut token,
                )
            },
            expected
        );
        assert_eq!(token, [0xa5; RS_HANDHELD_TX_LIFETIME_BYTES]);
    }
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ffi_poll_retains_original_deadline_and_preserves_frame_on_capacity_error() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    assert_eq!(
        unsafe { rs_handheld_rns_request_path(ctx, &[0x21; 16], &[0x31; 16], 0, 1000) },
        RsHandheldStatus::Ok
    );
    let mut raw = [0u8; 500];
    let (mut len, mut iface, mut reason) = (0, 0, -1);
    let mut token = [0xa5; RS_HANDHELD_TX_LIFETIME_BYTES];
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound_leased(
                ctx,
                raw.as_mut_ptr(),
                8,
                &mut len,
                &mut iface,
                &mut reason,
                &mut token,
            )
        },
        RsHandheldStatus::ErrCapacity
    );
    assert_eq!(token, [0xa5; RS_HANDHELD_TX_LIFETIME_BYTES]);
    assert_eq!(
        unsafe { rs_handheld_rns_tick(ctx, 110_000) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound_leased(
                ctx,
                raw.as_mut_ptr(),
                raw.len(),
                &mut len,
                &mut iface,
                &mut reason,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(len > 0);
    assert_eq!(reason, 3);
    assert!(!live(ctx, &token, 999));
    assert!(live(ctx, &token, 120_999));
    assert!(!live(ctx, &token, 121_000));
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound_leased(
                ctx,
                raw.as_mut_ptr(),
                raw.len(),
                &mut len,
                &mut iface,
                &mut reason,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(len, 0);
    assert_eq!(token, [0; RS_HANDHELD_TX_LIFETIME_BYTES]);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ffi_local_link_generation_prevents_unregister_reregister_revival() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let id = [0x41; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &id) },
        RsHandheldStatus::Ok
    );
    let old = capture(ctx, id, true, PacketContext::Keepalive);
    assert!(live(ctx, &old, 1000));
    // Idempotent registration does not retire a live session.
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &id) },
        RsHandheldStatus::Ok
    );
    assert!(live(ctx, &old, 1001));
    assert_eq!(
        unsafe { rs_handheld_rns_link_unregister(ctx, &id) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &old, 1001));
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &id) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &old, 1001));
    let current = capture(ctx, id, true, PacketContext::Keepalive);
    assert!(live(ctx, &current, 1001));
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ffi_teardown_can_finish_after_close_but_not_slot_reuse() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let id = [0x42; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &id) },
        RsHandheldStatus::Ok
    );
    let close = capture(ctx, id, true, PacketContext::LinkClose);
    assert_eq!(
        unsafe { rs_handheld_rns_link_unregister(ctx, &id) },
        RsHandheldStatus::Ok
    );
    assert!(live(ctx, &close, 1001));
    assert!(!live(ctx, &close, 121_000));
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &[0x43; 16]) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &close, 1001));
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ffi_token_rejects_malformed_discriminants_and_deadline_extension() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let original = capture(ctx, [0x44; 16], false, PacketContext::None);
    for (offset, value) in [
        (0, 0),
        (4, 2),
        (5, 9),
        (6, 1),
        (72, 1),
        (92, 9),
        (93, 2),
        (94, 1),
        (100, 3),
        (101, 1),
    ] {
        let mut token = original;
        token[offset] = value;
        let mut result = -1;
        assert_eq!(
            unsafe { rs_handheld_rns_outbound_lifetime_is_live(ctx, &token, 0, 1000, &mut result) },
            RsHandheldStatus::ErrInvalidArg,
            "offset={offset}"
        );
    }
    let mut token = original;
    token[16..24].copy_from_slice(&121_001u64.to_le_bytes());
    let mut result = -1;
    assert_eq!(
        unsafe { rs_handheld_rns_outbound_lifetime_is_live(ctx, &token, 0, 1000, &mut result) },
        RsHandheldStatus::ErrInvalidArg
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ffi_link_registration_reports_capacity_and_generation_exhaustion() {
    let ctx = loaded_ctx();
    for i in 0..LOCAL_LINK_SLOTS {
        assert_eq!(
            unsafe { rs_handheld_rns_link_register(ctx, &[i as u8; 16]) },
            RsHandheldStatus::Ok
        );
    }
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &[0xee; 16]) },
        RsHandheldStatus::ErrCapacity
    );
    for i in 0..LOCAL_LINK_SLOTS {
        assert_eq!(
            unsafe { rs_handheld_rns_link_unregister(ctx, &[i as u8; 16]) },
            RsHandheldStatus::Ok
        );
    }
    unsafe { (*ctx).link_generations = [u32::MAX; LOCAL_LINK_SLOTS] };
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &[0xee; 16]) },
        RsHandheldStatus::ErrCapacity
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

fn resource(ctx: *mut RsHandheldRns, seed: u8) -> Vec<u8> {
    let mut adv = [0; RS_HANDHELD_RESOURCE_ADV_MAX];
    let (mut length, mut parts) = (0, 0);
    let data = [0x58; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_build(
                ctx,
                &[0x51; 64],
                data.as_ptr(),
                data.len(),
                &[seed; 4],
                &[seed; 16],
                adv.as_mut_ptr(),
                adv.len(),
                &mut length,
                &mut parts,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(parts > 0);
    adv[..length].to_vec()
}

fn accept_resource(ctx: *mut RsHandheldRns, adv: &[u8]) {
    let (mut parts, mut transfer, mut size) = (0, 0, 0);
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_accept(
                ctx,
                adv.as_ptr(),
                adv.len(),
                &mut parts,
                &mut transfer,
                &mut size,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
}

#[test]
fn ffi_resource_parts_and_requests_cannot_revive_on_a_still_open_link() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let link = [0x49; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &link) },
        RsHandheldStatus::Ok
    );
    let adv = resource(ctx, 0x31);
    let part = capture(ctx, link, true, PacketContext::Resource);
    let advert = capture(ctx, link, true, PacketContext::ResourceAdv);
    let cancel = capture(ctx, link, true, PacketContext::ResourceIcl);
    assert!(live(ctx, &part, 1001) && live(ctx, &advert, 1001));
    assert_eq!(
        unsafe { rs_handheld_rns_resource_outbound_close(ctx) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &part, 1001) && !live(ctx, &advert, 1001));
    assert!(live(ctx, &cancel, 1001));
    resource(ctx, 0x32);
    assert!(!live(ctx, &part, 1001) && live(ctx, &cancel, 1001));
    assert!(live(
        ctx,
        &capture(ctx, link, true, PacketContext::Resource),
        1001
    ));

    accept_resource(ctx, &adv);
    let request = capture(ctx, link, true, PacketContext::ResourceReq);
    let proof = capture(ctx, link, true, PacketContext::ResourcePrf);
    assert!(live(ctx, &request, 1001));
    assert_eq!(
        unsafe { rs_handheld_rns_resource_inbound_close(ctx) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &request, 1001) && live(ctx, &proof, 1001));
    accept_resource(ctx, &adv);
    assert!(!live(ctx, &request, 1001) && live(ctx, &proof, 1001));
    assert_eq!(
        unsafe { rs_handheld_rns_link_unregister(ctx, &link) },
        RsHandheldStatus::Ok
    );
    assert!(!live(ctx, &proof, 1001) && !live(ctx, &cancel, 1001));
    unsafe { rs_handheld_rns_shutdown(ctx) };
}
