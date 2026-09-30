//! In-place relay operations over the existing shared codec workspace.
use super::*;
use lxmf_lite_core::propagation as relay;

fn failure(error: LxmfError) -> RsHandheldStatus {
    match error {
        LxmfError::OutputTooSmall | LxmfError::TooLong => RsHandheldStatus::ErrCapacity,
        LxmfError::Crypto | LxmfError::DestinationMismatch => RsHandheldStatus::ErrCrypto,
        _ => RsHandheldStatus::ErrInvalidArg,
    }
}

/// Check the full upload size before any PoW or encryption.
/// # Safety
/// Outputs are non-null, aligned, writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_relay_size(
    packed_length: usize,
    recipient_stamp: u8,
    entry_length: *mut usize,
    upload_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if packed_length < 96
            || recipient_stamp > 1
            || entry_length.is_null()
            || upload_length.is_null()
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let Some(plain) = (packed_length - 16).checked_add(usize::from(recipient_stamp) * 34)
        else {
            return RsHandheldStatus::ErrCapacity;
        };
        let Ok(cipher) = rns_lite_core::crypto::ecies_ciphertext_len(plain) else {
            return RsHandheldStatus::ErrCapacity;
        };
        let Some(entry) = cipher.checked_add(16) else {
            return RsHandheldStatus::ErrCapacity;
        };
        let Some(upload) = entry.checked_add(if entry <= 223 { 45 } else { 46 }) else {
            return RsHandheldStatus::ErrCapacity;
        };
        if upload > rns_resource::DATA_MAX {
            return RsHandheldStatus::ErrCapacity;
        }
        unsafe {
            *entry_length = entry;
            *upload_length = upload;
        }
        RsHandheldStatus::Ok
    })
}

/// Append a recipient PoW without changing signed bytes or message ID.
/// # Safety
/// Buffer is exclusively writable for capacity; other non-null pointers are
/// readable/writable as declared and disjoint. Capacity is bounded to DATA_MAX.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_append_stamp(
    buffer: *mut u8,
    length: usize,
    capacity: usize,
    stamp: *const [u8; 32],
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if buffer.is_null()
            || stamp.is_null()
            || out_length.is_null()
            || capacity > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts_mut(buffer, capacity) };
        match relay::append_recipient_stamp(bytes, length, unsafe { &*stamp }) {
            Ok(n) => {
                unsafe {
                    *out_length = n;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

/// Encrypt in place using the existing accepted peer ratchet, or its base key.
/// Persist the returned exact entry before upload; never persist caller entropy.
/// # Safety
/// Live serialized context; buffer writable for capacity; remaining non-null
/// pointers are disjoint, aligned and readable/writable for their declared size.
/// On crypto failure the buffer is not usable; scalar outputs remain unchanged.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rs_handheld_lxmf_relay_encrypt(
    ctx: *const RsHandheldRns,
    recipient: *const [u8; 64],
    wall_secs: u64,
    uptime_ms: u64,
    ephemeral: *const [u8; 32],
    iv: *const [u8; 16],
    buffer: *mut u8,
    length: usize,
    capacity: usize,
    out_length: *mut usize,
    transient_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || recipient.is_null()
            || ephemeral.is_null()
            || iv.is_null()
            || buffer.is_null()
            || out_length.is_null()
            || transient_id.is_null()
            || capacity > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &*ctx };
        if c.identity.is_none() {
            return RsHandheldStatus::ErrNotReady;
        }
        let recipient = unsafe { &*recipient };
        let destination = rns_lite_core::identity::destination_hash_from_name(
            "lxmf.delivery",
            Some(&rns_lite_core::identity::identity_hash(recipient)),
        );
        let ratchet = c.peer_ratchets.get(
            &destination,
            RatchetClock::from_host(wall_secs, uptime_ms / 1000),
        );
        let bytes = unsafe { core::slice::from_raw_parts_mut(buffer, capacity) };
        match relay::encrypt_message(
            bytes,
            length,
            recipient,
            ratchet.as_ref(),
            unsafe { &*ephemeral },
            unsafe { &*iv },
        ) {
            Ok((n, id)) => {
                unsafe {
                    *out_length = n;
                    *transient_id = id;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

/// Recompute the transient hash over the encrypted entry, excluding the PN stamp.
/// This is an integrity/binding primitive, not validation of encrypted contents.
/// # Safety
/// Non-null readable input and disjoint writable output, sized as declared.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_transient_id(
    data: *const u8,
    length: usize,
    out: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || out.is_null() || !(112..=rns_resource::DATA_MAX).contains(&length) {
            return RsHandheldStatus::ErrInvalidArg;
        }
        unsafe {
            *out = rns_lite_core::wire::sha256(core::slice::from_raw_parts(data, length));
        }
        RsHandheldStatus::Ok
    })
}

/// Wrap one persisted entry plus its mandatory 32-byte node stamp in place.
/// # Safety
/// Non-null exclusive buffer and disjoint stamp/scalar output, sized as declared.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_relay_upload(
    buffer: *mut u8,
    encrypted_length: usize,
    capacity: usize,
    timestamp: f64,
    stamp: *const [u8; 32],
    out_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if buffer.is_null()
            || stamp.is_null()
            || out_length.is_null()
            || capacity > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let bytes = unsafe { core::slice::from_raw_parts_mut(buffer, capacity) };
        match relay::wrap_upload(bytes, encrypted_length, unsafe { &*stamp }, timestamp) {
            Ok(n) => {
                unsafe {
                    *out_length = n;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

/// Decrypt a downloaded entry using retained local ratchets then the base key.
/// Caller must still validate sender/signature through ordinary LXMF admission.
/// # Safety
/// Live serialized context; buffer exclusively writable for length; scalar and
/// hash outputs are non-null, aligned and disjoint. Buffer unusable on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_relay_decrypt(
    ctx: *const RsHandheldRns,
    buffer: *mut u8,
    length: usize,
    out_length: *mut usize,
    transient_id: *mut [u8; 32],
) -> RsHandheldStatus {
    guard(|| {
        if ctx.is_null()
            || buffer.is_null()
            || out_length.is_null()
            || transient_id.is_null()
            || length > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let c = unsafe { &*ctx };
        let Some(identity) = &c.identity else {
            return RsHandheldStatus::ErrNotReady;
        };
        let bytes = unsafe { core::slice::from_raw_parts_mut(buffer, length) };
        match relay::decrypt_message(bytes, identity, c.ratchet_ring.private_keys()) {
            Ok((n, id, _)) => {
                unsafe {
                    *out_length = n;
                    *transient_id = id;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

/// Validate the complete available-ID array but retain only its first ID.
/// An empty list publishes zero ID/count; invalid input publishes nothing.
/// # Safety
/// Non-null readable input and disjoint aligned writable outputs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_available_first(
    data: *const u8,
    length: usize,
    id: *mut [u8; 32],
    count: *mut u32,
) -> RsHandheldStatus {
    unsafe { rs_handheld_lxmf_available_at(data, length, 0, id, count) }
}

/// Validate the complete list and copy one indexed ID. Out-of-range positions
/// publish zero ID with the actual count; malformed input publishes nothing.
/// # Safety
/// Non-null readable input and disjoint aligned writable outputs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_available_at(
    data: *const u8,
    length: usize,
    index: u32,
    id: *mut [u8; 32],
    count: *mut u32,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null() || id.is_null() || count.is_null() || length > rns_resource::DATA_MAX {
            return RsHandheldStatus::ErrInvalidArg;
        }
        match relay::available_at(
            unsafe { core::slice::from_raw_parts(data, length) },
            index as usize,
        ) {
            Ok((first, total)) => {
                unsafe {
                    *id = first.unwrap_or([0; 32]);
                    *count = total as u32;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

/// Borrow the sole fetched entry's span; an empty response returns length zero.
/// # Safety
/// Non-null readable input and disjoint writable offsets. Keep input live and
/// unchanged until the borrowed span has been consumed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_lxmf_fetched_view(
    data: *const u8,
    length: usize,
    offset: *mut usize,
    entry_length: *mut usize,
) -> RsHandheldStatus {
    guard(|| {
        if data.is_null()
            || offset.is_null()
            || entry_length.is_null()
            || length > rns_resource::DATA_MAX
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        match relay::fetched_message(unsafe { core::slice::from_raw_parts(data, length) }) {
            Ok(value) => {
                let (start, n) =
                    value.map_or((0, 0), |v| (v.as_ptr() as usize - data as usize, v.len()));
                unsafe {
                    *offset = start;
                    *entry_length = n;
                }
                RsHandheldStatus::Ok
            }
            Err(error) => failure(error),
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    unsafe fn context(private: &[u8; 64]) -> *mut RsHandheldRns {
        let mut ctx = core::ptr::null_mut();
        assert_eq!(
            unsafe { rs_handheld_rns_init(&mut ctx) },
            RsHandheldStatus::Ok
        );
        assert_eq!(
            unsafe { rs_handheld_rns_load_identity(ctx, private) },
            RsHandheldStatus::Ok
        );
        ctx
    }

    #[test]
    fn relay_ffi_roundtrip_sizes_stamps_and_retained_ratchets() {
        unsafe {
            for ratcheted in [false, true] {
                let sender = context(&[0x31; 64]);
                let recipient = context(&[0x42; 64]);
                let base_only = context(&[0x42; 64]);
                let identity = LocalIdentity::from_private_key(&[0x42; 64]);
                let now = 10000;
                if ratcheted {
                    let (announced, _) =
                        test_persist_commit_ratchet(recipient, &[0x51; 32], now, 0);
                    let mut changed = 0;
                    assert_eq!(
                        rs_handheld_rns_peer_ratchet_remember(
                            sender,
                            &identity.lxmf_delivery_hash(),
                            &announced,
                            now,
                            0,
                            &mut changed
                        ),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(changed, 1);
                    test_persist_commit_ratchet(
                        recipient,
                        &[0x52; 32],
                        now + rns_lite_core::ratchet::RATCHET_INTERVAL_SECS,
                        0,
                    );
                }
                for content_length in [0, 12, 400, 1500, 3360] {
                    let mut buffer = [0; rns_resource::DATA_MAX];
                    let content = vec![0x61; content_length];
                    let (mut n, mut destination, mut mid) = (0, [0; 16], [0; 32]);
                    assert_eq!(
                        rs_handheld_rns_lxmf_build_link(
                            sender,
                            identity.public_key(),
                            123.25,
                            b"title".as_ptr(),
                            5,
                            content.as_ptr(),
                            content.len(),
                            buffer.as_mut_ptr(),
                            buffer.len(),
                            &mut n,
                            &mut destination,
                            &mut mid
                        ),
                        RsHandheldStatus::Ok
                    );
                    let (mut expected_entry, mut expected_upload) = (0, 0);
                    assert_eq!(
                        rs_handheld_lxmf_relay_size(
                            n,
                            1,
                            &mut expected_entry,
                            &mut expected_upload
                        ),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(
                        rs_handheld_lxmf_append_stamp(
                            buffer.as_mut_ptr(),
                            n,
                            buffer.len(),
                            &[0x71; 32],
                            &mut n
                        ),
                        RsHandheldStatus::Ok
                    );
                    let original = buffer[..n].to_vec();
                    let mut transient = [0; 32];
                    assert_eq!(
                        rs_handheld_lxmf_relay_encrypt(
                            sender,
                            identity.public_key(),
                            now,
                            0,
                            &[0x81; 32],
                            &[0x91; 16],
                            buffer.as_mut_ptr(),
                            n,
                            buffer.len(),
                            &mut n,
                            &mut transient
                        ),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(n, expected_entry);
                    let encrypted = buffer[..n].to_vec();
                    let mut recomputed = [0; 32];
                    assert_eq!(
                        rs_handheld_lxmf_transient_id(buffer.as_ptr(), n, &mut recomputed),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(transient, recomputed);
                    let mut count = 0;
                    assert_eq!(
                        rs_handheld_lxmf_relay_upload(
                            buffer.as_mut_ptr(),
                            n,
                            buffer.len(),
                            456.5,
                            &[0; 32],
                            &mut count
                        ),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(count, expected_upload);
                    // Free nodes still receive the mandatory all-zero 32-byte stamp.
                    assert_eq!(&buffer[count - 32..count], &[0; 32]);
                    if ratcheted {
                        let mut wrong = encrypted.clone();
                        assert_eq!(
                            rs_handheld_lxmf_relay_decrypt(
                                base_only,
                                wrong.as_mut_ptr(),
                                wrong.len(),
                                &mut count,
                                &mut recomputed
                            ),
                            RsHandheldStatus::ErrCrypto
                        );
                    }
                    buffer[..n].copy_from_slice(&encrypted);
                    assert_eq!(
                        rs_handheld_lxmf_relay_decrypt(
                            recipient,
                            buffer.as_mut_ptr(),
                            n,
                            &mut count,
                            &mut recomputed
                        ),
                        RsHandheldStatus::Ok
                    );
                    assert_eq!(&buffer[..count], original.as_slice());
                    assert_eq!(transient, recomputed);
                    // The fifth stamp element does not change the signed message ID.
                    let source = (*sender).identity.as_ref().unwrap();
                    let mut scratch = [0; rns_resource::DATA_MAX];
                    let view = lxmf_codec::parse_link(
                        &identity,
                        &buffer[..count],
                        source.public_key(),
                        &mut scratch,
                    )
                    .unwrap();
                    assert_eq!(view.message_id, mid);
                }
                rs_handheld_rns_shutdown(sender);
                rs_handheld_rns_shutdown(recipient);
                rs_handheld_rns_shutdown(base_only);
            }
        }
    }

    #[test]
    fn relay_ffi_rejects_oversize_and_malformed_rpc_without_publishing() {
        unsafe {
            let (mut entry, mut upload) = (7, 9);
            assert_eq!(
                rs_handheld_lxmf_relay_size(usize::MAX, 1, &mut entry, &mut upload),
                RsHandheldStatus::ErrCapacity
            );
            assert_eq!(
                rs_handheld_lxmf_relay_size(rns_resource::DATA_MAX, 0, &mut entry, &mut upload),
                RsHandheldStatus::ErrCapacity
            );
            assert_eq!((entry, upload), (7, 9));
            let mut id = [0xAA; 32];
            let mut count = 88;
            let invalid = [0x92, 0xc4, 32];
            assert_eq!(
                rs_handheld_lxmf_available_first(
                    invalid.as_ptr(),
                    invalid.len(),
                    &mut id,
                    &mut count
                ),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!((id, count), ([0xAA; 32], 88));
            assert_eq!(
                rs_handheld_lxmf_available_first([0x90].as_ptr(), 1, &mut id, &mut count),
                RsHandheldStatus::Ok
            );
            assert_eq!((id, count), ([0; 32], 0));
            let list = [0x92, 0xc4, 32]
                .into_iter()
                .chain([1; 32])
                .chain([0xc4, 32])
                .chain([2; 32])
                .collect::<Vec<_>>();
            assert_eq!(
                rs_handheld_lxmf_available_at(list.as_ptr(), list.len(), 1, &mut id, &mut count),
                RsHandheldStatus::Ok
            );
            assert_eq!((id, count), ([2; 32], 2));
            assert_eq!(
                rs_handheld_lxmf_available_at(list.as_ptr(), list.len(), 2, &mut id, &mut count),
                RsHandheldStatus::Ok
            );
            assert_eq!((id, count), ([0; 32], 2));
            let fetch = [0x91, 0xc4, 3, 1, 2, 3];
            assert_eq!(
                rs_handheld_lxmf_fetched_view(fetch.as_ptr(), fetch.len(), &mut entry, &mut upload),
                RsHandheldStatus::Ok
            );
            assert_eq!((entry, upload), (3, 3));
            assert_eq!(
                rs_handheld_lxmf_fetched_view([0x91, 0x01].as_ptr(), 2, &mut entry, &mut upload),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!((entry, upload), (3, 3));
        }
    }
}
