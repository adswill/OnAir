// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir Kris modes (KAVB, KAPP, KAFT, KAAL): the shared channel codes. A family of systematic quasi-cyclic IRA LDPC codes (n = 1920, 2304, 7680; rates 1/4 .. 9/10,
//! dual-diagonal parity, linear-time encoder, layered normalised min-sum decoder) and the K = 7 convolutional code (rate 1/2 and 1/4) with a soft Viterbi decoder.
//! `no_std` with `alloc` (the static library carries no Rust `std`); the allocator is libc malloc. Only `ffi` and `heap` may use `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)]

extern crate alloc;

pub mod conv;
pub mod erasure;
pub mod ldpc;
pub mod xorshift;

#[allow(unsafe_code)]
pub mod ffi;

/// libc malloc as the global allocator (the code needs the heap for the cached code tables and decoder scratch). Not used by the unit tests, which run with `std`.
#[cfg(not(test))]
#[allow(unsafe_code)]
mod heap {
    use core::alloc::{GlobalAlloc, Layout};
    extern "C" {
        fn malloc(n: usize) -> *mut u8;
        fn calloc(n: usize, m: usize) -> *mut u8;
        fn realloc(p: *mut u8, n: usize) -> *mut u8;
        fn free(p: *mut u8);
    }
    pub struct Libc;
    // malloc returns memory aligned for any fundamental type (16 bytes on the 64-bit targets); nothing here asks for more.
    unsafe impl GlobalAlloc for Libc {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            if l.align() > 16 {
                return core::ptr::null_mut();
            }
            malloc(l.size())
        }
        unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
            if l.align() > 16 {
                return core::ptr::null_mut();
            }
            calloc(l.size(), 1)
        }
        unsafe fn dealloc(&self, p: *mut u8, _l: Layout) {
            free(p)
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            if l.align() > 16 {
                return core::ptr::null_mut();
            }
            realloc(p, n)
        }
    }
    #[global_allocator]
    static A: Libc = Libc;
}
