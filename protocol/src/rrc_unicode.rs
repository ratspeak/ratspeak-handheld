// SPDX-License-Identifier: MIT OR Apache-2.0
// Unicode 17 Cased / Case_Ignorable tables from Rust core, esp-rs 1.95.0.0,
// rustc 95e5bda86 (library/core/src/unicode/unicode_data.rs).
// Copyright The Rust Project Developers; notices are included by the existing
// "Rust core, alloc and compiler-builtins" license component.
// Adaptation: checked indexing replaces internal unsafe assume intrinsics.
// Kept in flash so exact contextual room lowercasing needs no String allocation.
#[repr(transparent)]
struct ShortOffsetRunHeader(u32);

impl ShortOffsetRunHeader {
    const fn new(start_index: usize, prefix_sum: u32) -> Self {
        assert!(start_index < (1 << 11));
        assert!(prefix_sum < (1 << 21));

        Self((start_index as u32) << 21 | prefix_sum)
    }

    #[inline]
    const fn start_index(&self) -> usize {
        (self.0 >> 21) as usize
    }

    #[inline]
    const fn prefix_sum(&self) -> u32 {
        self.0 & ((1 << 21) - 1)
    }
}

/// # Safety
///
/// - The last element of `short_offset_runs` must be greater than `std::char::MAX`.
/// - The start indices of all elements in `short_offset_runs` must be less than `OFFSETS`.
#[inline(always)]
fn skip_search<const SOR: usize, const OFFSETS: usize>(
    needle: char,
    short_offset_runs: &[ShortOffsetRunHeader; SOR],
    offsets: &[u8; OFFSETS],
) -> bool {
    let needle = needle as u32;

    let last_idx =
        match short_offset_runs.binary_search_by_key(&(needle << 11), |header| header.0 << 11) {
            Ok(idx) => idx + 1,
            Err(idx) => idx,
        };
    // SAFETY: `last_idx` *cannot* be past the end of the array, as the last
    // element is greater than `std::char::MAX` (the largest possible needle)
    // as guaranteed by the caller.
    //
    // So, we cannot have found it (i.e. `Ok(idx) => idx + 1 != length`) and the
    // correct location cannot be past it, so `Err(idx) => idx != length` either.
    //
    // This means that we can avoid bounds checking for the accesses below, too.
    //
    // We need to use `intrinsics::assume` since the `panic_nounwind` contained
    // in `hint::assert_unchecked` may not be optimized out.
    assert!(last_idx < SOR);

    let mut offset_idx = short_offset_runs[last_idx].start_index();
    let length = if let Some(next) = short_offset_runs.get(last_idx + 1) {
        (*next).start_index() - offset_idx
    } else {
        offsets.len() - offset_idx
    };

    let prev = last_idx
        .checked_sub(1)
        .map(|prev| short_offset_runs[prev].prefix_sum())
        .unwrap_or(0);

    let total = needle - prev;
    let mut prefix_sum = 0;
    for _ in 0..(length - 1) {
        // SAFETY: It is guaranteed that `length <= OFFSETS - offset_idx`,
        // so it follows that `length - 1 + offset_idx < OFFSETS`, therefore
        // `offset_idx < OFFSETS` is always true in this loop.
        //
        // We need to use `intrinsics::assume` since the `panic_nounwind` contained
        // in `hint::assert_unchecked` may not be optimized out.
        assert!(offset_idx < OFFSETS);
        let offset = offsets[offset_idx];
        prefix_sum += offset as u32;
        if prefix_sum > total {
            break;
        }
        offset_idx += 1;
    }
    offset_idx % 2 == 1
}

#[rustfmt::skip]
pub mod case_ignorable {
    use super::ShortOffsetRunHeader;

    static SHORT_OFFSET_RUNS: [ShortOffsetRunHeader; 36] = [
        ShortOffsetRunHeader::new(0, 688), ShortOffsetRunHeader::new(11, 4957),
        ShortOffsetRunHeader::new(263, 5906), ShortOffsetRunHeader::new(265, 8125),
        ShortOffsetRunHeader::new(377, 11388), ShortOffsetRunHeader::new(411, 12293),
        ShortOffsetRunHeader::new(423, 40981), ShortOffsetRunHeader::new(435, 42232),
        ShortOffsetRunHeader::new(437, 42508), ShortOffsetRunHeader::new(439, 64286),
        ShortOffsetRunHeader::new(535, 65024), ShortOffsetRunHeader::new(539, 66045),
        ShortOffsetRunHeader::new(569, 67456), ShortOffsetRunHeader::new(575, 68097),
        ShortOffsetRunHeader::new(581, 68900), ShortOffsetRunHeader::new(593, 69291),
        ShortOffsetRunHeader::new(601, 71727), ShortOffsetRunHeader::new(727, 71995),
        ShortOffsetRunHeader::new(731, 73459), ShortOffsetRunHeader::new(797, 78896),
        ShortOffsetRunHeader::new(809, 90398), ShortOffsetRunHeader::new(813, 92912),
        ShortOffsetRunHeader::new(817, 93504), ShortOffsetRunHeader::new(823, 94031),
        ShortOffsetRunHeader::new(827, 110576), ShortOffsetRunHeader::new(837, 113821),
        ShortOffsetRunHeader::new(843, 118528), ShortOffsetRunHeader::new(847, 119143),
        ShortOffsetRunHeader::new(851, 121344), ShortOffsetRunHeader::new(861, 122880),
        ShortOffsetRunHeader::new(873, 123566), ShortOffsetRunHeader::new(889, 124139),
        ShortOffsetRunHeader::new(893, 125136), ShortOffsetRunHeader::new(907, 127995),
        ShortOffsetRunHeader::new(911, 917505), ShortOffsetRunHeader::new(913, 2032112),
    ];
    static OFFSETS: [u8; 919] = [
        168, 1, 4, 1, 1, 1, 4, 1, 2, 2, 0, 192, 4, 2, 4, 1, 9, 2, 1, 1, 251, 7, 207, 1, 5, 1, 49,
        45, 1, 1, 1, 2, 1, 2, 1, 1, 44, 1, 11, 6, 10, 11, 1, 1, 35, 1, 10, 21, 16, 1, 101, 8, 1, 10,
        1, 4, 33, 1, 1, 1, 30, 27, 91, 11, 58, 11, 4, 1, 2, 1, 24, 24, 43, 3, 44, 1, 7, 2, 5, 9, 41,
        58, 55, 1, 1, 1, 4, 8, 4, 1, 3, 7, 10, 2, 13, 1, 15, 1, 58, 1, 4, 4, 8, 1, 20, 2, 26, 1, 2,
        2, 57, 1, 4, 2, 4, 2, 2, 3, 3, 1, 30, 2, 3, 1, 11, 2, 57, 1, 4, 5, 1, 2, 4, 1, 20, 2, 22, 6,
        1, 1, 58, 1, 2, 1, 1, 4, 8, 1, 7, 2, 11, 2, 30, 1, 61, 1, 12, 1, 50, 1, 3, 1, 55, 1, 1, 3,
        5, 3, 1, 4, 7, 2, 11, 2, 29, 1, 58, 1, 2, 1, 6, 1, 5, 2, 20, 2, 28, 2, 57, 2, 4, 4, 8, 1,
        20, 2, 29, 1, 72, 1, 7, 3, 1, 1, 90, 1, 2, 7, 11, 9, 98, 1, 2, 9, 9, 1, 1, 7, 73, 2, 27, 1,
        1, 1, 1, 1, 55, 14, 1, 5, 1, 2, 5, 11, 1, 36, 9, 1, 102, 4, 1, 6, 1, 2, 2, 2, 25, 2, 4, 3,
        16, 4, 13, 1, 2, 2, 6, 1, 15, 1, 94, 1, 0, 3, 0, 3, 29, 2, 30, 2, 30, 2, 64, 2, 1, 7, 8, 1,
        2, 11, 3, 1, 5, 1, 45, 5, 51, 1, 65, 2, 34, 1, 118, 3, 4, 2, 9, 1, 6, 3, 219, 2, 2, 1, 58,
        1, 1, 7, 1, 1, 1, 1, 2, 8, 6, 10, 2, 1, 39, 1, 8, 46, 2, 12, 20, 4, 48, 1, 1, 5, 1, 1, 5, 1,
        40, 9, 12, 2, 32, 4, 2, 2, 1, 3, 56, 1, 1, 2, 3, 1, 1, 3, 58, 8, 2, 2, 64, 6, 82, 3, 1, 13,
        1, 7, 4, 1, 6, 1, 3, 2, 50, 63, 13, 1, 34, 101, 0, 1, 1, 3, 11, 3, 13, 3, 13, 3, 13, 2, 12,
        5, 8, 2, 10, 1, 2, 1, 2, 5, 49, 5, 1, 10, 1, 1, 13, 1, 16, 13, 51, 33, 0, 2, 113, 3, 125, 1,
        15, 1, 96, 32, 47, 1, 0, 1, 36, 4, 3, 5, 5, 1, 93, 6, 93, 3, 0, 1, 0, 6, 0, 1, 98, 4, 1, 10,
        1, 1, 28, 4, 80, 2, 14, 34, 78, 1, 23, 3, 102, 4, 3, 2, 8, 1, 3, 1, 4, 1, 25, 2, 5, 1, 151,
        2, 26, 18, 13, 1, 38, 8, 25, 11, 46, 3, 48, 1, 2, 4, 2, 2, 17, 1, 21, 2, 66, 6, 2, 2, 2, 2,
        12, 1, 8, 1, 35, 1, 11, 1, 51, 1, 1, 3, 2, 2, 5, 2, 1, 1, 27, 1, 14, 2, 5, 2, 1, 1, 100, 5,
        9, 3, 121, 1, 2, 1, 4, 1, 0, 1, 147, 17, 0, 16, 3, 1, 12, 16, 34, 1, 2, 1, 169, 1, 7, 1, 6,
        1, 11, 1, 35, 1, 1, 1, 47, 1, 45, 2, 67, 1, 21, 3, 0, 1, 226, 1, 149, 5, 0, 6, 1, 42, 1, 9,
        0, 3, 1, 2, 5, 4, 40, 3, 4, 1, 165, 2, 0, 4, 38, 1, 26, 5, 1, 1, 0, 2, 24, 1, 52, 6, 70, 11,
        49, 4, 123, 1, 54, 15, 41, 1, 2, 2, 10, 3, 49, 4, 2, 2, 2, 1, 4, 1, 10, 1, 50, 3, 36, 5, 1,
        8, 62, 1, 12, 2, 52, 9, 10, 4, 2, 1, 95, 3, 2, 1, 1, 2, 6, 1, 2, 1, 157, 1, 3, 8, 21, 2, 57,
        2, 3, 1, 37, 7, 3, 5, 70, 6, 13, 1, 1, 1, 1, 1, 14, 2, 85, 8, 2, 3, 1, 1, 23, 1, 84, 6, 1,
        1, 4, 2, 1, 2, 238, 4, 6, 2, 1, 2, 27, 2, 85, 8, 2, 1, 1, 2, 106, 1, 1, 1, 2, 6, 1, 1, 101,
        1, 1, 1, 2, 4, 1, 5, 0, 9, 1, 2, 0, 2, 1, 1, 4, 1, 144, 4, 2, 2, 4, 1, 32, 10, 40, 6, 2, 4,
        8, 1, 9, 6, 2, 3, 46, 13, 1, 2, 198, 1, 1, 3, 1, 1, 201, 7, 1, 6, 1, 1, 82, 22, 2, 7, 1, 2,
        1, 2, 122, 6, 3, 1, 1, 2, 1, 7, 1, 1, 72, 2, 3, 1, 1, 1, 65, 1, 0, 2, 11, 2, 52, 5, 5, 1, 1,
        1, 23, 1, 0, 17, 6, 15, 0, 12, 3, 3, 0, 5, 59, 7, 9, 4, 0, 3, 40, 2, 0, 1, 63, 17, 64, 2, 1,
        2, 13, 2, 0, 4, 1, 7, 1, 2, 0, 2, 1, 4, 0, 46, 2, 23, 0, 3, 9, 16, 2, 7, 30, 4, 148, 3, 0,
        55, 4, 50, 8, 1, 14, 1, 22, 5, 1, 15, 0, 7, 1, 17, 2, 7, 1, 2, 1, 5, 5, 62, 33, 1, 160, 14,
        0, 1, 61, 4, 0, 5, 254, 2, 243, 1, 2, 1, 7, 2, 5, 1, 9, 1, 0, 7, 109, 8, 0, 5, 0, 1, 30, 96,
        128, 240, 0,
    ];
    #[inline]
    pub fn lookup(c: char) -> bool {
        debug_assert!(!c.is_ascii());
        (c as u32) >= 0xa8 && lookup_slow(c)
    }

    #[inline(never)]
    fn lookup_slow(c: char) -> bool {
        const {
            assert!(SHORT_OFFSET_RUNS.last().unwrap().0 > char::MAX as u32);
            let mut i = 0;
            while i < SHORT_OFFSET_RUNS.len() {
                assert!(SHORT_OFFSET_RUNS[i].start_index() < OFFSETS.len());
                i += 1;
            }
        }
        // SAFETY: We just ensured the last element of `SHORT_OFFSET_RUNS` is greater than `std::char::MAX`
        // and the start indices of all elements in `SHORT_OFFSET_RUNS` are smaller than `OFFSETS.len()`.
        super::skip_search(c, &SHORT_OFFSET_RUNS, &OFFSETS)
    }
}

#[rustfmt::skip]
pub mod cased {
    use super::ShortOffsetRunHeader;

    static SHORT_OFFSET_RUNS: [ShortOffsetRunHeader; 22] = [
        ShortOffsetRunHeader::new(0, 4256), ShortOffsetRunHeader::new(51, 5024),
        ShortOffsetRunHeader::new(61, 7296), ShortOffsetRunHeader::new(65, 7958),
        ShortOffsetRunHeader::new(74, 9398), ShortOffsetRunHeader::new(149, 11264),
        ShortOffsetRunHeader::new(151, 42560), ShortOffsetRunHeader::new(163, 43824),
        ShortOffsetRunHeader::new(177, 64256), ShortOffsetRunHeader::new(183, 65313),
        ShortOffsetRunHeader::new(187, 66560), ShortOffsetRunHeader::new(191, 67456),
        ShortOffsetRunHeader::new(213, 68736), ShortOffsetRunHeader::new(221, 71840),
        ShortOffsetRunHeader::new(229, 93760), ShortOffsetRunHeader::new(231, 119808),
        ShortOffsetRunHeader::new(237, 120486), ShortOffsetRunHeader::new(274, 122624),
        ShortOffsetRunHeader::new(297, 122928), ShortOffsetRunHeader::new(303, 125184),
        ShortOffsetRunHeader::new(305, 127280), ShortOffsetRunHeader::new(307, 1241482),
    ];
    static OFFSETS: [u8; 313] = [
        170, 1, 10, 1, 4, 1, 5, 23, 1, 31, 1, 195, 1, 4, 4, 208, 2, 35, 7, 2, 30, 5, 96, 1, 42, 4,
        2, 2, 2, 4, 1, 1, 6, 1, 1, 3, 1, 1, 1, 20, 1, 83, 1, 139, 8, 166, 1, 38, 9, 41, 0, 38, 1, 1,
        5, 1, 2, 43, 1, 4, 0, 86, 2, 6, 0, 11, 5, 43, 2, 3, 64, 192, 64, 0, 2, 6, 2, 38, 2, 6, 2, 8,
        1, 1, 1, 1, 1, 1, 1, 31, 2, 53, 1, 7, 1, 1, 3, 3, 1, 7, 3, 4, 2, 6, 4, 13, 5, 3, 1, 7, 116,
        1, 13, 1, 16, 13, 101, 1, 4, 1, 2, 10, 1, 1, 3, 5, 6, 1, 1, 1, 1, 1, 1, 4, 1, 6, 4, 1, 2, 4,
        5, 5, 4, 1, 17, 32, 3, 2, 0, 52, 0, 229, 6, 4, 3, 2, 12, 38, 1, 1, 5, 1, 0, 46, 18, 30, 132,
        102, 3, 4, 1, 77, 20, 6, 1, 3, 0, 43, 1, 14, 6, 80, 0, 7, 12, 5, 0, 26, 6, 26, 0, 80, 96,
        36, 4, 36, 116, 11, 1, 15, 1, 7, 1, 2, 1, 11, 1, 15, 1, 7, 1, 2, 0, 1, 2, 3, 1, 42, 1, 9, 0,
        51, 13, 51, 93, 22, 10, 22, 0, 64, 0, 64, 32, 25, 2, 25, 0, 85, 1, 71, 1, 2, 2, 1, 2, 2, 2,
        4, 1, 12, 1, 1, 1, 7, 1, 65, 1, 4, 2, 8, 1, 7, 1, 28, 1, 4, 1, 5, 1, 1, 3, 7, 1, 0, 2, 25,
        1, 25, 1, 31, 1, 25, 1, 31, 1, 25, 1, 31, 1, 25, 1, 31, 1, 25, 1, 8, 0, 10, 1, 20, 6, 6, 0,
        62, 0, 68, 0, 26, 6, 26, 6, 26, 0,
    ];
    #[inline]
    pub fn lookup(c: char) -> bool {
        debug_assert!(!c.is_ascii());
        (c as u32) >= 0xaa && lookup_slow(c)
    }

    #[inline(never)]
    fn lookup_slow(c: char) -> bool {
        const {
            assert!(SHORT_OFFSET_RUNS.last().unwrap().0 > char::MAX as u32);
            let mut i = 0;
            while i < SHORT_OFFSET_RUNS.len() {
                assert!(SHORT_OFFSET_RUNS[i].start_index() < OFFSETS.len());
                i += 1;
            }
        }
        // SAFETY: We just ensured the last element of `SHORT_OFFSET_RUNS` is greater than `std::char::MAX`
        // and the start indices of all elements in `SHORT_OFFSET_RUNS` are smaller than `OFFSETS.len()`.
        super::skip_search(c, &SHORT_OFFSET_RUNS, &OFFSETS)
    }
}
