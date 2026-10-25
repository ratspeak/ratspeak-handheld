use super::*;

#[test]
fn foreign_next_hop_cannot_surface_local_or_poison_final_hop_copy() {
    for enabled in [0, 1] {
        for (packet_type, destination_type, context) in [
            (
                PacketType::Data,
                DestinationType::Single,
                PacketContext::None,
            ),
            (
                PacketType::LinkRequest,
                DestinationType::Single,
                PacketContext::None,
            ),
            (
                PacketType::Proof,
                DestinationType::Single,
                PacketContext::None,
            ),
            (PacketType::Data, DestinationType::Link, PacketContext::None),
            (
                PacketType::Data,
                DestinationType::Link,
                PacketContext::Channel,
            ),
            (
                PacketType::Proof,
                DestinationType::Link,
                PacketContext::Lrproof,
            ),
        ] {
            let ctx = loaded_ctx();
            let _node = open_transport_into(ctx, enabled);
            let mut destination = [0; 16];
            assert_eq!(
                unsafe { rs_handheld_rns_destination_hash(ctx, &mut destination) },
                RsHandheldStatus::Ok
            );
            if destination_type == DestinationType::Link {
                destination = [0x67; 16];
                assert_eq!(
                    unsafe { rs_handheld_rns_link_register(ctx, &destination) },
                    RsHandheldStatus::Ok
                );
            }
            let header = PacketHeader {
                flags: PacketFlags {
                    header_type: HeaderType::Header1,
                    context_flag: false,
                    transport_type: TransportType::Broadcast,
                    destination_type,
                    packet_type,
                },
                hops: 1,
                transport_id: None,
                destination_hash: destination,
                context,
            };
            // Routing fixtures still need a structurally valid handshake shape;
            // authentication belongs to the endpoint owner after local dispatch.
            let payload: &[u8] = if packet_type == PacketType::LinkRequest {
                &[0x42; 64]
            } else if context == PacketContext::Lrproof {
                &[0x43; 96]
            } else {
                b"same encrypted payload survives relay header rewriting"
            };
            let final_hop = build_packet(header, payload).unwrap();
            let overheard = build_packet(
                PacketHeader {
                    flags: PacketFlags {
                        header_type: HeaderType::Header2,
                        transport_type: TransportType::Transport,
                        ..header.flags
                    },
                    hops: 0,
                    transport_id: Some([0x99; 16]),
                    ..header
                },
                payload,
            )
            .unwrap();
            let hash = packet_hash(final_hop.as_slice(), HeaderType::Header1);
            assert_eq!(hash, packet_hash(overheard.as_slice(), HeaderType::Header2));
            let mut action = -1;
            let mut local: RsHandheldLocalFrame = unsafe { std::mem::zeroed() };
            for packet in [&overheard, &overheard, &final_hop] {
                assert_eq!(
                    unsafe {
                        rs_handheld_rns_packet_ingest(
                            ctx,
                            packet.as_slice().as_ptr(),
                            packet.len(),
                            0,
                            1_000,
                            &mut action,
                            core::ptr::null_mut(),
                            &mut local,
                        )
                    },
                    RsHandheldStatus::Ok
                );
                assert_eq!(
                    action,
                    if core::ptr::eq(packet, &final_hop) {
                        INGEST_LOCAL_FRAME
                    } else {
                        8
                    },
                    "next-hop ownership must survive endpoint post-processing: {packet_type:?}/{destination_type:?}/{context:?} enabled={enabled}"
                );
            }
            assert_eq!(local.packet_hash, hash);
            assert_eq!(&local.payload[..local.payload_len as usize], payload);
            unsafe { rs_handheld_rns_shutdown(ctx) };
        }
    }
}
