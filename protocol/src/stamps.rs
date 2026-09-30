//! One explicitly owned, fallibly allocated proof-of-work job per firmware owner.
use super::*;
use lxmf_lite_core::stamp::{Progress, StampKind, StampSearch};

pub struct RsHandheldStamp {
    search: StampSearch,
}
const _: () = assert!(core::mem::size_of::<RsHandheldStamp>() <= 384);

#[repr(C)]
pub struct RsHandheldStampProgress {
    pub attempts: u64,
    pub stamp: [u8; 32],
    pub completed_rounds: u16,
    pub total_rounds: u16,
    pub value: u16,
    /// 0 preparing, 1 searching, 2 complete, 3 exhausted, 4 cancelled.
    pub state: u8,
}

/// Start a bounded job (kind 0 recipient, 1 propagation). The runtime owns at
/// most one, additionally limits elapsed time, and destroys it on every exit.
/// No ephemeral encryption key or plaintext is retained in this job.
///
/// # Safety
/// `material`/`seed` read 32 bytes; `out` writes a new owned pointer. Regions
/// must be non-null and disjoint. Seed is fresh CSPRNG entropy. Destroy exactly
/// once; serialize all job calls. Refusal leaves `out` unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_stamp_create(
    material: *const [u8; 32],
    kind: u8,
    cost: u8,
    seed: *const [u8; 32],
    attempt_limit: u64,
    out: *mut *mut RsHandheldStamp,
) -> RsHandheldStatus {
    guard(|| {
        if material.is_null()
            || seed.is_null()
            || out.is_null()
            || kind > 1
            || attempt_limit > 8_388_608
        {
            return RsHandheldStatus::ErrInvalidArg;
        }
        if cost > 20 {
            return RsHandheldStatus::ErrUnsupported;
        }
        let kind = if kind == 0 {
            StampKind::Message
        } else {
            StampKind::Propagation
        };
        let layout = core::alloc::Layout::new::<RsHandheldStamp>();
        // SAFETY: nonzero layout; initialize before exposing the fallible allocation.
        let raw = unsafe { alloc::alloc::alloc(layout) } as *mut RsHandheldStamp;
        if raw.is_null() {
            return RsHandheldStatus::ErrInternal;
        }
        unsafe {
            raw.write(RsHandheldStamp {
                search: StampSearch::new(*material, kind, cost, *seed, attempt_limit),
            });
            *out = raw;
        }
        RsHandheldStatus::Ok
    })
}

/// Run at most four preparation rounds or 1024 nonces per call. Firmware should
/// begin with one round / 16 nonces and use its own elapsed-time slice guard;
/// operation counts alone make no claim about ESP32 throughput.
///
/// # Safety
/// `job` is live, exclusively owned; `out` writes a disjoint aligned progress.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_stamp_step(
    job: *mut RsHandheldStamp,
    round_budget: u16,
    attempt_budget: u32,
    out: *mut RsHandheldStampProgress,
) -> RsHandheldStatus {
    guard(|| {
        if job.is_null() || out.is_null() || round_budget > 4 || attempt_budget > 1024 {
            return RsHandheldStatus::ErrInvalidArg;
        }
        // SAFETY: caller's live exclusive job and writable disjoint progress.
        let search = unsafe { &mut (*job).search };
        let progress = search.step(round_budget, attempt_budget);
        let mut result = RsHandheldStampProgress {
            attempts: search.attempts(),
            stamp: [0; 32],
            completed_rounds: 0,
            total_rounds: 0,
            value: 0,
            state: 0,
        };
        match progress {
            Progress::Preparing { completed, total } => {
                result.completed_rounds = completed;
                result.total_rounds = total;
            }
            Progress::Searching { .. } => result.state = 1,
            Progress::Complete { stamp, value } => {
                result.state = 2;
                result.stamp = stamp;
                result.value = value as u16;
            }
            Progress::Exhausted => result.state = 3,
            Progress::Cancelled => result.state = 4,
        }
        unsafe {
            *out = result;
        }
        RsHandheldStatus::Ok
    })
}

/// Cancel an active job; terminal results are stable.
/// # Safety
/// `job` is live and exclusively owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_stamp_cancel(job: *mut RsHandheldStamp) -> RsHandheldStatus {
    guard(|| {
        if job.is_null() {
            return RsHandheldStatus::ErrInvalidArg;
        }
        unsafe {
            (*job).search.cancel();
        }
        RsHandheldStatus::Ok
    })
}

/// Release a job; null is a no-op.
/// # Safety
/// A non-null pointer was returned by create, remains live/exclusively owned,
/// and is destroyed exactly once. No other caller may retain or use it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rs_handheld_stamp_destroy(job: *mut RsHandheldStamp) {
    if !job.is_null() {
        // SAFETY: exactly the allocation created above, under caller ownership.
        unsafe {
            drop(Box::from_raw(job));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn progress() -> RsHandheldStampProgress {
        RsHandheldStampProgress {
            attempts: 0,
            stamp: [0; 32],
            completed_rounds: 0,
            total_rounds: 0,
            value: 0,
            state: 99,
        }
    }
    #[test]
    fn bounded_ffi_work_cancellation_and_free_cost() {
        unsafe {
            let mut job = core::ptr::null_mut();
            let mut out = progress();
            assert_eq!(
                rs_handheld_stamp_create(&[1; 32], 1, 21, &[2; 32], 10, &mut job),
                RsHandheldStatus::ErrUnsupported
            );
            assert!(job.is_null());
            assert_eq!(
                rs_handheld_stamp_create(&[1; 32], 1, 16, &[2; 32], 10, &mut job),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                rs_handheld_stamp_step(job, 5, 0, &mut out),
                RsHandheldStatus::ErrInvalidArg
            );
            assert_eq!(out.state, 99);
            assert_eq!(
                rs_handheld_stamp_step(job, 1, 16, &mut out),
                RsHandheldStatus::Ok
            );
            assert_eq!(
                (
                    out.state,
                    out.completed_rounds,
                    out.total_rounds,
                    out.attempts
                ),
                (0, 1, 1000, 0)
            );
            rs_handheld_stamp_cancel(job);
            rs_handheld_stamp_step(job, 4, 1024, &mut out);
            assert_eq!(out.state, 4);
            rs_handheld_stamp_destroy(job);
            assert_eq!(
                rs_handheld_stamp_create(&[1; 32], 1, 0, &[2; 32], 0, &mut job),
                RsHandheldStatus::Ok
            );
            rs_handheld_stamp_step(job, 0, 0, &mut out);
            assert_eq!(out.state, 2);
            assert_eq!(out.stamp, [0; 32]);
            rs_handheld_stamp_cancel(job);
            rs_handheld_stamp_step(job, 0, 0, &mut out);
            assert_eq!(out.state, 2);
            rs_handheld_stamp_destroy(job);
        }
    }
    #[test]
    fn ffi_search_slices_match_the_lite_owner() {
        unsafe {
            let mut job = core::ptr::null_mut();
            let mut out = progress();
            assert_eq!(
                rs_handheld_stamp_create(&[3; 32], 1, 8, &[4; 32], 8192, &mut job),
                RsHandheldStatus::Ok
            );
            for _ in 0..250 {
                rs_handheld_stamp_step(job, 4, 0, &mut out);
            }
            assert_eq!((out.state, out.attempts), (1, 0));
            for _ in 0..512 {
                rs_handheld_stamp_step(job, 0, 16, &mut out);
                if out.state == 2 {
                    break;
                }
            }
            let mut trusted = StampSearch::new([3; 32], StampKind::Propagation, 8, [4; 32], 8192);
            trusted.step(1000, 0);
            let expected = trusted.step(0, 8192);
            assert_eq!(
                expected,
                Progress::Complete {
                    stamp: out.stamp,
                    value: u32::from(out.value)
                }
            );
            assert_eq!(out.attempts, trusted.attempts());
            rs_handheld_stamp_destroy(job);
        }
    }
}
