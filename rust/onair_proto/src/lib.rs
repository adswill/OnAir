// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir protocol frame layers in safe Rust. Everything outside `ffi` is `forbid(unsafe_code)`, `no_std` and allocation free:
//! the C ABI in `ffi` hands in slices and caller-owned output buffers, so there is no allocator, no dependencies, and all
//! indexing is bounds-checked (the profile aborts on panic instead of unwinding across the C boundary).
#![cfg_attr(not(test), no_std)]

#[forbid(unsafe_code)]
pub mod bits;
#[forbid(unsafe_code)]
pub mod frame;
#[forbid(unsafe_code)]
pub mod mmsi;
#[forbid(unsafe_code)]
pub mod msg;
#[forbid(unsafe_code)]
pub mod nmea;

pub mod ffi;
