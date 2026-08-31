use super::*;
use core::ffi::CStr;

#[test]
fn version_is_valid_cstring() {
    let p = rs_handheld_rns_version();
    assert!(!p.is_null());
    let s = unsafe { CStr::from_ptr(p) }.to_str().unwrap();
    assert!(
        s.contains("ratspeak-handheld-protocol"),
        "version was {s:?}"
    );
    assert!(s.contains("link+resource"), "version was {s:?}");
}

#[test]
fn init_status_shutdown_roundtrip() {
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    assert!(!ctx.is_null());
    // Scaffold is honest: a valid context is NOT_READY, not OK.
    assert_eq!(
        unsafe { rs_handheld_rns_status(ctx) },
        RsHandheldStatus::ErrNotReady
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn double_init_yields_distinct_contexts() {
    let mut a: *mut RsHandheldRns = core::ptr::null_mut();
    let mut b: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut a) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut b) },
        RsHandheldStatus::Ok
    );
    assert!(!a.is_null() && !b.is_null() && a != b);
    unsafe { rs_handheld_rns_shutdown(a) };
    unsafe { rs_handheld_rns_shutdown(b) };
}

#[test]
fn null_args_are_handled() {
    assert_eq!(
        unsafe { rs_handheld_rns_init(core::ptr::null_mut()) },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe { rs_handheld_rns_status(core::ptr::null()) },
        RsHandheldStatus::ErrInvalidArg
    );
    // Null shutdown must not crash.
    unsafe { rs_handheld_rns_shutdown(core::ptr::null_mut()) };
}

#[test]
fn panic_guard_returns_internal() {
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {})); // silence the expected panic
    let s = guard(|| panic!("intentional test panic"));
    std::panic::set_hook(prev);
    assert_eq!(s, RsHandheldStatus::ErrInternal);
}

#[test]
fn pure_wire_and_auto_ffi_builders() {
    let destination = [0xaau8; 16];
    let transport = [0xbbu8; 16];
    let payload = [0xffu8];
    let mut out = [0u8; 500];
    let mut out_len = 0usize;

    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_build(
                0,
                PacketType::Data as i32,
                DestinationType::Link as i32,
                PacketContext::Keepalive.to_byte(),
                core::ptr::null(),
                &destination,
                payload.as_ptr(),
                payload.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(out_len, 20);
    assert_eq!(out[0], 0x0c);
    assert_eq!(&out[2..18], &destination);
    assert_eq!(&out[18..out_len], &[0xfa, 0xff]);

    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_build(
                1,
                PacketType::LinkRequest as i32,
                DestinationType::Single as i32,
                PacketContext::None.to_byte(),
                &transport,
                &destination,
                payload.as_ptr(),
                payload.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(out_len, 36);
    assert_eq!(&out[..2], &[0x52, 0]);
    assert_eq!(&out[2..18], &transport);
    assert_eq!(&out[18..34], &destination);
    assert_eq!(&out[34..out_len], &[0, 0xff]);

    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_build(
                0,
                0,
                0,
                0,
                &transport,
                &destination,
                core::ptr::null(),
                0,
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_build(
                0,
                0,
                0,
                0,
                core::ptr::null(),
                &destination,
                payload.as_ptr(),
                payload.len(),
                out.as_mut_ptr(),
                19,
                &mut out_len,
            )
        },
        RsHandheldStatus::ErrCapacity
    );

    let address = unhex::<16>("fe8000000000000032aea4fffe80e5ca");
    let mut text = [0 as c_char; 40];
    let mut text_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_auto_format_ipv6(&address, text.as_mut_ptr(), text.len(), &mut text_len)
        },
        RsHandheldStatus::Ok
    );
    let text_bytes = unsafe { core::slice::from_raw_parts(text.as_ptr().cast::<u8>(), text_len) };
    assert_eq!(text_bytes, b"fe80::32ae:a4ff:fe80:e5ca");
    assert_eq!(text[text_len], 0);

    let group_id = b"reticulum";
    let mut group = [0u8; 16];
    assert_eq!(
        unsafe {
            rs_handheld_rns_auto_multicast_group(group_id.as_ptr(), group_id.len(), &mut group)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(group, unhex("ff120000d70bfb1c16e45e39485e31e1"));

    let mut token = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_auto_beacon_token(
                group_id.as_ptr(),
                group_id.len(),
                &address,
                &mut token,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        token,
        unhex("e71916549d73d3589a46da91e5c3d9f800bc72b024d1820d1fac9de26570e780")
    );
    assert_eq!(
        unsafe { rs_handheld_rns_auto_format_ipv6(&address, text.as_mut_ptr(), 1, &mut text_len) },
        RsHandheldStatus::ErrCapacity
    );
}

fn unhex<const N: usize>(s: &str) -> [u8; N] {
    let mut o = [0u8; N];
    for (i, byte) in o.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
    }
    o
}

#[test]
fn identity_ffi_parity_and_lifecycle() {
    // "incrementing" vector (Python RNS) — same as rns-lite's parity vectors.
    let prv: [u8; 64] = unhex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
    );
    let want_id: [u8; 16] = unhex("aca31af0441d81dbec71e82da0b4b5f5");
    let want_dest: [u8; 16] = unhex("fae321c442e3c9bdcd7a3e79d850e03c");

    // Stateless validate (out_public_key optional -> null).
    let mut id_hash = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_validate_identity(&prv, &mut id_hash, core::ptr::null_mut()) },
        RsHandheldStatus::Ok
    );
    assert_eq!(id_hash, want_id);

    // Context lifecycle: not-ready before load, parity after.
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    let mut h = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_identity_hash(ctx, &mut h) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(ctx, &prv) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_identity_hash(ctx, &mut h) },
        RsHandheldStatus::Ok
    );
    assert_eq!(h, want_id);
    let mut d = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut d) },
        RsHandheldStatus::Ok
    );
    assert_eq!(d, want_dest);
    let mut exported = [0u8; 64];
    assert_eq!(
        unsafe { rs_handheld_rns_export_identity(ctx, &mut exported) },
        RsHandheldStatus::Ok
    );
    assert_eq!(exported, prv);
    unsafe { rs_handheld_rns_shutdown(ctx) };

    // Null-arg contract.
    assert_eq!(
        unsafe {
            rs_handheld_rns_validate_identity(
                core::ptr::null(),
                &mut id_hash,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );
}

#[test]
fn create_identity_derives_from_entropy() {
    let entropy: [u8; 64] = unhex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
    );
    let mut prv = [0u8; 64];
    let mut id_hash = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_create_identity(&entropy, &mut prv, &mut id_hash) },
        RsHandheldStatus::Ok
    );
    assert_eq!(prv, entropy); // entropy IS the private key
    assert_eq!(id_hash, unhex::<16>("aca31af0441d81dbec71e82da0b4b5f5"));
}

fn to_hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for byte in b {
        s.push_str(&format!("{byte:02x}"));
    }
    s
}

// Pinned announce vectors (vectors/announce_vectors.json, RNS 1.3.8 validate_announce: OK).
const INCREMENTING: &str = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
// random_hash = rng_seed(5) || unix_secs_be(5); 1234567890 -> 00 49 96 02 d2,
// so the composed random_hash is a1a2a3a4a500499602d2 (matches the pinned vectors).
const RNG_SEED: [u8; 5] = [0xa1, 0xa2, 0xa3, 0xa4, 0xa5];
const UNIX_SECS: u64 = 1234567890;
const RATCHET_PRIVATE: [u8; 32] = [0x91; 32];
const RATCHET: &str = "0791f592e9fdf04d44d7e0e23500b6ccf035be69667647589967e6b612a13150";
const RAT_APP_DATA: &str = "93c403526174c090";
const WANT_DEST: &str = "fae321c442e3c9bdcd7a3e79d850e03c";
const WANT_ID: &str = "aca31af0441d81dbec71e82da0b4b5f5";
const V_RAT_NO_RATCHET: &str = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d908a1a2a3a4a500499602d2f1e57775c5bfc843cba0fe74b4dab8efd846fa5d38818b3d06e19f94c529c1c1d6b8d738d108512f43f189f4a552f8d7b1d1943022d4fcd2d2511a14b1bb3e0c93c403526174c090";
const V_EMPTY_NO_RATCHET: &str = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d908a1a2a3a4a500499602d20e8907915c27f1e3583682ea8db429e79be165eff7128ae170f6b6f5d72a6d15408a7bb57e3fd89312fbdc60814df8679743b737a50b89bcd9046490a8789804";
const V_RAT_WITH_RATCHET: &str = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d908a1a2a3a4a500499602d20791f592e9fdf04d44d7e0e23500b6ccf035be69667647589967e6b612a13150da718acb19e9e08a063c72c54f872952f3033016b3b95a227db28906af289cd26de355e2a311a23283a510027595901f782998b222123aa2f482f31e2a523c0793c403526174c090";

fn loaded_ctx() -> *mut RsHandheldRns {
    let prv: [u8; 64] = unhex(INCREMENTING);
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(ctx, &prv) },
        RsHandheldStatus::Ok
    );
    ctx
}

fn announce_at(
    ctx: *mut RsHandheldRns,
    rng_seed: &[u8; RANDOM_SEED_LEN],
    announce_order: u64,
    ratchet: *const [u8; 32],
    app: &[u8],
) -> (Vec<u8>, [u8; 16]) {
    if !ratchet.is_null() {
        assert_eq!(
            test_persist_commit_announce_wire(ctx, announce_order),
            announce_order
        );
    }
    let mut out = [0u8; 600];
    let mut out_len = 0usize;
    let mut dest = [0u8; 16];
    let app_ptr = if app.is_empty() {
        core::ptr::null()
    } else {
        app.as_ptr()
    };
    let st = unsafe {
        rs_handheld_rns_announce(
            ctx,
            rng_seed,
            announce_order,
            ratchet,
            app_ptr,
            app.len(),
            out.as_mut_ptr(),
            out.len(),
            &mut out_len,
            &mut dest,
        )
    };
    assert_eq!(st, RsHandheldStatus::Ok);
    (out[..out_len].to_vec(), dest)
}

fn announce(ctx: *mut RsHandheldRns, ratchet: *const [u8; 32], app: &[u8]) -> (Vec<u8>, [u8; 16]) {
    announce_at(ctx, &RNG_SEED, UNIX_SECS, ratchet, app)
}

#[test]
fn announce_create_byte_parity_with_python() {
    let ctx = loaded_ctx();
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let ratchet: [u8; 32] = unhex(RATCHET);
    let want_dest: [u8; 16] = unhex(WANT_DEST);

    let (a, dest) = announce(ctx, core::ptr::null(), &rat);
    assert_eq!(to_hex(&a), V_RAT_NO_RATCHET);
    assert_eq!(dest, want_dest);

    let (b, _) = announce(ctx, core::ptr::null(), &[]);
    assert_eq!(to_hex(&b), V_EMPTY_NO_RATCHET);

    let (committed, action) = test_persist_commit_ratchet(ctx, &RATCHET_PRIVATE, UNIX_SECS, 0);
    assert_eq!(action, RATCHET_PREP_ROTATED);
    assert_eq!(committed, ratchet);
    let (c, _) = announce(ctx, &ratchet, &rat);
    assert_eq!(to_hex(&c), V_RAT_WITH_RATCHET);

    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn announce_ingest_validates_and_extracts() {
    let ctx = loaded_ctx();
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let ratchet: [u8; 32] = unhex(RATCHET);
    let want_dest: [u8; 16] = unhex(WANT_DEST);
    let want_id: [u8; 16] = unhex(WANT_ID);

    // No ratchet.
    let (a, dest) = announce(ctx, core::ptr::null(), &rat);
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                a.as_ptr(),
                a.len(),
                &dest,
                0,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev.identity_hash, want_id);
    assert_eq!(ev.destination_hash, want_dest);
    assert_eq!(ev.has_ratchet, 0);
    assert_eq!(&ev.app_data[..ev.app_data_len as usize], &rat[..]);

    // With ratchet (context_flag = 1).
    let (committed, action) = test_persist_commit_ratchet(ctx, &RATCHET_PRIVATE, UNIX_SECS, 0);
    assert_eq!(action, RATCHET_PREP_ROTATED);
    assert_eq!(committed, ratchet);
    let (c, dest_c) = announce(ctx, &ratchet, &rat);
    let mut ev2: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                c.as_ptr(),
                c.len(),
                &dest_c,
                1,
                core::ptr::null(),
                &mut ev2,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev2.has_ratchet, 1);
    assert_eq!(ev2.ratchet, ratchet);
    assert_eq!(&ev2.app_data[..ev2.app_data_len as usize], &rat[..]);

    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn announce_ingest_rejects_tampered_and_wrong_dest() {
    let ctx = loaded_ctx();
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let want_id: [u8; 16] = unhex(WANT_ID);
    let (mut a, dest) = announce(ctx, core::ptr::null(), &rat);

    // Tampered signature -> ErrCrypto.
    let mut tampered = a.clone();
    let sig_start = 64 + 10 + 10; // pub + name_hash + random_hash
    tampered[sig_start] ^= 0xFF;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                tampered.as_ptr(),
                tampered.len(),
                &dest,
                0,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::ErrCrypto
    );

    // Wrong destination hash -> ErrCrypto (binding mismatch).
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                a.as_ptr(),
                a.len(),
                &want_id,
                0,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::ErrCrypto
    );

    // Wrong context_flag (claim ratchet on a no-ratchet announce) is always rejected:
    // the claimed ratchet carves 32 bytes out of the layout, here under-running it
    // (ErrInvalidArg); with larger app_data it would instead break the signature
    // (ErrCrypto). Either way it must never validate.
    assert_ne!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                a.as_ptr(),
                a.len(),
                &dest,
                1,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::Ok
    );

    // No identity loaded -> ErrNotReady on create.
    a[0] ^= 0x00; // keep `a` used
    let mut empty_ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut empty_ctx) },
        RsHandheldStatus::Ok
    );
    let mut out = [0u8; 512];
    let mut out_len = 0usize;
    let mut d = [0u8; 16];
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce(
                empty_ctx,
                &RNG_SEED,
                UNIX_SECS,
                core::ptr::null(),
                rat.as_ptr(),
                rat.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
                &mut d,
            )
        },
        RsHandheldStatus::ErrNotReady
    );
    unsafe { rs_handheld_rns_shutdown(empty_ctx) };
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn announce_ingest_accepts_full_single_packet_app_data() {
    // A network-valid announce with >256 bytes of app_data (a long LXMF display name) must
    // be accepted, not black-holed. Round-trips create -> ingest at the wire max.
    let ctx = loaded_ctx();
    let want_id: [u8; 16] = unhex(WANT_ID);
    let big = vec![0x5au8; RS_HANDHELD_ANNOUNCE_MAX_APP_DATA];
    let (a, dest) = announce(ctx, core::ptr::null(), &big);
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                a.as_ptr(),
                a.len(),
                &dest,
                0,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev.identity_hash, want_id);
    assert_eq!(ev.app_data_len as usize, RS_HANDHELD_ANNOUNCE_MAX_APP_DATA);
    assert_eq!(&ev.app_data[..ev.app_data_len as usize], &big[..]);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn announce_ingest_enforces_first_seen_key_continuity() {
    // With a known public key, a matching announce validates; a different key for the same
    // destination is rejected (the hash-collision / path-hijack defense).
    let ctx = loaded_ctx();
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let (a, dest) = announce(ctx, core::ptr::null(), &rat);

    // The announced (correct) public key — read it via a None-key ingest first.
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(
                a.as_ptr(),
                a.len(),
                &dest,
                0,
                core::ptr::null(),
                &mut ev,
            )
        },
        RsHandheldStatus::Ok
    );
    let correct_key = ev.public_key;

    // Same key -> OK.
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(a.as_ptr(), a.len(), &dest, 0, &correct_key, &mut ev)
        },
        RsHandheldStatus::Ok
    );
    // Different key for this destination -> ErrCrypto (PublicKeyChanged).
    let mut wrong_key = correct_key;
    wrong_key[0] ^= 0xFF;
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_ingest(a.as_ptr(), a.len(), &dest, 0, &wrong_key, &mut ev)
        },
        RsHandheldStatus::ErrCrypto
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

// ---- Transport / packet IO ----

const TRANSPORT_ID: [u8; 16] = [0x42; 16];

/// The non-compiled profile id (any id != ACTIVE_PROFILE must be refused).
const OTHER_PROFILE: i32 = if ACTIVE_PROFILE == RS_HANDHELD_PROFILE_SMALL {
    RS_HANDHELD_PROFILE_MICRO
} else {
    RS_HANDHELD_PROFILE_SMALL
};

/// Heap slot for the active profile's node + placement open_transport into it.
/// The returned Box OWNS the node memory: keep it alive until after shutdown.
fn open_transport_into(ctx: *mut RsHandheldRns, enabled: i32) -> Box<MaybeUninit<ActiveNode>> {
    let mut buf = Box::<ActiveNode>::new_uninit();
    assert_eq!(
        unsafe {
            rs_handheld_rns_open_transport(
                ctx,
                ACTIVE_PROFILE,
                &TRANSPORT_ID,
                enabled,
                buf.as_mut_ptr().cast::<u8>(),
                core::mem::size_of::<ActiveNode>(),
            )
        },
        RsHandheldStatus::Ok
    );
    buf
}

#[test]
fn transport_size_align_report_active_profile_only() {
    assert_eq!(
        rs_handheld_rns_transport_size(ACTIVE_PROFILE),
        core::mem::size_of::<ActiveNode>()
    );
    assert_eq!(
        rs_handheld_rns_transport_align(ACTIVE_PROFILE),
        core::mem::align_of::<ActiveNode>()
    );
    // Wrong-artifact tripwire: a profile this .a was not compiled with reports 0.
    assert_eq!(rs_handheld_rns_transport_size(OTHER_PROFILE), 0);
    assert_eq!(rs_handheld_rns_transport_align(OTHER_PROFILE), 0);
    assert_eq!(rs_handheld_rns_transport_size(7), 0);
    assert_eq!(rs_handheld_rns_transport_align(-1), 0);
}

#[test]
fn ffi_context_and_micro_node_stay_inside_cardputer_heap_budget() {
    let context = core::mem::size_of::<RsHandheldRns>();
    let small_node = core::mem::size_of::<rns_lite_core::SmallNode>();
    let micro_node = core::mem::size_of::<rns_lite_core::MicroNode>();
    println!("ffi context={context} B, small node={small_node} B, micro node={micro_node} B");
    assert!(context <= 9 * 1024);
    assert!(context + micro_node <= 41 * 1024);
}

#[test]
fn open_transport_validates_profile_buffer_alignment_and_flips_status() {
    let ctx = loaded_ctx();
    let mut buf = Box::<ActiveNode>::new_uninit();
    let p = buf.as_mut_ptr().cast::<u8>();
    let n = core::mem::size_of::<ActiveNode>();

    // Wrong artifact profile -> ErrUnsupported (checked before the buffer).
    assert_eq!(
        unsafe { rs_handheld_rns_open_transport(ctx, OTHER_PROFILE, &TRANSPORT_ID, 1, p, n) },
        RsHandheldStatus::ErrUnsupported
    );
    // Null / undersized / misaligned buffer -> ErrInvalidArg (nothing written).
    assert_eq!(
        unsafe {
            rs_handheld_rns_open_transport(
                ctx,
                ACTIVE_PROFILE,
                &TRANSPORT_ID,
                1,
                core::ptr::null_mut(),
                n,
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe { rs_handheld_rns_open_transport(ctx, ACTIVE_PROFILE, &TRANSPORT_ID, 1, p, n - 1) },
        RsHandheldStatus::ErrInvalidArg
    );
    // align_of(ActiveNode) >= 2, so p+1 is always misaligned (rejected before any write,
    // so the 1-byte-short view is never touched).
    assert_eq!(
        unsafe {
            rs_handheld_rns_open_transport(ctx, ACTIVE_PROFILE, &TRANSPORT_ID, 1, p.add(1), n)
        },
        RsHandheldStatus::ErrInvalidArg
    );

    // Status: identity loaded but no node -> NOT_READY; flips to OK on open.
    assert_eq!(
        unsafe { rs_handheld_rns_status(ctx) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_open_transport(ctx, ACTIVE_PROFILE, &TRANSPORT_ID, 1, p, n) },
        RsHandheldStatus::Ok
    );
    assert_eq!(unsafe { rs_handheld_rns_status(ctx) }, RsHandheldStatus::Ok);
    unsafe { rs_handheld_rns_shutdown(ctx) };
    drop(buf); // freed only after shutdown, per the ownership contract
}

#[test]
fn init_node_in_equals_by_value_construction() {
    // Placement construction must be indistinguishable from new() for a tiny profile
    // AND both shipped profiles (PartialEq over every table/field).
    type Tiny = LiteNode<4, 4, 2, 2, 2, 2, 4>;
    let mut cfg = LiteConfig::ESP32_LORA_TRANSPORT_SMALL;
    cfg.table_caps.path_entries = 4;
    cfg.table_caps.packet_hashes = 4;
    cfg.table_caps.announce_entries = 2;
    cfg.table_caps.reverse_entries = 2;
    cfg.table_caps.link_entries = 2;
    cfg.table_caps.path_request_tags = 2;
    cfg.table_caps.queued_announces_per_interface = 2;
    cfg.table_caps.tx_queue_depth = 2;
    let mut slot = MaybeUninit::<Tiny>::uninit();
    let placed = init_node_in(&mut slot, cfg, TRANSPORT_ID).unwrap();
    assert!(*placed == Tiny::new(cfg, TRANSPORT_ID).unwrap());

    let small_cfg = LiteConfig::ESP32_LORA_TRANSPORT_SMALL;
    let mut small_slot = Box::<rns_lite_core::SmallNode>::new_uninit();
    let placed = init_node_in(&mut small_slot, small_cfg, TRANSPORT_ID).unwrap();
    assert!(*placed == rns_lite_core::SmallNode::new(small_cfg, TRANSPORT_ID).unwrap());

    let micro_cfg = LiteConfig::ESP32_LORA_TRANSPORT_MICRO;
    let mut micro_slot = Box::<rns_lite_core::MicroNode>::new_uninit();
    let placed = init_node_in(&mut micro_slot, micro_cfg, TRANSPORT_ID).unwrap();
    assert!(*placed == rns_lite_core::MicroNode::new(micro_cfg, TRANSPORT_ID).unwrap());

    // Validation runs BEFORE any write: caps beyond the const capacities are rejected.
    let mut bad_slot = MaybeUninit::<Tiny>::uninit();
    assert_eq!(
        init_node_in(
            &mut bad_slot,
            LiteConfig::ESP32_LORA_TRANSPORT_SMALL,
            TRANSPORT_ID
        )
        .unwrap_err(),
        TransportError::CapacityTooSmall
    );
}

#[test]
fn placement_writer_covers_every_lite_field() {
    // COMPILE-TIME drift guard: exhaustive (no `..`) destructures fail the build if
    // rns-lite-core adds a LiteNode/table/Queue field init_node_in does not write.
    use rns_lite_core::tables::{
        AnnounceCache, AnnounceSchedule, LinkTable, PacketHashTable, PathTable, Queue,
        RequestTagTable, ReverseTable,
    };
    type Tiny = LiteNode<1, 1, 1, 1, 1, 1, 1>;
    let node = Tiny::new_const(LiteConfig::ESP32_LORA_TRANSPORT_SMALL, TRANSPORT_ID);
    let LiteNode {
        config: _,
        transport_id: _,
        own_destinations: _,
        announce_admission: _,
        known_destinations: _,
        packet_hashes,
        paths,
        announce_cache,
        announce_schedule,
        reverse,
        links,
        request_tags,
        outbound,
        stats: _,
    } = node;
    let PacketHashTable { entries: _ } = packet_hashes;
    let PathTable { entries: _ } = paths;
    let AnnounceCache { entries: _ } = announce_cache;
    let AnnounceSchedule { entries: _ } = announce_schedule;
    let ReverseTable { entries: _ } = reverse;
    let LinkTable { entries: _ } = links;
    let RequestTagTable { entries: _ } = request_tags;
    let Queue {
        entries: _,
        head: _,
        len: _,
    } = outbound;
}

/// Frame an announce payload as a Header1 broadcast announce packet (what the LoRa driver
/// would deliver), so the transport FFI can ingest it.
fn frame_announce(payload: &[u8], dest: &[u8; 16]) -> Vec<u8> {
    frame_announce_with_route(payload, dest, 0, None, PacketContext::None)
}

fn frame_announce_with_route(
    payload: &[u8],
    dest: &[u8; 16],
    hops: u8,
    transport_id: Option<[u8; 16]>,
    context: PacketContext,
) -> Vec<u8> {
    use rns_lite_core::wire::{
        DestinationType, HeaderType, PacketFlags, PacketHeader, TransportType, build_packet,
    };
    let (header_type, transport_type) = if transport_id.is_some() {
        (HeaderType::Header2, TransportType::Transport)
    } else {
        (HeaderType::Header1, TransportType::Broadcast)
    };
    let header = PacketHeader {
        flags: PacketFlags {
            header_type,
            context_flag: false,
            transport_type,
            destination_type: DestinationType::Single,
            packet_type: PacketType::Announce,
        },
        hops,
        transport_id,
        destination_hash: *dest,
        context,
    };
    build_packet(header, payload).unwrap().as_slice().to_vec()
}

#[test]
fn transport_not_ready_before_open() {
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    let raw = [0u8; 19];
    let mut action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                raw.as_ptr(),
                raw.len(),
                1,
                0,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_tick(ctx, 0) },
        RsHandheldStatus::ErrNotReady
    );
    let mut has = -1;
    assert_eq!(
        unsafe { rs_handheld_rns_has_path(ctx, &TRANSPORT_ID, 0, &mut has) },
        RsHandheldStatus::ErrNotReady
    );
    let mut count = 99u32;
    assert_eq!(
        unsafe { rs_handheld_rns_path_count(ctx, 0, &mut count) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_path_count(core::ptr::null(), 0, &mut count) },
        RsHandheldStatus::ErrInvalidArg
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn packet_ingest_rejects_unknown_interface_mode() {
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    let raw = [0u8; 19];
    let mut action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest_with_mode(
                ctx,
                raw.as_ptr(),
                raw.len(),
                1,
                0x7f,
                0,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn packet_ingest_drops_own_announce_echo() {
    // The node's own lxmf.delivery announce, echoed back (as by a neighbouring relay):
    // Dropped by the own-destination guard — no path learned, no contact surfaced.
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let (payload, dest) = announce(ctx, core::ptr::null(), &rat);
    let mut pkt = frame_announce(&payload, &dest);
    pkt[1] = 1; // echoed copy: hops bumped by the relay

    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_ptr(),
                pkt.len(),
                7,
                1000,
                &mut action,
                &mut ev,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action, 8, "own announce echo must be Dropped");
    let mut has = 0;
    assert_eq!(
        unsafe { rs_handheld_rns_has_path(ctx, &dest, 1000, &mut has) },
        RsHandheldStatus::Ok
    );
    assert_eq!(has, 0, "no phantom self-path");
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn packet_ingest_reports_own_path_request_in_transport_posture() {
    // With transport_enabled=1 (relay+endpoint), a path request for OUR destination
    // must surface as INGEST_PATH_REQUEST_SELF (host re-announces) — never forward a
    // spurious self path-request (trusted outbound.rs / Python Transport.py:2969).
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    let mut dest = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut dest) },
        RsHandheldStatus::Ok
    );
    use rns_lite_core::wire::{
        DestinationType, HeaderType, PacketFlags, PacketHeader, TransportType, build_packet,
    };
    let mut payload = Vec::new();
    payload.extend_from_slice(&dest);
    payload.extend_from_slice(&[0x22u8; 16]); // tag
    let header = PacketHeader {
        flags: PacketFlags {
            header_type: HeaderType::Header1,
            context_flag: false,
            transport_type: TransportType::Broadcast,
            destination_type: DestinationType::Plain,
            packet_type: PacketType::Data,
        },
        hops: 0,
        transport_id: None,
        destination_hash: rns_lite_core::path_request_destination(),
        context: PacketContext::None,
    };
    let pkt = build_packet(header, &payload).unwrap();
    let mut action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_slice().as_ptr(),
                pkt.as_slice().len(),
                7,
                1000,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action, INGEST_PATH_REQUEST_SELF);
    // Nothing spurious queued (the pre-fix behavior forwarded a self path-request).
    assert_eq!(
        unsafe { rs_handheld_rns_tick(ctx, 5000) },
        RsHandheldStatus::Ok
    );
    let mut out = [0u8; 640];
    let mut outlen = 1usize;
    let mut iface = 0u8;
    let mut reason = -1i32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                out.as_mut_ptr(),
                out.len(),
                &mut outlen,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(outlen, 0);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn packet_ingest_rejects_hops_at_pathfinder_m_for_all_types() {
    // Python 1.3.8 parse-rejects raw hops >= 128; neither the relay core nor the
    // endpoint post-processing (contact surfacing / local classification) may accept it.
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0); // endpoint posture
    let peer = LocalIdentity::from_private_key(&[0x5Eu8; 64]);
    let rh = compose_random_hash(&RNG_SEED, UNIX_SECS);
    let dest = peer.destination_hash("lxmf.delivery");
    let mut adata = [0u8; 600];
    let n = peer
        .create_announce_named("lxmf.delivery", &rh, None, &[], &mut adata)
        .unwrap();
    let mut pkt = frame_announce(&adata[..n], &dest);
    pkt[1] = 128; // raw hops byte at PATHFINDER_M

    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    let mut local: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_ptr(),
                pkt.len(),
                7,
                1000,
                &mut action,
                &mut ev,
                &mut local,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action, 8, "hops >= 128 must be Dropped");
    assert_eq!(ev.identity_hash, [0u8; 16], "no contact surfaced");
    let mut has = 0;
    assert_eq!(
        unsafe { rs_handheld_rns_has_path(ctx, &dest, 1000, &mut has) },
        RsHandheldStatus::Ok
    );
    assert_eq!(has, 0);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn packet_ingest_suppresses_non_lxmf_delivery_aspect_contact() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    // A valid announce for a DIFFERENT aspect of the same identity (e.g. lxst.telephony):
    // its path is learned, but it must NOT be surfaced as a contact.
    let prv: [u8; 64] = unhex(INCREMENTING);
    let id = LocalIdentity::from_private_key(&prv);
    let rh = compose_random_hash(&RNG_SEED, UNIX_SECS);
    let dest = id.destination_hash("lxst.telephony");
    let mut adata = [0u8; 600];
    let n = id
        .create_announce_named("lxst.telephony", &rh, None, &[], &mut adata)
        .unwrap();
    let pkt = frame_announce(&adata[..n], &dest);

    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_ptr(),
                pkt.len(),
                7,
                1000,
                &mut action,
                &mut ev,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    // Non-delivery aspect -> ANNOUNCE_OTHER (not LearnedAnnounce/Scheduled); event unfilled.
    assert_eq!(action, INGEST_ANNOUNCE_OTHER);
    assert_eq!(ev.identity_hash, [0u8; 16]);
    // But the path IS learned (transport learns every aspect, for every aspect).
    let mut has = 0;
    assert_eq!(
        unsafe { rs_handheld_rns_has_path(ctx, &dest, 1000, &mut has) },
        RsHandheldStatus::Ok
    );
    assert_eq!(has, 1);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn transport_ingest_learns_path_and_rebroadcasts() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    // A PEER announce (distinct identity): the node's own delivery destination is
    // registered at open, and an own-announce echo is now Dropped (guard test below).
    let peer = LocalIdentity::from_private_key(&[0x5Eu8; 64]);
    let rh = compose_random_hash(&RNG_SEED, UNIX_SECS);
    let dest = peer.destination_hash("lxmf.delivery");
    let mut adata = [0u8; 600];
    let n = peer
        .create_announce_named("lxmf.delivery", &rh, None, &rat, &mut adata)
        .unwrap();
    let payload = adata[..n].to_vec();
    let pkt = frame_announce(&payload, &dest);

    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_ptr(),
                pkt.len(),
                7,
                1000,
                &mut action,
                &mut ev,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    // Announce action: LearnedAnnounce(2) or ScheduledAnnounce(3); the event is filled.
    assert!(action == 2 || action == 3, "action was {action}");
    assert_eq!(&ev.identity_hash, peer.identity_hash());
    assert_eq!(&ev.app_data[..ev.app_data_len as usize], &rat[..]);

    let mut has = 0;
    assert_eq!(
        unsafe { rs_handheld_rns_has_path(ctx, &dest, 1000, &mut has) },
        RsHandheldStatus::Ok
    );
    assert_eq!(has, 1);

    // Live path count reflects the learned (unexpired) entry, and drops back to 0
    // once the path ages out (the cumulative learned_announces counter does not).
    let mut count = 0u32;
    assert_eq!(
        unsafe { rs_handheld_rns_path_count(ctx, 1000, &mut count) },
        RsHandheldStatus::Ok
    );
    assert_eq!(count, 1);

    // Dispatch the scheduled rebroadcast (jitter window [0,500)), then poll it.
    assert_eq!(
        unsafe { rs_handheld_rns_tick(ctx, 3000) },
        RsHandheldStatus::Ok
    );
    let mut out = [0u8; 600];
    let mut len = 0usize;
    let mut iface = 0u8;
    let mut reason = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                out.as_mut_ptr(),
                out.len(),
                &mut len,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(len > 0, "expected a rebroadcast frame");
    assert_eq!(reason, 0); // RS_HANDHELD_TX_ANNOUNCE_REBROADCAST
    assert_eq!(iface, 7);

    let mut stats: RsHandheldTransportStats = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { rs_handheld_rns_transport_stats(ctx, &mut stats) },
        RsHandheldStatus::Ok
    );
    assert_eq!(stats.accepted, 1);
    assert_eq!(stats.learned_announces, 1);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn route_api_selects_interface_and_header_from_live_path() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);

    let one_hop = LocalIdentity::from_private_key(&[0x61; 64]);
    let one_dest = one_hop.lxmf_delivery_hash();
    let mut announce_data = [0u8; 600];
    let one_len = one_hop
        .create_announce_named(
            "lxmf.delivery",
            &compose_random_hash(&[1; 5], 1),
            None,
            &[],
            &mut announce_data,
        )
        .unwrap();
    let one_packet = frame_announce(&announce_data[..one_len], &one_dest);
    let mut action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                one_packet.as_ptr(),
                one_packet.len(),
                7,
                1000,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(action == 2 || action == 3);

    let two_hop = LocalIdentity::from_private_key(&[0x62; 64]);
    let two_dest = two_hop.lxmf_delivery_hash();
    let two_len = two_hop
        .create_announce_named(
            "lxmf.delivery",
            &compose_random_hash(&[2; 5], 2),
            None,
            &[],
            &mut announce_data,
        )
        .unwrap();
    let next_hop = [0xA5; 16];
    let two_packet = frame_announce_with_route(
        &announce_data[..two_len],
        &two_dest,
        1,
        Some(next_hop),
        PacketContext::None,
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                two_packet.as_ptr(),
                two_packet.len(),
                9,
                1001,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(action == 2 || action == 3);

    let mut route = RsHandheldRoute {
        kind: -1,
        interface_id: 0,
        header_type: 0,
        next_hop: [0; 16],
        hops: 0,
    };
    assert_eq!(
        unsafe { rs_handheld_rns_route(ctx, &one_dest, 1001, &mut route) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        route,
        RsHandheldRoute {
            kind: ROUTE_DIRECT,
            interface_id: 7,
            header_type: 0,
            next_hop: [0; 16],
            hops: 1,
        }
    );

    assert_eq!(
        unsafe { rs_handheld_rns_route(ctx, &two_dest, 1001, &mut route) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        route,
        RsHandheldRoute {
            kind: ROUTE_DIRECT,
            interface_id: 9,
            header_type: 1,
            next_hop,
            hops: 2,
        }
    );

    assert_eq!(
        unsafe { rs_handheld_rns_route(ctx, &[0xEE; 16], 1001, &mut route) },
        RsHandheldStatus::Ok
    );
    assert_eq!(route.kind, ROUTE_BROADCAST);

    assert_eq!(
        unsafe { rs_handheld_rns_route(ctx, &one_dest, u64::MAX, &mut route) },
        RsHandheldStatus::Ok
    );
    assert_eq!(route.kind, ROUTE_BROADCAST);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn known_destination_ffi_roundtrips_complete_blob() {
    let peer = LocalIdentity::from_private_key(&[0x63; 64]);
    let destination = peer.lxmf_delivery_hash();
    let public_key = peer.public_key();

    let first = loaded_ctx();
    let _first_node = open_transport_into(first, 0);
    let mut changed = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_learn(first, &destination, public_key, 1000, &mut changed)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_learn(first, &destination, public_key, 2000, &mut changed)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 0);

    let mut count = 0;
    assert_eq!(
        unsafe { rs_handheld_rns_known_dest_count(first, &mut count) },
        RsHandheldStatus::Ok
    );
    assert_eq!(count, 1);
    let mut recalled = [0u8; 64];
    let mut found = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_recall(first, &destination, &mut recalled, &mut found)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!((found, recalled), (1, *public_key));

    let mut blob = vec![0u8; KnownDestinations::<128>::blob_capacity()];
    let mut blob_len = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_export(first, blob.as_mut_ptr(), blob.len(), &mut blob_len)
        },
        RsHandheldStatus::Ok
    );

    let second = loaded_ctx();
    let _second_node = open_transport_into(second, 0);
    assert_eq!(
        unsafe { rs_handheld_rns_known_dest_import(second, blob.as_ptr(), blob_len, 3000) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_recall(second, &destination, &mut recalled, &mut found)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!((found, recalled), (1, *public_key));

    unsafe { rs_handheld_rns_shutdown(first) };
    unsafe { rs_handheld_rns_shutdown(second) };
}

#[test]
fn transport_announce_budget_drops_burst_but_exempts_path_responses() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);

    let ingest_announce = |seed: u8, context: PacketContext, now_ms: u64| {
        let peer = LocalIdentity::from_private_key(&[seed; 64]);
        let destination = peer.lxmf_delivery_hash();
        let mut payload = [0u8; 600];
        let len = peer
            .create_announce_named(
                "lxmf.delivery",
                &compose_random_hash(&[seed; 5], seed as u64),
                None,
                &[],
                &mut payload,
            )
            .unwrap();
        let packet = frame_announce_with_route(&payload[..len], &destination, 0, None, context);
        let mut action = -1;
        assert_eq!(
            unsafe {
                rs_handheld_rns_packet_ingest(
                    ctx,
                    packet.as_ptr(),
                    packet.len(),
                    1,
                    now_ms,
                    &mut action,
                    core::ptr::null_mut(),
                    core::ptr::null_mut(),
                )
            },
            RsHandheldStatus::Ok
        );
        action
    };

    for seed in 0x70..0x73 {
        let action = ingest_announce(seed, PacketContext::None, 0);
        assert!(action == 2 || action == 3, "action was {action}");
    }
    assert_eq!(ingest_announce(0x73, PacketContext::None, 0), 8);
    let exempt = ingest_announce(0x74, PacketContext::PathResponse, 0);
    assert!(exempt == 2 || exempt == 3, "action was {exempt}");

    let mut stats: RsHandheldTransportStats = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { rs_handheld_rns_transport_stats(ctx, &mut stats) },
        RsHandheldStatus::Ok
    );
    assert_eq!(stats.announces_rate_dropped, 1);

    assert_eq!(
        unsafe { rs_handheld_rns_set_announce_budget(ctx, 1, 1) },
        RsHandheldStatus::Ok
    );
    let first = ingest_announce(0x75, PacketContext::None, 1000);
    assert!(first == 2 || first == 3, "action was {first}");
    assert_eq!(ingest_announce(0x76, PacketContext::None, 1000), 8);
    assert_eq!(
        unsafe { rs_handheld_rns_transport_stats(ctx, &mut stats) },
        RsHandheldStatus::Ok
    );
    assert_eq!(stats.announces_rate_dropped, 2);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn transport_request_path_emits_wire_packet() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    let dest = [0x11u8; 16];
    let tag = [0x22u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_request_path(ctx, &dest, &tag, 3, 1000) },
        RsHandheldStatus::Ok
    );
    let mut out = [0u8; 600];
    let mut len = 0usize;
    let mut iface = 0u8;
    let mut reason = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                out.as_mut_ptr(),
                out.len(),
                &mut len,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(len > 0);
    assert_eq!(reason, 3); // RS_HANDHELD_TX_PATH_REQUEST
    assert_eq!(iface, 3);
    let view = PacketView::parse(&out[..len]).unwrap();
    assert_eq!(view.header.flags.packet_type, PacketType::Data);
    // payload = dest(16) || transport_id(16, transport_enabled) || tag(16)
    assert_eq!(&view.payload[..16], &dest);
    assert_eq!(&view.payload[16..32], &TRANSPORT_ID);
    assert_eq!(&view.payload[32..48], &tag);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn transport_request_path_endpoint_only_omits_transport_id() {
    // The actual rsDeck endpoint shape: transport_enabled = 0 -> no transport_id in the payload.
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let dest = [0x11u8; 16];
    let tag = [0x22u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_request_path(ctx, &dest, &tag, 1, 1000) },
        RsHandheldStatus::Ok
    );
    let mut out = [0u8; 600];
    let mut len = 0usize;
    let mut iface = 0u8;
    let mut reason = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                out.as_mut_ptr(),
                out.len(),
                &mut len,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::Ok
    );
    let view = PacketView::parse(&out[..len]).unwrap();
    // payload = dest(16) || tag(16) — NO transport_id when endpoint-only.
    assert_eq!(view.payload.len(), 32);
    assert_eq!(&view.payload[..16], &dest);
    assert_eq!(&view.payload[16..32], &tag);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn poll_outbound_undersized_buffer_preserves_frame() {
    // An undersized `out` must return ErrCapacity WITHOUT consuming the queued frame.
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 1);
    let dest = [0x11u8; 16];
    let tag = [0x22u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_request_path(ctx, &dest, &tag, 1, 1000) },
        RsHandheldStatus::Ok
    );
    let mut small = [0u8; 8];
    let mut len = 999usize;
    let mut iface = 0u8;
    let mut reason = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                small.as_mut_ptr(),
                small.len(),
                &mut len,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::ErrCapacity
    );
    // The frame is still queued — a correctly-sized poll retrieves it.
    let mut big = [0u8; 600];
    assert_eq!(
        unsafe {
            rs_handheld_rns_poll_outbound(
                ctx,
                big.as_mut_ptr(),
                big.len(),
                &mut len,
                &mut iface,
                &mut reason,
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(len > 0);
    assert_eq!(reason, 3); // RS_HANDHELD_TX_PATH_REQUEST
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

// ---- LXMF single-frame — pinned vs vectors/lxmf_vectors.json (RNS 1.3.8 / LXMF 1.0.1) ----

fn unhex_vec(s: &str) -> Vec<u8> {
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}
// Source = the "incrementing" identity; its 64-byte PUBLIC key (identity vector).
const INCREMENTING_PUB: &str = "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
const LXMF_RECIPIENT_PRV: &str = "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f";
const LXMF_RECIPIENT_PUB: &str = "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a174553b456dddfc6908ecab1c101fe6ab21e2baa0617795b7d43a63482993fd5";
const LXMF_DEST_HASH: &str = "cf0b2a4a8d2a0b6978b71290da7cc80e";
const LXMF_MESSAGE_ID: &str = "dceab683f560aad56d128fc7051ceb41ef4cd935430df0eb089a4ea0095be1a4";
const LXMF_ECIES_DET: &str = "7b0d47d93427f8311160781c7c733fd89f88970aef490d8aa0ee19a4cb8a1b14444444444444444444444444444444440d7c1dee5eb3705ffb1071c5a2793fbd560c3b182efc727d9941535dcbc9b12df1985a5fb1084d7efc4e2d3cf97ff76dee026669294ec05d51bf843cd4c3b998b2e66be6b571f6a02cf5fb88480a474b389ba269ae44cf74ac6f699680c83c208d2d3be4fcf5ba8de6b2b18b20a1c90bc0324fa01763f8468836445c617b9ab8b87bda7cf9e4cc0760763516a0ef8f69";
const LXMF_PYTHON_BLOB: &str = "d762902286b2608284e235803a959fb9657e4c1c308a6ced5ec60fb28c99ed6d4a99af0f98a241ac49f47b42c70a7aa64bfe86f5f607888314ab81952f37499f69a03bd0764494145eb5301582335f2aa2e679139d8c9aa96b5f0d674b401c0355e2e399d65eee5f9e92bd360d5c3a44cbeb3d822c4c384392ee25121b0cf8f49867f4f9f24a7a0292c35d0a8ba213ee80e3f81ae94be5601ab9c90703b095c2e9871d5c749f443d5c5750d4331a2fe30fb0a94b761c288f7921c7972dde8f4c";

#[test]
fn lxmf_build_byte_exact_with_python() {
    // Source = the incrementing identity (loaded_ctx); recipient = the pinned key.
    let ctx = loaded_ctx();
    let recipient_pub: [u8; 64] = unhex(LXMF_RECIPIENT_PUB);
    let title = b"Hi";
    let content = b"hello world";
    let mut out = [0u8; 600];
    let mut out_len = 0usize;
    let mut dest = [0u8; 16];
    let mut mid = [0u8; 32];
    let st = unsafe {
        rs_handheld_rns_lxmf_build(
            ctx,
            &recipient_pub,
            1234567890.5,
            1_234_567_890,
            0,
            title.as_ptr(),
            title.len(),
            content.as_ptr(),
            content.len(),
            &[0x33; 32],
            &[0x44; 16],
            out.as_mut_ptr(),
            out.len(),
            &mut out_len,
            &mut dest,
            &mut mid,
        )
    };
    assert_eq!(st, RsHandheldStatus::Ok);
    assert_eq!(to_hex(&out[..out_len]), LXMF_ECIES_DET);
    assert_eq!(to_hex(&dest), LXMF_DEST_HASH);
    assert_eq!(to_hex(&mid), LXMF_MESSAGE_ID);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn lxmf_parse_decrypts_and_validates_python_message() {
    // Load the RECIPIENT identity; parse a Python-RNS-encrypted message from the source.
    let prv: [u8; 64] = unhex(LXMF_RECIPIENT_PRV);
    let source_pub: [u8; 64] = unhex(INCREMENTING_PUB);
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(ctx, &prv) },
        RsHandheldStatus::Ok
    );
    let blob = unhex_vec(LXMF_PYTHON_BLOB);
    let mut msg: RsHandheldLxmfMessage = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse(ctx, blob.as_ptr(), blob.len(), &source_pub, &mut msg)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(to_hex(&msg.message_id), LXMF_MESSAGE_ID);
    assert_eq!(&msg.title[..msg.title_len as usize], b"Hi");
    assert_eq!(&msg.content[..msg.content_len as usize], b"hello world");
    assert_eq!(msg.timestamp, 1234567890.5);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn lxmf_roundtrip_build_then_parse() {
    // Source ctx builds; recipient ctx parses (its own decrypt + validate).
    let src_ctx = loaded_ctx();
    let recipient_pub: [u8; 64] = unhex(LXMF_RECIPIENT_PUB);
    let src_pub: [u8; 64] = unhex(INCREMENTING_PUB);
    let mut out = [0u8; 600];
    let mut out_len = 0usize;
    let mut dest = [0u8; 16];
    let mut mid = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                src_ctx,
                &recipient_pub,
                42.0,
                42,
                0,
                core::ptr::null(),
                0,
                b"body".as_ptr(),
                4,
                &[0x01; 32],
                &[0x02; 16],
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
                &mut dest,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );

    let recip_prv: [u8; 64] = unhex(LXMF_RECIPIENT_PRV);
    let mut rcp_ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut rcp_ctx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(rcp_ctx, &recip_prv) },
        RsHandheldStatus::Ok
    );
    let mut msg: RsHandheldLxmfMessage = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe { rs_handheld_rns_lxmf_parse(rcp_ctx, out.as_ptr(), out_len, &src_pub, &mut msg) },
        RsHandheldStatus::Ok
    );
    assert_eq!(msg.message_id, mid);
    assert_eq!(msg.title_len, 0);
    assert_eq!(&msg.content[..msg.content_len as usize], b"body");
    unsafe { rs_handheld_rns_shutdown(src_ctx) };
    unsafe { rs_handheld_rns_shutdown(rcp_ctx) };
}

// ---- Proof / dedup — pinned vs vectors/proof_vectors.json (RNS 1.3.8) ----

const PROOF_HASH: &str = "aca7bfd99e41af775d2bb28d5262ff4a30ae6ad052e25caf2cacc86b7d9be316";
const PROOF_IMPLICIT: &str = "3f1262730f052ad87821c1883627130995aa4912fff993fa0e5feb33f6d64477c88cbdfe72e443334b4d3b1d7275ae543241575725ea2d3c04f6f05598be560f";

#[test]
fn proof_build_byte_exact_and_validate() {
    let ctx = loaded_ctx(); // the incrementing identity (the prover)
    let packet_hash: [u8; 32] = unhex(PROOF_HASH);
    let pub_key: [u8; 64] = unhex(INCREMENTING_PUB);

    // Implicit (Reticulum default): byte-exact with the Python vector.
    let mut out = [0u8; 96];
    let mut out_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_proof_build(
                ctx,
                &packet_hash,
                1,
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(to_hex(&out[..out_len]), PROOF_IMPLICIT);

    // The proof validates against the prover's public key.
    let mut valid = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_proof_validate(
                &pub_key,
                &packet_hash,
                out.as_ptr(),
                out_len,
                &mut valid,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 1);

    // Explicit form validates too; a wrong packet hash does not.
    let mut elen = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_proof_build(
                ctx,
                &packet_hash,
                0,
                out.as_mut_ptr(),
                out.len(),
                &mut elen,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(elen, 96);
    assert_eq!(
        unsafe {
            rs_handheld_rns_proof_validate(&pub_key, &packet_hash, out.as_ptr(), elen, &mut valid)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 1);
    let wrong_hash = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_proof_validate(&pub_key, &wrong_hash, out.as_ptr(), elen, &mut valid)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 0);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn seen_message_dedup() {
    let mut ctx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut ctx) },
        RsHandheldStatus::Ok
    );
    let id = [0xaau8; 32];
    let mut seen = -1;
    for _ in 0..2 {
        assert_eq!(
            unsafe { rs_handheld_rns_has_seen_message(ctx, &id, &mut seen) },
            RsHandheldStatus::Ok
        );
        assert_eq!(seen, 0);
    }
    let mut is_new = -1;
    assert_eq!(
        unsafe { rs_handheld_rns_seen_message(ctx, &id, &mut is_new) },
        RsHandheldStatus::Ok
    );
    assert_eq!(is_new, 1); // first sight
    assert_eq!(
        unsafe { rs_handheld_rns_has_seen_message(ctx, &id, &mut seen) },
        RsHandheldStatus::Ok
    );
    assert_eq!(seen, 1);
    assert_eq!(
        unsafe { rs_handheld_rns_has_seen_message(core::ptr::null(), &id, &mut seen) },
        RsHandheldStatus::ErrInvalidArg
    );
    assert_eq!(
        unsafe { rs_handheld_rns_seen_message(ctx, &id, &mut is_new) },
        RsHandheldStatus::Ok
    );
    assert_eq!(is_new, 0); // duplicate
    let other = [0xbbu8; 32];
    assert_eq!(
        unsafe { rs_handheld_rns_seen_message(ctx, &other, &mut is_new) },
        RsHandheldStatus::Ok
    );
    assert_eq!(is_new, 1);

    // Seeding (boot replay from storage): a seeded id is then recognised as a duplicate.
    let seeded = [0xddu8; 32];
    assert_eq!(
        unsafe { rs_handheld_rns_seed_seen_message(ctx, &seeded) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_seen_message(ctx, &seeded, &mut is_new) },
        RsHandheldStatus::Ok
    );
    assert_eq!(is_new, 0); // already seeded -> duplicate
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

// Full link handshake through the FFI: initiator (ctx) <-> responder (a second loaded ctx,
// standing in for a remote peer), key agreement, then a frame each way.
#[test]
fn link_handshake_and_session_through_ffi() {
    // Two endpoints. The responder is the destination the initiator links to.
    let responder = loaded_ctx(); // identity = INCREMENTING; its lxmf.delivery dest is the target
    let mut resp_pub = [0u8; 64];
    let resp_prv: [u8; 64] = unhex(INCREMENTING);
    assert_eq!(
        unsafe { rs_handheld_rns_validate_identity(&resp_prv, &mut [0u8; 16], &mut resp_pub) },
        RsHandheldStatus::Ok
    );
    let mut dest_hash = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(responder, &mut dest_hash) },
        RsHandheldStatus::Ok
    );

    // 1. Initiator builds a LINKREQUEST with ephemeral keys.
    let init_x = [0x33u8; 32];
    let init_ed = [0x44u8; 32];
    let mut req = [0u8; RS_HANDHELD_LINK_REQUEST_LEN];
    let mut req_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_request_build(
                &init_x,
                &init_ed,
                1,
                500,
                req.as_mut_ptr(),
                req.len(),
                &mut req_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(req_len, RS_HANDHELD_LINK_REQUEST_LEN);

    // Both sides compute the same link id.
    let mut link_id = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_link_id(&dest_hash, req.as_ptr(), req_len, &mut link_id) },
        RsHandheldStatus::Ok
    );

    // 2. Responder parses the request, derives its session key, builds the proof.
    let mut init_x_pub = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_request_parse(
                req.as_ptr(),
                req_len,
                &mut init_x_pub,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    let resp_x = [0x55u8; 32];
    let mut resp_key = [0u8; 64];
    assert_eq!(
        unsafe { rs_handheld_rns_link_derive(&resp_x, &init_x_pub, &link_id, &mut resp_key) },
        RsHandheldStatus::Ok
    );
    let mut proof = [0u8; RS_HANDHELD_LINK_PROOF_LEN];
    let mut proof_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_proof_build(
                responder,
                &resp_x,
                &link_id,
                1,
                500,
                proof.as_mut_ptr(),
                proof.len(),
                &mut proof_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(proof_len, RS_HANDHELD_LINK_PROOF_LEN);

    // 3. Initiator validates the proof (knows the destination identity's public key), extracts
    //    the responder's ephemeral pub, derives the matching session key.
    let mut valid = 0i32;
    let mut resp_x_pub = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_proof_validate(
                &resp_pub,
                &link_id,
                proof.as_ptr(),
                proof_len,
                &mut resp_x_pub,
                &mut valid,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 1);
    let mut init_key = [0u8; 64];
    assert_eq!(
        unsafe { rs_handheld_rns_link_derive(&init_x, &resp_x_pub, &link_id, &mut init_key) },
        RsHandheldStatus::Ok
    );
    assert_eq!(init_key, resp_key); // both ends agree

    // 4. A frame each way over the session key.
    let msg = b"hello over the link";
    let mut ct = [0u8; 128];
    let mut ct_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_encrypt(
                &init_key,
                msg.as_ptr(),
                msg.len(),
                &[0x66; 16],
                ct.as_mut_ptr(),
                ct.len(),
                &mut ct_len,
            )
        },
        RsHandheldStatus::Ok
    );
    let mut pt = [0u8; 128];
    let mut pt_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_decrypt(
                &resp_key,
                ct.as_ptr(),
                ct_len,
                pt.as_mut_ptr(),
                pt.len(),
                &mut pt_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(&pt[..pt_len], msg);

    // Tampered ciphertext -> ErrCrypto.
    ct[ct_len - 1] ^= 0xFF;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_decrypt(
                &resp_key,
                ct.as_ptr(),
                ct_len,
                pt.as_mut_ptr(),
                pt.len(),
                &mut pt_len,
            )
        },
        RsHandheldStatus::ErrCrypto
    );

    // A proof for the wrong link id -> not valid (status OK, valid=0).
    let mut bad_valid = 1i32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_link_proof_validate(
                &resp_pub,
                &[0u8; 16],
                proof.as_ptr(),
                proof_len,
                core::ptr::null_mut(),
                &mut bad_valid,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(bad_valid, 0);

    unsafe { rs_handheld_rns_shutdown(responder) };
}

// ---- Resource — pinned vs ../rsReticulumLite/vectors/resource_vectors.json
// (RNS 1.3.8; same fixed handshake material as the lite compat tests). ----

const RES_LINK_ID: [u8; 16] = [0xCD; 16];
const RES_RESP_X_PUB: &str = "38ab664bd86f77d7e66bdd9ae0792913a94fd8b33a1260027e4b46c1f4884c67";
const RES_SESSION_KEY: &str = "e88b39ea8e2e6e0310e3c9f971ad879f1462081bbd81be4ed6e1ac863e9dea34613d30de36100b342e6f08bcca7fa80e08c275431da6725646d63eba17d04e05";
const RES_RANDOM_HASH: [u8; 4] = [0xAB; 4];
const RES_IV: [u8; 16] = [0x11; 16];
const RES_MULTI_HASH: &str = "42ab3f846a49714d899a01f3f1e475a2e68817ef123e02b9305c35c1e99331d9";

fn resource_session_key() -> [u8; 64] {
    // Same derivation the C++ owner performs at link establishment; pinned vs Python.
    let init_x = [0x33u8; 32];
    let resp_pub: [u8; 32] = unhex(RES_RESP_X_PUB);
    let mut key = [0u8; 64];
    assert_eq!(
        unsafe { rs_handheld_rns_link_derive(&init_x, &resp_pub, &RES_LINK_ID, &mut key) },
        RsHandheldStatus::Ok
    );
    assert_eq!(to_hex(&key), RES_SESSION_KEY);
    key
}

fn resource_payload(len: usize) -> Vec<u8> {
    (0..len).map(|i| (i % 251) as u8).collect()
}

#[test]
fn resource_full_transfer_through_ffi() {
    // Sender and receiver contexts, both over the pinned session key (multi vector: 2000 B).
    let key = resource_session_key();
    let payload = resource_payload(2000);
    let mut tx: *mut RsHandheldRns = core::ptr::null_mut();
    let mut rx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut tx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut rx) },
        RsHandheldStatus::Ok
    );

    // Sender: build + advertise.
    let mut adv = [0u8; RS_HANDHELD_RESOURCE_ADV_MAX];
    let mut adv_len = 0usize;
    let mut num_parts = 0u32;
    let mut rhash = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_build(
                tx,
                &key,
                payload.as_ptr(),
                payload.len(),
                &RES_RANDOM_HASH,
                &RES_IV,
                adv.as_mut_ptr(),
                adv.len(),
                &mut adv_len,
                &mut num_parts,
                &mut rhash,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(num_parts, 5); // pinned multi vector geometry
    assert_eq!(to_hex(&rhash), RES_MULTI_HASH);

    // Receiver: accept the advertisement.
    let mut r_parts = 0u32;
    let mut r_transfer = 0u32;
    let mut r_data = 0u32;
    let mut r_hash = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_accept(
                rx,
                adv.as_ptr(),
                adv_len,
                &mut r_parts,
                &mut r_transfer,
                &mut r_data,
                &mut r_hash,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!((r_parts, r_transfer, r_data), (5, 2064, 2000));
    assert_eq!(r_hash, rhash);

    // Request/serve/ingest rounds until complete (initial window 4 -> two rounds: 4 + 1).
    let mut rounds = 0;
    let mut complete = 0i32;
    while complete == 0 {
        rounds += 1;
        assert!(
            rounds <= 2,
            "transfer did not converge in the pinned rounds"
        );
        let mut req = [0u8; RS_HANDHELD_RESOURCE_REQUEST_MAX];
        let mut req_len = 0usize;
        assert_eq!(
            unsafe {
                rs_handheld_rns_resource_request_build(
                    rx,
                    req.as_mut_ptr(),
                    req.len(),
                    &mut req_len,
                )
            },
            RsHandheldStatus::Ok
        );
        let mut idx = [0u32; RS_HANDHELD_RESOURCE_MAX_PARTS];
        let mut count = 0u32;
        assert_eq!(
            unsafe {
                rs_handheld_rns_resource_request_serve(
                    tx,
                    req.as_ptr(),
                    req_len,
                    idx.as_mut_ptr(),
                    &mut count,
                )
            },
            RsHandheldStatus::Ok
        );
        assert!(count > 0);
        for &i in &idx[..count as usize] {
            let mut part = [0u8; RS_HANDHELD_RESOURCE_SDU];
            let mut part_len = 0usize;
            assert_eq!(
                unsafe {
                    rs_handheld_rns_resource_part_emit(
                        tx,
                        i,
                        part.as_mut_ptr(),
                        part.len(),
                        &mut part_len,
                    )
                },
                RsHandheldStatus::Ok
            );
            let mut is_new = -1i32;
            assert_eq!(
                unsafe {
                    rs_handheld_rns_resource_part_ingest(
                        rx,
                        part.as_ptr(),
                        part_len,
                        &mut is_new,
                        &mut complete,
                    )
                },
                RsHandheldStatus::Ok
            );
            assert_eq!(is_new, 1);
            // A duplicate is silently not-new.
            let mut dup = -1i32;
            assert_eq!(
                unsafe {
                    rs_handheld_rns_resource_part_ingest(
                        rx,
                        part.as_ptr(),
                        part_len,
                        &mut dup,
                        &mut complete,
                    )
                },
                RsHandheldStatus::Ok
            );
            assert_eq!(dup, 0);
        }
    }

    // Assemble + proof; sender validates -> DELIVERED.
    let mut data = [0u8; RS_HANDHELD_RESOURCE_DATA_MAX];
    let mut data_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_assemble(
                rx,
                &key,
                data.as_mut_ptr(),
                data.len(),
                &mut data_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(&data[..data_len], &payload[..]);
    let mut proof = [0u8; RS_HANDHELD_RESOURCE_PROOF_LEN];
    let mut proof_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_proof_build(
                rx,
                proof.as_mut_ptr(),
                proof.len(),
                &mut proof_len,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(proof_len, RS_HANDHELD_RESOURCE_PROOF_LEN);
    let mut valid = -1i32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_proof_validate(tx, proof.as_ptr(), proof_len, &mut valid)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 1);

    // Tampered proof -> valid=0.
    let mut bad = proof;
    bad[63] ^= 0xFF;
    assert_eq!(
        unsafe { rs_handheld_rns_resource_proof_validate(tx, bad.as_ptr(), bad.len(), &mut valid) },
        RsHandheldStatus::Ok
    );
    assert_eq!(valid, 0);

    // Close both sides; the surfaces then report not-ready.
    assert_eq!(
        unsafe { rs_handheld_rns_resource_outbound_close(tx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_resource_inbound_close(rx) },
        RsHandheldStatus::Ok
    );
    let mut part_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_part_emit(tx, 0, data.as_mut_ptr(), data.len(), &mut part_len)
        },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_proof_build(
                rx,
                proof.as_mut_ptr(),
                proof.len(),
                &mut proof_len,
            )
        },
        RsHandheldStatus::ErrNotReady
    );
    unsafe { rs_handheld_rns_shutdown(tx) };
    unsafe { rs_handheld_rns_shutdown(rx) };
}

#[test]
fn resource_negative_paths_through_ffi() {
    let key = resource_session_key();
    let payload = resource_payload(600);
    let mut tx: *mut RsHandheldRns = core::ptr::null_mut();
    let mut rx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut tx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut rx) },
        RsHandheldStatus::Ok
    );

    // No transfer open -> ErrNotReady on every stateful surface.
    let mut n = 0usize;
    let mut buf = [0u8; 600];
    let mut i32out = 0i32;
    assert_eq!(
        unsafe { rs_handheld_rns_resource_part_emit(tx, 0, buf.as_mut_ptr(), buf.len(), &mut n) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_resource_request_build(rx, buf.as_mut_ptr(), buf.len(), &mut n) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_part_ingest(rx, buf.as_ptr(), 4, &mut i32out, &mut i32out)
        },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_resource_window_grow(rx, 0) },
        RsHandheldStatus::ErrNotReady
    );
    assert_eq!(
        unsafe { rs_handheld_rns_resource_window_shrink(rx) },
        RsHandheldStatus::ErrNotReady
    );

    // Oversized payload -> ErrCapacity.
    let big = resource_payload(RS_HANDHELD_RESOURCE_DATA_MAX + 1);
    let mut adv = [0u8; RS_HANDHELD_RESOURCE_ADV_MAX];
    let mut adv_len = 0usize;
    let mut parts = 0u32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_build(
                tx,
                &key,
                big.as_ptr(),
                big.len(),
                &RES_RANDOM_HASH,
                &RES_IV,
                adv.as_mut_ptr(),
                adv.len(),
                &mut adv_len,
                &mut parts,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::ErrCapacity
    );

    // Real transfer for the remaining paths.
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_build(
                tx,
                &key,
                payload.as_ptr(),
                payload.len(),
                &RES_RANDOM_HASH,
                &RES_IV,
                adv.as_mut_ptr(),
                adv.len(),
                &mut adv_len,
                &mut parts,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );

    // Unsupported ADV variants are refused at accept (fail-closed honest subset).
    let parsed = ResourceAdv::parse(&adv[..adv_len]).unwrap();
    for mutate in [
        (|a: &mut ResourceAdv| a.flags.compressed = true) as fn(&mut ResourceAdv),
        |a| a.flags.split = true,
        |a| a.flags.has_metadata = true,
        |a| a.flags.is_request = true,
        |a| a.flags.encrypted = false,
    ] {
        let mut bad = parsed;
        mutate(&mut bad);
        let mut packed = [0u8; RS_HANDHELD_RESOURCE_ADV_MAX];
        let bn = bad.pack(&mut packed).unwrap();
        let (mut a, mut b, mut c) = (0u32, 0u32, 0u32);
        assert_eq!(
            unsafe {
                rs_handheld_rns_resource_advertise_accept(
                    rx,
                    packed.as_ptr(),
                    bn,
                    &mut a,
                    &mut b,
                    &mut c,
                    core::ptr::null_mut(),
                )
            },
            RsHandheldStatus::ErrUnsupported
        );
    }
    // Malformed ADV -> ErrInvalidArg.
    let (mut a, mut b, mut c) = (0u32, 0u32, 0u32);
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_accept(
                rx,
                adv.as_ptr(),
                3,
                &mut a,
                &mut b,
                &mut c,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::ErrInvalidArg
    );

    // Accept the honest ADV; premature assemble -> ErrNotReady; tampered part -> new=0.
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_advertise_accept(
                rx,
                adv.as_ptr(),
                adv_len,
                &mut a,
                &mut b,
                &mut c,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    let mut data = [0u8; RS_HANDHELD_RESOURCE_DATA_MAX];
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_assemble(rx, &key, data.as_mut_ptr(), data.len(), &mut n)
        },
        RsHandheldStatus::ErrNotReady
    );
    let mut part = [0u8; RS_HANDHELD_RESOURCE_SDU];
    let mut part_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_part_emit(tx, 0, part.as_mut_ptr(), part.len(), &mut part_len)
        },
        RsHandheldStatus::Ok
    );
    let mut tampered = part;
    tampered[0] ^= 0xFF;
    let (mut is_new, mut complete) = (-1i32, -1i32);
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_part_ingest(
                rx,
                tampered.as_ptr(),
                part_len,
                &mut is_new,
                &mut complete,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!((is_new, complete), (0, 0));

    // Out-of-range part index -> ErrInvalidArg; undersized part buffer -> ErrCapacity.
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_part_emit(tx, 99, part.as_mut_ptr(), part.len(), &mut part_len)
        },
        RsHandheldStatus::ErrInvalidArg
    );
    let mut tiny = [0u8; 8];
    assert_eq!(
        unsafe { rs_handheld_rns_resource_part_emit(tx, 0, tiny.as_mut_ptr(), tiny.len(), &mut n) },
        RsHandheldStatus::ErrCapacity
    );

    // A request for a different resource hash serves zero parts.
    let mut foreign = [0u8; 33];
    foreign[0] = 0x00;
    foreign[1..33].copy_from_slice(&[0x77; 32]);
    let mut idx = [0u32; RS_HANDHELD_RESOURCE_MAX_PARTS];
    let mut count = 99u32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_resource_request_serve(
                tx,
                foreign.as_ptr(),
                foreign.len(),
                idx.as_mut_ptr(),
                &mut count,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(count, 0);

    // Window ops are live once a transfer is open.
    assert_eq!(
        unsafe { rs_handheld_rns_resource_window_grow(rx, 0) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_resource_window_shrink(rx) },
        RsHandheldStatus::Ok
    );

    unsafe { rs_handheld_rns_shutdown(tx) };
    unsafe { rs_handheld_rns_shutdown(rx) };
}

// ---- local-frame ingest, peek, link codec, packet_hash ----

fn frame_data_h1(dest: &[u8; 16], payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.push(0x00); // flags: Data, Header1, Broadcast, Single, ctx_flag 0
    v.push(0x00); // hops
    v.extend_from_slice(dest);
    v.push(0x00); // context None
    v.extend_from_slice(payload);
    v
}

#[test]
fn packet_hash_matches_wire_rule() {
    let raw = frame_data_h1(&[0xAB; 16], b"payload bytes here");
    let mut out = [0u8; 32];
    assert_eq!(
        unsafe { rs_handheld_rns_packet_hash(raw.as_ptr(), raw.len(), 0, &mut out) },
        RsHandheldStatus::Ok
    );
    assert_eq!(out, packet_hash(&raw, HeaderType::Header1));
    // Bad header_type / short buffer -> InvalidArg.
    assert_eq!(
        unsafe { rs_handheld_rns_packet_hash(raw.as_ptr(), raw.len(), 9, &mut out) },
        RsHandheldStatus::ErrInvalidArg
    );
}

#[test]
fn ingest_reports_local_frame_for_our_destination() {
    // Endpoint node (transport_enabled=0); a DATA packet to OUR lxmf.delivery dest is dropped by
    // the relay core and re-surfaced as a LocalFrame.
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let mut our_dest = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut our_dest) },
        RsHandheldStatus::Ok
    );
    let pkt = frame_data_h1(&our_dest, b"opportunistic ciphertext");
    let mut action = -1;
    let mut local: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt.as_ptr(),
                pkt.len(),
                0,
                1000,
                &mut action,
                core::ptr::null_mut(),
                &mut local,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action, INGEST_LOCAL_FRAME);
    assert_eq!(local.packet_type, PacketType::Data as u8);
    assert_eq!(local.destination_hash, our_dest);
    assert_eq!(local.packet_hash, packet_hash(&pkt, HeaderType::Header1));
    assert_eq!(
        &local.payload[..local.payload_len as usize],
        b"opportunistic ciphertext"
    );

    // A DATA packet to a STRANGER dest is dropped, NOT local.
    let pkt2 = frame_data_h1(&[0x77; 16], b"not ours");
    let mut action2 = -1;
    let mut local2: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                pkt2.as_ptr(),
                pkt2.len(),
                0,
                1001,
                &mut action2,
                core::ptr::null_mut(),
                &mut local2,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action2, 8); // Dropped, not local
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn link_register_routes_link_frame_local() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let link_id = [0x5A; 16];
    // Distinct payload per call so the transport hashlist doesn't dedup identical frames.
    let frame = |tag: u8| -> Vec<u8> {
        let mut v = Vec::new();
        v.push(0x0C); // flags: Data, Header1, Broadcast, Link(0b11<<2), ctx 0
        v.push(0x00);
        v.extend_from_slice(&link_id);
        v.push(0x00);
        v.extend_from_slice(&[tag; 12]);
        v
    };
    let call = |ctx: *mut RsHandheldRns, v: &[u8], a: &mut i32, l: &mut RsHandheldLocalFrame| unsafe {
        rs_handheld_rns_packet_ingest(
            ctx,
            v.as_ptr(),
            v.len(),
            0,
            2000,
            a,
            core::ptr::null_mut(),
            l,
        )
    };
    // Link-typed DATA to an UNregistered link id is dropped, not local.
    let mut action = -1;
    let mut local: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        call(ctx, &frame(1), &mut action, &mut local),
        RsHandheldStatus::Ok
    );
    assert_ne!(action, INGEST_LOCAL_FRAME);

    assert_eq!(
        unsafe { rs_handheld_rns_link_register(ctx, &link_id) },
        RsHandheldStatus::Ok
    );
    let mut action2 = -1;
    let mut local2: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        call(ctx, &frame(2), &mut action2, &mut local2),
        RsHandheldStatus::Ok
    );
    assert_eq!(action2, INGEST_LOCAL_FRAME);
    assert_eq!(local2.destination_hash, link_id);

    // After unregister, back to non-local.
    assert_eq!(
        unsafe { rs_handheld_rns_link_unregister(ctx, &link_id) },
        RsHandheldStatus::Ok
    );
    let mut action3 = -1;
    let mut local3: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        call(ctx, &frame(3), &mut action3, &mut local3),
        RsHandheldStatus::Ok
    );
    assert_ne!(action3, INGEST_LOCAL_FRAME);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

/// PLAIN/DATA/Header1 packet to the `rnstransport.path.request` control dest carrying
/// `payload` (requested-dest || [requestor_id] || tag). flag byte = Plain(2)<<2 | Data(0) = 0x08.
fn frame_path_request(payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.push(0x08); // Data, Header1, Broadcast, Plain, ctx_flag 0
    v.push(0x00); // hops
    v.extend_from_slice(&rns_lite_core::path_request_destination());
    v.push(0x00); // context None
    v.extend_from_slice(payload);
    v
}

fn ingest_action(ctx: *mut RsHandheldRns, raw: &[u8], now_ms: u64) -> i32 {
    let mut action = -1;
    let mut local: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                ctx,
                raw.as_ptr(),
                raw.len(),
                0,
                now_ms,
                &mut action,
                core::ptr::null_mut(),
                &mut local,
            )
        },
        RsHandheldStatus::Ok
    );
    action
}

fn take_own_path_tag(ctx: *mut RsHandheldRns) -> ([u8; 16], usize) {
    let mut tag = [0u8; 16];
    let mut tag_len = 0usize;
    assert_eq!(
        unsafe { rs_handheld_rns_take_own_path_request_tag(ctx, &mut tag, &mut tag_len) },
        RsHandheldStatus::Ok
    );
    (tag, tag_len)
}

#[test]
fn ingest_reports_path_request_self_for_our_dest() {
    // Endpoint node; a path request for OUR delivery dest is dropped by the relay core and
    // re-classified as a self path-request signal (C++ re-announces as a PATH_RESPONSE).
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let mut our_dest = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut our_dest) },
        RsHandheldStatus::Ok
    );

    // Leaf requestor: requested(16) || tag(16).
    let mut payload = our_dest.to_vec();
    payload.extend_from_slice(&[0x22u8; 16]);
    assert_eq!(
        ingest_action(ctx, &frame_path_request(&payload), 1000),
        INGEST_PATH_REQUEST_SELF
    );
    assert_eq!(take_own_path_tag(ctx), ([0x22u8; 16], 16));
    let mut consumed = [0u8; 16];
    let mut consumed_len = 0usize;
    assert_eq!(
        unsafe { rs_handheld_rns_take_own_path_request_tag(ctx, &mut consumed, &mut consumed_len) },
        RsHandheldStatus::ErrNotReady
    );

    // Transport-enabled requestor: requested(16) || requestor_id(16) || tag(16) — still ours.
    let mut payload_t = our_dest.to_vec();
    payload_t.extend_from_slice(&[0x33u8; 16]); // requestor transport id
    payload_t.extend_from_slice(&[0x44u8; 16]); // tag
    assert_eq!(
        ingest_action(ctx, &frame_path_request(&payload_t), 1001),
        INGEST_PATH_REQUEST_SELF
    );
    assert_eq!(take_own_path_tag(ctx), ([0x44u8; 16], 16));

    // Short tags retain their exact length and are zero-padded only at the C boundary.
    let mut payload_short = our_dest.to_vec();
    payload_short.extend_from_slice(&[0x55u8; 7]);
    assert_eq!(
        ingest_action(ctx, &frame_path_request(&payload_short), 1002),
        INGEST_PATH_REQUEST_SELF
    );
    let (short, short_len) = take_own_path_tag(ctx);
    assert_eq!(short_len, 7);
    assert_eq!(&short[..7], &[0x55u8; 7]);
    assert_eq!(&short[7..], &[0u8; 9]);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn ingest_ignores_path_request_for_stranger_and_tagless() {
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let mut our_dest = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut our_dest) },
        RsHandheldStatus::Ok
    );

    // A request for a STRANGER dest is dropped, never a self signal.
    let mut stranger = [0x77u8; 16].to_vec();
    stranger.extend_from_slice(&[0x22u8; 16]);
    assert_eq!(ingest_action(ctx, &frame_path_request(&stranger), 2000), 8);

    // Tagless request for our own dest (16 bytes, no tag) is ignored (Python parity).
    assert_eq!(
        ingest_action(ctx, &frame_path_request(&our_dest), 2001),
        8 // Dropped, not 10
    );
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn path_request_self_literal_retransmit_deduped_before_detection() {
    // A byte-identical retransmit is dropped as Duplicate by the lite request_tags gate inside
    // node.ingest, BEFORE the self path-request post-check runs (storm-defense layer 1).
    let ctx = loaded_ctx();
    let _node = open_transport_into(ctx, 0);
    let mut our_dest = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_destination_hash(ctx, &mut our_dest) },
        RsHandheldStatus::Ok
    );
    let mut payload = our_dest.to_vec();
    payload.extend_from_slice(&[0x55u8; 16]);
    let raw = frame_path_request(&payload);
    assert_eq!(ingest_action(ctx, &raw, 3000), INGEST_PATH_REQUEST_SELF);
    // Same bytes again (same tag) within the 120s gate -> Duplicate (1), not 10.
    assert_eq!(ingest_action(ctx, &raw, 3100), 1);
    unsafe { rs_handheld_rns_shutdown(ctx) };
}

#[test]
fn lxmf_peek_source_recovers_hash_without_validating() {
    // Build an opportunistic message to the recipient; the recipient peeks the source hash.
    let src_ctx = loaded_ctx();
    let recipient_pub: [u8; 64] = unhex(LXMF_RECIPIENT_PUB);
    let mut out = [0u8; 600];
    let mut out_len = 0usize;
    let mut dest = [0u8; 16];
    let mut mid = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                src_ctx,
                &recipient_pub,
                1.0,
                1,
                0,
                b"t".as_ptr(),
                1,
                b"c".as_ptr(),
                1,
                &[0x21; 32],
                &[0x22; 16],
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
                &mut dest,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );
    // Load the recipient identity and peek.
    let prv: [u8; 64] = unhex(LXMF_RECIPIENT_PRV);
    let mut rx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut rx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(rx, &prv) },
        RsHandheldStatus::Ok
    );
    let mut src_hash = [0u8; 16];
    assert_eq!(
        unsafe { rs_handheld_rns_lxmf_peek_source(rx, out.as_ptr(), out_len, &mut src_hash) },
        RsHandheldStatus::Ok
    );
    // Source hash = the incrementing identity's lxmf.delivery dest.
    let src_id = LocalIdentity::from_private_key(&{
        let k: [u8; 64] = unhex(INCREMENTING);
        k
    });
    assert_eq!(src_hash, src_id.lxmf_delivery_hash());
    unsafe { rs_handheld_rns_shutdown(src_ctx) };
    unsafe { rs_handheld_rns_shutdown(rx) };
}

#[test]
fn lxmf_parse_auto_resolves_known_then_path_then_reports_unknown() {
    let source_ctx = loaded_ctx();
    let recipient_public_key: [u8; 64] = unhex(LXMF_RECIPIENT_PUB);
    let mut payload = [0u8; 600];
    let mut payload_len = 0usize;
    let mut destination = [0u8; 16];
    let mut message_id = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                source_ctx,
                &recipient_public_key,
                42.0,
                42,
                0,
                b"auto".as_ptr(),
                4,
                b"resolved".as_ptr(),
                8,
                &[0x31; 32],
                &[0x32; 16],
                payload.as_mut_ptr(),
                payload.len(),
                &mut payload_len,
                &mut destination,
                &mut message_id,
            )
        },
        RsHandheldStatus::Ok
    );

    let recipient_private_key: [u8; 64] = unhex(LXMF_RECIPIENT_PRV);
    let mut recipient: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut recipient) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(recipient, &recipient_private_key) },
        RsHandheldStatus::Ok
    );
    let _node = open_transport_into(recipient, 0);

    let mut message: RsHandheldLxmfMessage = unsafe { std::mem::zeroed() };
    let mut resolved = [0u8; 64];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_auto(
                recipient,
                payload.as_ptr(),
                payload_len,
                &mut message,
                &mut resolved,
            )
        },
        RsHandheldStatus::ErrSourceUnknown
    );

    let source = LocalIdentity::from_private_key(&unhex::<64>(INCREMENTING));
    let source_destination = source.lxmf_delivery_hash();
    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_known_dest_learn(
                recipient,
                &source_destination,
                source.public_key(),
                1000,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_auto(
                recipient,
                payload.as_ptr(),
                payload_len,
                &mut message,
                &mut resolved,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(resolved, *source.public_key());
    assert_eq!(
        &message.content[..message.content_len as usize],
        b"resolved"
    );

    // Relearn through transport, then remove only durable memory. The same call now reaches
    // the volatile path-table fallback and still authenticates the sender.
    let mut announce_payload = [0u8; 600];
    let announce_len = source
        .create_announce_named(
            "lxmf.delivery",
            &compose_random_hash(&[0x33; 5], 43),
            None,
            &[],
            &mut announce_payload,
        )
        .unwrap();
    let announce_packet = frame_announce(&announce_payload[..announce_len], &source_destination);
    let mut action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                recipient,
                announce_packet.as_ptr(),
                announce_packet.len(),
                4,
                2000,
                &mut action,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(action == 2 || action == 3);
    let mut node_ptr = unsafe { &mut *recipient }.node.unwrap();
    unsafe { node_ptr.as_mut() }.known_destinations = KnownDestinations::new();
    resolved.fill(0);
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_auto(
                recipient,
                payload.as_ptr(),
                payload_len,
                &mut message,
                &mut resolved,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(resolved, *source.public_key());

    unsafe { rs_handheld_rns_shutdown(source_ctx) };
    unsafe { rs_handheld_rns_shutdown(recipient) };
}

#[test]
fn lxmf_build_link_and_parse_link_roundtrip() {
    // Source builds a FULL packed (DIRECT) message; recipient parses it.
    let src_ctx = loaded_ctx();
    let recipient_pub: [u8; 64] = unhex(LXMF_RECIPIENT_PUB);
    // A large content only deliverable via link/resource.
    let content = vec![0x5Au8; 1500];
    let mut out = vec![0u8; 4096];
    let mut out_len = 0usize;
    let mut dest = [0u8; 16];
    let mut mid = [0u8; 32];
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build_link(
                src_ctx,
                &recipient_pub,
                1234567890.5,
                b"Hi".as_ptr(),
                2,
                content.as_ptr(),
                content.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut out_len,
                &mut dest,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(to_hex(&dest), LXMF_DEST_HASH);
    assert_eq!(&out[..16], &dest); // full packed form carries dest prefix

    // Recipient parses.
    let prv: [u8; 64] = unhex(LXMF_RECIPIENT_PRV);
    let source_pub: [u8; 64] = unhex(INCREMENTING_PUB);
    let mut rx: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut rx) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(rx, &prv) },
        RsHandheldStatus::Ok
    );
    let mut r_mid = [0u8; 32];
    let mut r_src = [0u8; 16];
    let mut r_ts = 0f64;
    let mut title_buf = vec![0u8; RS_HANDHELD_RESOURCE_DATA_MAX];
    let mut content_buf = vec![0u8; RS_HANDHELD_RESOURCE_DATA_MAX];
    let mut tlen = 0usize;
    let mut clen = 0usize;
    let mut is_reaction = -1i32;
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_link(
                rx,
                out.as_ptr(),
                out_len,
                &source_pub,
                &mut r_mid,
                &mut r_src,
                &mut r_ts,
                title_buf.as_mut_ptr(),
                title_buf.len(),
                &mut tlen,
                content_buf.as_mut_ptr(),
                content_buf.len(),
                &mut clen,
                &mut is_reaction,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(r_mid, mid);
    assert_eq!(r_ts, 1234567890.5);
    assert_eq!(&title_buf[..tlen], b"Hi");
    assert_eq!(clen, content.len());
    assert_eq!(&content_buf[..clen], &content[..]);
    assert_eq!(is_reaction, 0);

    // Wrong recipient identity -> ErrCrypto (destination mismatch).
    let wrong_prv: [u8; 64] = unhex(INCREMENTING);
    let mut wr: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut wr) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(wr, &wrong_prv) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse_link(
                wr,
                out.as_ptr(),
                out_len,
                &source_pub,
                &mut r_mid,
                &mut r_src,
                &mut r_ts,
                title_buf.as_mut_ptr(),
                title_buf.len(),
                &mut tlen,
                content_buf.as_mut_ptr(),
                content_buf.len(),
                &mut clen,
                &mut is_reaction,
            )
        },
        RsHandheldStatus::ErrCrypto
    );
    unsafe { rs_handheld_rns_shutdown(src_ctx) };
    unsafe { rs_handheld_rns_shutdown(rx) };
    unsafe { rs_handheld_rns_shutdown(wr) };
}

#[test]
fn lxmf_reaction_scanner_matches_micro_filter_semantics() {
    // Standard FIELD_REACTION 0x40 (any value).
    assert!(lxmf_is_reaction(&[0x81, 0x40, 0x80]));
    // Legacy envelope: {0xFB: "ratspeak.reaction"} (0xFB packs as uint8 0xcc 0xfb).
    let mut legacy = vec![0x81, 0xcc, 0xfb, 0xb1];
    legacy.extend_from_slice(b"ratspeak.reaction");
    assert!(lxmf_is_reaction(&legacy));
    // Legacy chat envelope with customData kind == "reaction":
    // {0xFB: "ratspeak.chat.v1", 0xFC: bin(msgpack {"kind": "reaction"})}.
    let inner: &[u8] = &[
        0x81, 0xa4, b'k', b'i', b'n', b'd', 0xa8, b'r', b'e', b'a', b'c', b't', b'i', b'o', b'n',
    ];
    let mut chat = vec![0x82, 0xcc, 0xfb, 0xb0];
    chat.extend_from_slice(b"ratspeak.chat.v1");
    chat.extend_from_slice(&[0xcc, 0xfc, 0xc4, inner.len() as u8]);
    chat.extend_from_slice(inner);
    assert!(lxmf_is_reaction(&chat));
    // Same envelope, kind == "chat": NOT a reaction.
    let inner_chat: &[u8] = &[
        0x81, 0xa4, b'k', b'i', b'n', b'd', 0xa4, b'c', b'h', b'a', b't',
    ];
    let mut not_reaction = vec![0x82, 0xcc, 0xfb, 0xb0];
    not_reaction.extend_from_slice(b"ratspeak.chat.v1");
    not_reaction.extend_from_slice(&[0xcc, 0xfc, 0xc4, inner_chat.len() as u8]);
    not_reaction.extend_from_slice(inner_chat);
    assert!(!lxmf_is_reaction(&not_reaction));
    // Empty fields, non-map fields, unrelated field: not reactions.
    assert!(!lxmf_is_reaction(&[0x80]));
    assert!(!lxmf_is_reaction(&[0xc0]));
    assert!(!lxmf_is_reaction(&[0x81, 0x30, 0xc4, 0x01, 0xaa]));
}
#[test]
fn accepted_peer_ratchet_survives_the_uptime_vs_epoch_clock_split() {
    // Regression: the C++ acceptance event occurs on an uptime-driven pump while expiry may
    // later use wall time. Both clocks are explicit, so no domain is silently reinterpreted.
    let learner = loaded_ctx();
    let _node = open_transport_into(learner, 1);
    let epoch: u64 = 1_700_000_000;

    // Host announces first (carries the real clock) -> the ctx learns the epoch.
    let (_own, own_action) = test_persist_commit_ratchet(learner, &[0xF1; 32], epoch, 0);
    assert_eq!(own_action, RATCHET_PREP_ROTATED);

    let mut peer: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut peer) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(peer, &[0x68u8; PRIVATE_KEY_LENGTH]) },
        RsHandheldStatus::Ok
    );
    let peer_id = LocalIdentity::from_private_key(&[0x68u8; PRIVATE_KEY_LENGTH]);
    let (announced, peer_action) = test_persist_commit_ratchet(peer, &[0xF2; 32], epoch, 0);
    assert_eq!(peer_action, RATCHET_PREP_ROTATED);
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let (payload, dest) = announce(peer, &announced, &rat);
    let pkt = {
        use rns_lite_core::wire::{
            DestinationType, HeaderType, PacketContext, PacketFlags, PacketHeader, TransportType,
            build_packet,
        };
        let header = PacketHeader {
            flags: PacketFlags {
                header_type: HeaderType::Header1,
                context_flag: true,
                transport_type: TransportType::Broadcast,
                destination_type: DestinationType::Single,
                packet_type: PacketType::Announce,
            },
            hops: 0,
            transport_id: None,
            destination_hash: dest,
            context: PacketContext::None,
        };
        build_packet(header, &payload).unwrap().as_slice().to_vec()
    };

    // Ingest with a small UPTIME clock, as the pump really does (seconds since boot).
    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                learner,
                pkt.as_ptr(),
                pkt.len(),
                7,
                45_000, // 45 s of uptime
                &mut action,
                &mut ev,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev.has_ratchet, 1);

    // Simulate the level-above KeyMap gate accepting the event. Packet ingest itself does not
    // commit peer state; this explicit call is the only write seam.
    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchet_remember(
                learner,
                &ev.destination_hash,
                &ev.ratchet,
                epoch,
                45_000,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);

    // Build with the WALL-CLOCK timestamp: the learned ratchet must still be preferred.
    let mut blob = [0u8; 600];
    let (mut n, mut d, mut mid) = (0usize, [0u8; DESTINATION_LENGTH], [0u8; 32]);
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_build(
                learner,
                peer_id.public_key(),
                epoch as f64,
                epoch,
                45_000,
                b"Hi".as_ptr(),
                2,
                b"clock split".as_ptr(),
                11,
                &[0xF3; 32],
                &[0xF4; 16],
                blob.as_mut_ptr(),
                blob.len(),
                &mut n,
                &mut d,
                &mut mid,
            )
        },
        RsHandheldStatus::Ok
    );
    // Proof it targeted the ratchet: the peer's BASE key must not open it, its ring must.
    let mut scratch = [0u8; rns_lite_core::crypto::MAX_ECIES_PLAINTEXT];
    let base_x: [u8; 32] = peer_id.private_key()[..32].try_into().unwrap();
    assert!(
        rns_lite_core::crypto::ecies_decrypt(
            &blob[..n],
            &base_x,
            peer_id.identity_hash(),
            &mut scratch,
        )
        .is_err(),
        "learned peer ratchet was ignored — clock split regression"
    );
    let mut msg: RsHandheldLxmfMessage = unsafe { std::mem::zeroed() };
    let learner_id = LocalIdentity::from_private_key(&unhex::<64>(INCREMENTING));
    assert_eq!(
        unsafe {
            rs_handheld_rns_lxmf_parse(peer, blob.as_ptr(), n, learner_id.public_key(), &mut msg)
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(&msg.content[..msg.content_len as usize], b"clock split");

    unsafe { rs_handheld_rns_shutdown(learner) };
    unsafe { rs_handheld_rns_shutdown(peer) };
}

#[test]
fn packet_ingest_never_commits_peer_ratchet_before_host_acceptance() {
    let learner = loaded_ctx();
    let _node = open_transport_into(learner, 1);

    let mut peer: *mut RsHandheldRns = core::ptr::null_mut();
    assert_eq!(
        unsafe { rs_handheld_rns_init(&mut peer) },
        RsHandheldStatus::Ok
    );
    assert_eq!(
        unsafe { rs_handheld_rns_load_identity(peer, &[0x66u8; PRIVATE_KEY_LENGTH]) },
        RsHandheldStatus::Ok
    );
    let (announced, peer_action) = test_persist_commit_ratchet(peer, &[0xD1; 32], 5_000, 0);
    assert_eq!(peer_action, RATCHET_PREP_ROTATED);

    // Ratcheted announce: payload carries the ratchet, header carries context_flag.
    let rat: [u8; 8] = unhex(RAT_APP_DATA);
    let (payload, dest) = announce(peer, &announced, &rat);
    let pkt = {
        use rns_lite_core::wire::{
            DestinationType, HeaderType, PacketContext, PacketFlags, PacketHeader, TransportType,
            build_packet,
        };
        let header = PacketHeader {
            flags: PacketFlags {
                header_type: HeaderType::Header1,
                context_flag: true,
                transport_type: TransportType::Broadcast,
                destination_type: DestinationType::Single,
                packet_type: PacketType::Announce,
            },
            hops: 0,
            transport_id: None,
            destination_hash: dest,
            context: PacketContext::None,
        };
        build_packet(header, &payload).unwrap().as_slice().to_vec()
    };

    let mut action = -1;
    let mut ev: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                learner,
                pkt.as_ptr(),
                pkt.len(),
                7,
                6_000_000,
                &mut action,
                &mut ev,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(ev.has_ratchet, 1);
    assert_eq!(ev.ratchet, announced);

    // Packet validation/surfacing alone MUST leave peer state untouched.
    let mut blob = [0u8; RS_HANDHELD_PEER_RATCHETS_BLOB_MAX];
    let mut blen = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_export(learner, blob.as_mut_ptr(), blob.len(), &mut blen)
        },
        RsHandheldStatus::Ok
    );
    let table = PeerRatchets::deserialize(&blob[..blen]).unwrap();
    assert!(table.is_empty());

    // The level above accepts KeyMap/freshness and explicitly commits once.
    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchet_remember(
                learner,
                &dest,
                &announced,
                6_000,
                6_000_000,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchet_remember(
                learner,
                &dest,
                &announced,
                7_000,
                7_000_000,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 0, "same key must not refresh its expiry");

    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_export(learner, blob.as_mut_ptr(), blob.len(), &mut blen)
        },
        RsHandheldStatus::Ok
    );
    let table = PeerRatchets::deserialize(&blob[..blen]).unwrap();
    assert_eq!(
        table.get(&dest, RatchetClock::new(Some(6_000), 0)),
        Some(announced)
    );

    unsafe { rs_handheld_rns_shutdown(learner) };
    unsafe { rs_handheld_rns_shutdown(peer) };
}

#[test]
fn freshness_rejected_announce_surfaces_no_event_and_changes_no_peer_state() {
    use rns_lite_core::wire::{
        DestinationType, HeaderType, PacketContext, PacketFlags, PacketHeader, TransportType,
        build_packet,
    };

    let learner = loaded_ctx();
    let _node = open_transport_into(learner, 1);
    let peer_identity = [0x6Au8; PRIVATE_KEY_LENGTH];

    let make_peer = |entropy: [u8; 32], order: u64, rng: [u8; 5]| {
        let mut peer: *mut RsHandheldRns = core::ptr::null_mut();
        assert_eq!(
            unsafe { rs_handheld_rns_init(&mut peer) },
            RsHandheldStatus::Ok
        );
        assert_eq!(
            unsafe { rs_handheld_rns_load_identity(peer, &peer_identity) },
            RsHandheldStatus::Ok
        );
        let (ratchet, action) = test_persist_commit_ratchet(peer, &entropy, order, 0);
        assert_eq!(action, RATCHET_PREP_ROTATED);
        let (payload, destination) = announce_at(peer, &rng, order, &ratchet, b"freshness");
        let header = PacketHeader {
            flags: PacketFlags {
                header_type: HeaderType::Header1,
                context_flag: true,
                transport_type: TransportType::Broadcast,
                destination_type: DestinationType::Single,
                packet_type: PacketType::Announce,
            },
            hops: 0,
            transport_id: None,
            destination_hash: destination,
            context: PacketContext::None,
        };
        let packet = build_packet(header, &payload).unwrap().as_slice().to_vec();
        (peer, ratchet, destination, packet)
    };

    let (fresh_peer, fresh_ratchet, destination, fresh_packet) =
        make_peer([0x6B; 32], 6_000, [1, 2, 3, 4, 5]);
    let mut action = -1;
    let mut event: RsHandheldAnnounceEvent = unsafe { std::mem::zeroed() };
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                learner,
                fresh_packet.as_ptr(),
                fresh_packet.len(),
                1,
                10_000,
                &mut action,
                &mut event,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert!(matches!(action, 2 | 3));
    assert_eq!(event.ratchet, fresh_ratchet);

    let mut changed = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchet_remember(
                learner,
                &destination,
                &fresh_ratchet,
                6_000,
                10_000,
                &mut changed,
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(changed, 1);

    // Same identity/destination, valid signature, distinct packet hash, but an older announce
    // order. Transport must classify it as ignored and leave the output event untouched.
    let (stale_peer, stale_ratchet, stale_destination, stale_packet) =
        make_peer([0x6C; 32], 5_000, [6, 7, 8, 9, 10]);
    assert_eq!(stale_destination, destination);
    assert_ne!(stale_ratchet, fresh_ratchet);
    event.identity_hash = [0xED; DESTINATION_LENGTH];
    event.ratchet = [0xEE; RATCHET_LEN];
    event.has_ratchet = 77;
    action = -1;
    assert_eq!(
        unsafe {
            rs_handheld_rns_packet_ingest(
                learner,
                stale_packet.as_ptr(),
                stale_packet.len(),
                1,
                20_000,
                &mut action,
                &mut event,
                core::ptr::null_mut(),
            )
        },
        RsHandheldStatus::Ok
    );
    assert_eq!(action, 12);
    assert_eq!(event.identity_hash, [0xED; DESTINATION_LENGTH]);
    assert_eq!(event.ratchet, [0xEE; RATCHET_LEN]);
    assert_eq!(event.has_ratchet, 77);

    let mut blob = [0u8; RS_HANDHELD_PEER_RATCHETS_BLOB_MAX];
    let mut blob_len = 0usize;
    assert_eq!(
        unsafe {
            rs_handheld_rns_peer_ratchets_export(
                learner,
                blob.as_mut_ptr(),
                blob.len(),
                &mut blob_len,
            )
        },
        RsHandheldStatus::Ok
    );
    let table = PeerRatchets::deserialize(&blob[..blob_len]).unwrap();
    assert_eq!(
        table.get(&destination, RatchetClock::new(Some(6_000), 0)),
        Some(fresh_ratchet)
    );

    unsafe { rs_handheld_rns_shutdown(learner) };
    unsafe { rs_handheld_rns_shutdown(fresh_peer) };
    unsafe { rs_handheld_rns_shutdown(stale_peer) };
}
