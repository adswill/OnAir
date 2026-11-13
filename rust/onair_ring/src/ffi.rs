// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_ring` (core/include/dect2/rs/ring.h). Every export is `onair_ring_*`. The caller owns the storage and the header
//! block (`onair_ring_hdr_size()` bytes, 128-aligned) and keeps both alive and at the same address while the ring is used.
//! Safety contract of every function taking `h`: it is a header initialised by `onair_ring_init` that is still alive; the thread rules in
//! ring.rs apply.
#![allow(unsafe_code, clippy::missing_safety_doc)]

use crate::ring::Ring;
use core::mem::{align_of, size_of};

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_ring_version() -> u32 {
    1
}

/// Bytes the caller must reserve for the header block, and its alignment.
#[no_mangle]
pub extern "C" fn onair_ring_hdr_size() -> usize {
    size_of::<Ring>()
}

#[no_mangle]
pub extern "C" fn onair_ring_hdr_align() -> usize {
    align_of::<Ring>()
}

/// Builds an empty ring in `hdr` over `buf` (`cap` elements of `elem` bytes, `cap` a power of two). 0 = ok, -1 = bad arguments.
///
/// # Safety
/// `hdr` valid and aligned for `onair_ring_hdr_size()` bytes; `buf` valid for `cap * elem` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ring_init(hdr: *mut u8, buf: *mut u8, cap: usize, elem: usize, start: usize) -> i32 {
    if hdr.is_null() || (hdr as usize) % align_of::<Ring>() != 0 {
        return -1;
    }
    match Ring::new(buf, cap, elem, start) {
        Some(r) => {
            core::ptr::write(hdr as *mut Ring, r);
            0
        }
        None => -1,
    }
}

unsafe fn ring<'a>(h: *const u8) -> &'a Ring {
    &*(h as *const Ring)
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_capacity(h: *const u8) -> usize {
    ring(h).capacity()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_write(h: *const u8, src: *const u8, n: usize, now_ns: i64) -> usize {
    ring(h).write(src, n, now_ns)
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_read(h: *const u8, dst: *mut u8, n: usize) -> usize {
    ring(h).read(dst, n)
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_skip_to_newest(h: *const u8, keep: usize) -> usize {
    ring(h).skip_to_newest(keep)
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_drop_backlog(h: *const u8) -> usize {
    ring(h).drop_backlog()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_available(h: *const u8) -> usize {
    ring(h).available()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_dropped(h: *const u8) -> u64 {
    ring(h).dropped()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_note_lost(h: *const u8, n: u64) {
    ring(h).note_lost(n)
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_clear(h: *const u8) {
    ring(h).clear()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ring_reset_dropped(h: *const u8) {
    ring(h).reset_dropped()
}

/// 1 and the stamp in the three out parameters, or 0 while nothing was written yet.
#[no_mangle]
pub unsafe extern "C" fn onair_ring_last_write(h: *const u8, offered: *mut u64, ns: *mut i64, n: *mut usize) -> i32 {
    match ring(h).last_write() {
        Some((o, t, k)) => {
            *offered = o;
            *ns = t;
            *n = k;
            1
        }
        None => 0,
    }
}
