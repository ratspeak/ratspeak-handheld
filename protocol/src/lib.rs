//! Reticulum and LXMF protocol bridge for Ratspeak handheld firmware.
//!
//! Exposes the bounded `rns-lite-core` and `lxmf-lite-core` APIs through
//! `include/ratspeak_protocol.h`. C++ owns hardware I/O, delivery scheduling,
//! link/resource timers, and persistence. Rust handles protocol validation,
//! routing policy, codecs, and cryptography.
//!
//! Contexts are not internally synchronized; callers must serialize access.
//! Host builds catch panics and return `RS_HANDHELD_ERR_INTERNAL`. Firmware
//! builds use `no_std` with panic-abort and a watchdog-reset panic handler.
//! Context and resource allocations use the firmware's C heap. Transport
//! tables are placed in a caller-owned buffer selected for the board.

#![cfg_attr(not(feature = "std"), no_std)]

// Unconditional so `alloc::alloc::alloc_zeroed` (used by box_zeroed) resolves in both the
// std host-test build and the no_std firmware build.
extern crate alloc;

#[cfg(not(feature = "std"))]
use alloc::boxed::Box;
use core::ffi::c_char;
#[cfg(feature = "std")]
use std::panic::{AssertUnwindSafe, catch_unwind};

use core::mem::MaybeUninit;
use core::ptr::{NonNull, addr_of_mut};

/// Allocate a heap `T` whose all-zero bit pattern is a valid value, without a by-value stack
/// temporary (a plain `Box::new(T{..})` materialises `T` on the stack first — fatal for the
/// ~3.7 KiB resource types on the small MCU task stack). Returns `None` on OOM.
///
/// SAFETY CONTRACT: only call for a non-ZST `T` where all-zero is a valid inhabitant.
fn box_zeroed<T>() -> Option<Box<T>> {
    let layout = core::alloc::Layout::new::<T>();
    debug_assert!(layout.size() != 0);
    // SAFETY: layout is non-zero-sized (asserted); alloc_zeroed returns zeroed memory that is
    // a valid `T` per the contract, and `from_raw` takes ownership of that allocation.
    let raw = unsafe { alloc::alloc::alloc_zeroed(layout) } as *mut T;
    NonNull::new(raw).map(|p| unsafe { Box::from_raw(p.as_ptr()) })
}
use lxmf_lite_core::lxmf::{self as lxmf_codec, LxmfError, LxmfView};
use rns_lite_core::constants::{
    DESTINATION_LENGTH, PRIVATE_KEY_LENGTH, PUBLIC_KEY_LENGTH, SIGNATURE_LENGTH,
};
use rns_lite_core::identity::{
    AnnounceError, AnnounceView, LXMF_DELIVERY_NAME, LocalIdentity, MAX_ANNOUNCE_APP_DATA,
    SIGNED_DATA_MAX, compose_random_hash, name_hash,
};
use rns_lite_core::link::{
    self as rns_link, LINK_KEY_LENGTH, LINK_MDU, LINK_PROOF_LEN, LINK_REQUEST_LEN, LinkError,
    LinkKeys, LinkProofView, LinkRequestView, SignallingData,
};
use rns_lite_core::proof::{self as rns_proof, ProofError, SeenMessages100};
use rns_lite_core::ratchet::{
    PEER_RATCHETS_BLOB_MAX, PeerRatchets, RATCHET_RING_BLOB_MAX, RatchetClock, RatchetPreparation,
    RatchetRing,
};
use rns_lite_core::resource::{
    self as rns_resource, InboundResource, OutboundResource, PartRequestView, ResourceAdv,
    ResourceError,
};
use rns_lite_core::wire::{
    DestinationType, HeaderType, PacketContext, PacketFlags, PacketHeader, PacketType, PacketView,
    TransportType, build_packet, packet_hash,
};
use rns_lite_core::{
    ANNOUNCE_WIRE_STATE_BLOB_LEN, AnnounceAdmission, AnnounceAdmissionConfig, AnnounceWireState,
    AutoError, IngestAction, InterfaceMode, KnownDestinationError, KnownDestinations, LiteConfig,
    LiteNode, OutboundReason, RxMeta, TransportError, TransportStats,
};

// ---- Transport table profile: one compiled LiteNode instantiation per artifact ----

/// Profile ids. Mirror `RS_HANDHELD_PROFILE_*` in `include/ratspeak_protocol.h`.
pub const RS_HANDHELD_PROFILE_SMALL: i32 = 0;
pub const RS_HANDHELD_PROFILE_MICRO: i32 = 1;

/// Reticulum interface-mode values used by the interface-scoped ingest ABI. These mirror the
/// trusted wire/runtime discriminants rather than the internal Rust enum layout.
pub const RS_HANDHELD_IFACE_MODE_FULL: i32 = 0x01;
pub const RS_HANDHELD_IFACE_MODE_ACCESS_POINT: i32 = 0x08;
pub const RS_HANDHELD_IFACE_MODE_ROAMING: i32 = 0x10;
pub const RS_HANDHELD_IFACE_MODE_BOUNDARY: i32 = 0x20;
pub const RS_HANDHELD_IFACE_MODE_GATEWAY: i32 = 0x40;

#[cfg(all(feature = "profile-small", feature = "profile-micro"))]
compile_error!("features `profile-small` and `profile-micro` are mutually exclusive");
#[cfg(not(any(feature = "profile-small", feature = "profile-micro")))]
compile_error!("enable exactly one of `profile-small` / `profile-micro`");

/// The single node type this artifact was compiled with. Keeping exactly one instantiation
/// reachable lets --gc-sections strip the other profile's transport/table code entirely.
#[cfg(feature = "profile-small")]
type ActiveNode = rns_lite_core::SmallNode;
#[cfg(feature = "profile-micro")]
type ActiveNode = rns_lite_core::MicroNode;

#[cfg(feature = "profile-small")]
const ACTIVE_PROFILE: i32 = RS_HANDHELD_PROFILE_SMALL;
#[cfg(feature = "profile-micro")]
const ACTIVE_PROFILE: i32 = RS_HANDHELD_PROFILE_MICRO;

#[cfg(feature = "profile-small")]
const ACTIVE_CONFIG: LiteConfig = LiteConfig::ESP32_LORA_TRANSPORT_SMALL;
#[cfg(feature = "profile-micro")]
const ACTIVE_CONFIG: LiteConfig = LiteConfig::ESP32_LORA_TRANSPORT_MICRO;

/// Compile-time guards: profile discriminants are ABI (header `RS_HANDHELD_PROFILE_*`), and
/// the MICRO node must keep fitting the cardputer's internal-heap budget on EVERY target
/// (this assert also runs in the xtensa staticlib build).
const _: () = {
    assert!(RS_HANDHELD_PROFILE_SMALL == 0);
    assert!(RS_HANDHELD_PROFILE_MICRO == 1);
    assert!(core::mem::size_of::<rns_lite_core::MicroNode>() <= 32 * 1024);
};

/// Announce field sizes (fixed by the Reticulum wire format).
const RANDOM_SEED_LEN: usize = 5; // entropy half of random_hash; the low 5 bytes are the timestamp
const RATCHET_LEN: usize = 32;

/// Status / error codes. Mirrors `rs_handheld_status_t` in `include/ratspeak_protocol.h`.
///
/// `#[repr(i32)]` pins the width/signedness independent of the target C-enum default, so
/// the by-value return ABI cannot drift as codes are added. New codes MUST stay in
/// `0..=i32::MAX` and the C header's enum must remain `int`-sized (do not compile the C
/// side with `-fshort-enums`).
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RsHandheldStatus {
    Ok = 0,
    ErrInvalidArg = 1,
    ErrCapacity = 2,
    ErrCrypto = 3,
    ErrNotReady = 4,
    ErrUnsupported = 5,
    /// Transient, entropy-dependent failure (resource map-hash collision): retry the same call
    /// with fresh caller entropy.
    ErrRetry = 6,
    /// The LXMF payload decrypted far enough to identify its source, but neither the durable
    /// known-destination table nor the volatile path table has that source's public key.
    ErrSourceUnknown = 7,
    ErrInternal = 255,
}

/// Compile-time guard: discriminants must match the hand-written C header. A future
/// reorder/desync fails the build instead of slipping past to a runtime ABI mismatch.
const _: () = {
    assert!(RsHandheldStatus::Ok as i32 == 0);
    assert!(RsHandheldStatus::ErrInvalidArg as i32 == 1);
    assert!(RsHandheldStatus::ErrCapacity as i32 == 2);
    assert!(RsHandheldStatus::ErrCrypto as i32 == 3);
    assert!(RsHandheldStatus::ErrNotReady as i32 == 4);
    assert!(RsHandheldStatus::ErrUnsupported as i32 == 5);
    assert!(RsHandheldStatus::ErrRetry as i32 == 6);
    assert!(RsHandheldStatus::ErrSourceUnknown as i32 == 7);
    assert!(RsHandheldStatus::ErrInternal as i32 == 255);
};

/// Opaque, Rust-owned backend context. C sees only `rs_handheld_rns_t*`.
pub struct RsHandheldRns {
    /// Active identity. `None` until `rs_handheld_rns_load_identity` succeeds.
    identity: Option<LocalIdentity>,
    /// Transport/relay node. `None` until `rs_handheld_rns_open_transport` succeeds.
    /// Points into the CALLER-owned buffer passed to open_transport (never Rust-allocated,
    /// never freed here): the node is plain no-Drop data, so shutdown drops this pointer
    /// without touching the buffer; C++ frees it only after shutdown.
    node: Option<NonNull<ActiveNode>>,
    /// Bounded set of recently-seen inbound message ids (host MAX_SEEN_IDS = 100).
    seen: SeenMessages100,
    /// Active outbound resource transfer. `None` until `resource_advertise_build`. One per
    /// context (single-resource staged scope); a new build replaces it. Boxed: ~3.7 KiB buffer.
    resource_out: Option<Box<OutboundResource>>,
    /// Active inbound resource transfer. `None` until `resource_advertise_accept`. One per
    /// context; a new accept replaces it. Boxed: ~3.8 KiB buffer.
    resource_in: Option<Box<InboundResource>>,
    /// Registered live link ids: inbound frames whose destination is one of these route to the
    /// C++ link manager as LocalFrame. Bounded fixed set; the C++ side registers/unregisters as
    /// links open/close. Endpoint scope keeps this small.
    link_ids: [Option<[u8; DESTINATION_LENGTH]>; LOCAL_LINK_SLOTS],
    /// Own announce-ratchet ring. Rotation candidates stay outside live state
    /// until the host has persisted and committed their signed bytes.
    ratchet_ring: RatchetRing,
    /// Latest accepted ratchet per peer destination. Packet ingest never mutates
    /// this table; C++ commits only after freshness and KeyMap policy accept.
    peer_ratchets: PeerRatchets,
    /// Durable 40-bit announce ordering, independent from rotation/expiry clocks.
    announce_wire: AnnounceWireState,
    /// Exact candidate digests prevent a stale/unrelated signed blob from being
    /// committed through a different prepare transaction.
    pending_ratchet: Option<[u8; 32]>,
    pending_announce_wire: Option<[u8; 32]>,
    /// Exact tag from the most recently surfaced own-destination path request. The C++ pump
    /// consumes it immediately so its bounded response cache can preserve Python tag semantics.
    own_path_request: Option<OwnPathRequest>,
}

// The context is one long-lived C-allocator allocation. Together with the caller-owned
// MicroNode placement buffer it is the Rust backend's fixed base cost on the no-PSRAM Cardputer;
// keep deliberate headroom inside the board's ~55 KiB free-heap envelope for C++ and transient
// resource buffers. These target-compiled guards catch pointer-width/layout drift as well as
// ratchet/table growth.
const _: () = {
    assert!(core::mem::size_of::<RsHandheldRns>() <= 9 * 1024);
    assert!(
        core::mem::size_of::<RsHandheldRns>() + core::mem::size_of::<rns_lite_core::MicroNode>()
            <= 41 * 1024
    );
};

#[derive(Clone, Copy)]
struct OwnPathRequest {
    tag: [u8; DESTINATION_LENGTH],
    tag_len: usize,
}

/// Registered-link slots for inbound routing. Endpoint-scale: one outbound + a few inbound.
const LOCAL_LINK_SLOTS: usize = 8;

/// NUL-terminated version string, owned by Rust (static). C must not free it.
const VERSION: &str = concat!(
    "ratspeak-handheld-protocol ",
    env!("CARGO_PKG_VERSION"),
    " (link+resource+local)\0"
);

/// Run `f` under a panic guard: a panic becomes `ErrInternal` instead of unwinding
/// across the FFI boundary.
#[cfg(feature = "std")]
fn guard<F: FnOnce() -> RsHandheldStatus>(f: F) -> RsHandheldStatus {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(RsHandheldStatus::ErrInternal)
}

/// no_std firmware profile builds with `panic = "abort"`: a panic cannot unwind, it goes
/// straight to the `#[panic_handler]` (halt -> hardware watchdog reset), so no guard is
/// possible or needed — unwinding never crosses the boundary by construction.
#[cfg(not(feature = "std"))]
fn guard<F: FnOnce() -> RsHandheldStatus>(f: F) -> RsHandheldStatus {
    f()
}

/// Bare-metal runtime for the firmware staticlib (no_std, `target_os = "none"`).
///
/// Context and resource allocations share the firmware's C heap, so ordinary
/// heap telemetry includes Rust allocations. Transport tables instead use a
/// caller-owned buffer: PSRAM for T-Deck/T-Pager, internal RAM for Cardputer.
#[cfg(all(not(feature = "std"), target_os = "none"))]
mod ffi_rt {
    use core::alloc::{GlobalAlloc, Layout};

    unsafe extern "C" {
        fn malloc(size: usize) -> *mut u8;
        fn free(ptr: *mut u8);
    }

    /// Alignment floor C malloc is trusted for. ESP-IDF's TLSF heap guarantees 4-byte
    /// alignment; anything larger takes the over-allocate-and-stash-raw-pointer path.
    const MALLOC_ALIGN: usize = 4;

    struct CMalloc;

    // SAFETY: delegates to the C heap; the over-aligned path stores the raw pointer one
    // word below the aligned block and dealloc branches on the same layout it was given.
    unsafe impl GlobalAlloc for CMalloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            if layout.align() <= MALLOC_ALIGN {
                return unsafe { malloc(layout.size().max(1)) };
            }
            let ptr_size = core::mem::size_of::<usize>();
            let Some(total) = layout
                .size()
                .checked_add(layout.align())
                .and_then(|n| n.checked_add(ptr_size))
            else {
                return core::ptr::null_mut();
            };
            let raw = unsafe { malloc(total) };
            if raw.is_null() {
                return core::ptr::null_mut();
            }
            let aligned = (raw as usize + ptr_size).next_multiple_of(layout.align());
            // SAFETY: aligned - ptr_size >= raw, inside the block just allocated.
            unsafe { ((aligned - ptr_size) as *mut usize).write_unaligned(raw as usize) };
            aligned as *mut u8
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            if layout.align() <= MALLOC_ALIGN {
                return unsafe { free(ptr) };
            }
            let ptr_size = core::mem::size_of::<usize>();
            // SAFETY: written by `alloc` for this same over-aligned layout.
            let raw = unsafe { ((ptr as usize - ptr_size) as *const usize).read_unaligned() };
            unsafe { free(raw as *mut u8) };
        }
    }

    #[global_allocator]
    static ALLOC: CMalloc = CMalloc;

    /// panic = "abort" lands here: halt; the ESP task watchdog then resets the device.
    #[panic_handler]
    fn panic(_info: &core::panic::PanicInfo) -> ! {
        loop {}
    }
}

/// Returns a static NUL-terminated version string. Never null; C must not free it.
#[unsafe(no_mangle)]
pub extern "C" fn rs_handheld_rns_version() -> *const c_char {
    VERSION.as_ptr() as *const c_char
}

/// Allocate a backend context; `*out` receives an owned pointer (free with shutdown).
/// Returns `Ok`, or `ErrInvalidArg` if `out` is null.
///
/// # Safety
/// `out` must be null, or point to a writable `*mut rs_handheld_rns_t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_init(out: *mut *mut RsHandheldRns) -> RsHandheldStatus {
    guard(|| {
        if out.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        let ctx = Box::new(RsHandheldRns {
            identity: None,
            node: None,
            seen: SeenMessages100::new(),
            resource_out: None,
            resource_in: None,
            link_ids: [None; LOCAL_LINK_SLOTS],
            ratchet_ring: RatchetRing::new(),
            peer_ratchets: PeerRatchets::new(),
            announce_wire: AnnounceWireState::new(),
            pending_ratchet: None,
            pending_announce_wire: None,
            own_path_request: None,
        });
        // SAFETY: `out` is non-null and writable per the contract above.
        unsafe { *out = Box::into_raw(ctx) };
        RsHandheldStatus::Ok
    })
}

/// Report backend status: `Ok` once an identity is loaded AND a transport node is open
/// (the protocol paths are real from that point); `ErrNotReady` before that.
/// Null `ctx` -> `ErrInvalidArg`.
///
/// # Safety
/// `ctx` must be null, or a live pointer from `rs_handheld_rns_init` not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_status(ctx: *const RsHandheldRns) -> RsHandheldStatus {
    guard(|| {
        // SAFETY: `ctx` is null or a valid `RsHandheldRns` per the contract above.
        match unsafe { ctx.as_ref() } {
            None => RsHandheldStatus::ErrInvalidArg,
            Some(ctx) if ctx.identity.is_some() && ctx.node.is_some() => RsHandheldStatus::Ok,
            Some(_) => RsHandheldStatus::ErrNotReady,
        }
    })
}

/// Free a context from `rs_handheld_rns_init`. Null is a no-op. As with all C ownership,
/// each context must be freed exactly once. The transport node buffer (caller-owned,
/// see `rs_handheld_rns_open_transport`) is NOT touched — free it after this returns.
///
/// # Safety
/// `ctx` must be null, or a pointer from `rs_handheld_rns_init` freed at most once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_rns_shutdown(ctx: *mut RsHandheldRns) {
    let _ = guard(|| {
        if !ctx.is_null() {
            // SAFETY: `ctx` is a live, once-only pointer from init per the contract.
            drop(unsafe { Box::from_raw(ctx) });
        }
        RsHandheldStatus::Ok
    });
}

mod identity;
pub use identity::*;
mod announce;
pub use announce::*;
mod transport;
pub use transport::*;
mod lxmf;
pub use lxmf::*;
mod proof;
pub use proof::*;
mod link;
pub use link::*;
mod resource;
pub use resource::*;

#[cfg(test)]
fn test_persist_commit_ratchet(
    ctx: *mut RsHandheldRns,
    entropy: &[u8; RATCHET_LEN],
    wall_secs: u64,
    uptime_ms: u64,
) -> ([u8; RATCHET_LEN], i32) {
    let mut blob = [0u8; RS_HANDHELD_RATCHET_RING_BLOB_MAX];
    let mut blob_len = 0usize;
    let mut action = -1;
    let mut public = [0u8; RATCHET_LEN];
    assert_eq!(
        unsafe {
            rs_handheld_rns_ratchet_prepare(
                ctx,
                entropy,
                wall_secs,
                uptime_ms,
                blob.as_mut_ptr(),
                blob.len(),
                &mut blob_len,
                &mut action,
                &mut public,
            )
        },
        RsHandheldStatus::Ok
    );
    if matches!(action, RATCHET_PREP_PERSIST_METADATA | RATCHET_PREP_ROTATED) {
        assert_eq!(
            unsafe { rs_handheld_rns_ratchet_commit(ctx, blob.as_ptr(), blob_len, &mut public) },
            RsHandheldStatus::Ok
        );
    }
    blob.fill(0);
    (public, action)
}

#[cfg(test)]
fn test_persist_commit_announce_wire(ctx: *mut RsHandheldRns, wall_secs: u64) -> u64 {
    let mut blob = [0u8; RS_HANDHELD_ANNOUNCE_STATE_BLOB_MAX];
    let mut blob_len = 0usize;
    let mut value = 0u64;
    let mut ready = 0;
    assert_eq!(
        unsafe {
            rs_handheld_rns_announce_state_prepare(
                ctx,
                wall_secs,
                blob.as_mut_ptr(),
                blob.len(),
                &mut blob_len,
                &mut value,
                &mut ready,
            )
        },
        RsHandheldStatus::Ok
    );
    if ready == 1 {
        assert_eq!(
            unsafe {
                rs_handheld_rns_announce_state_commit(ctx, blob.as_ptr(), blob_len, &mut value)
            },
            RsHandheldStatus::Ok
        );
    }
    value
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod ratchet_tests;
