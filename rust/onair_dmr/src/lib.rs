// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir DMR: the Tier I / II / III air interface frame layer of ETSI TS 102 361-1 / -2 and TS 102 361-4: error control codes (Golay, QR, Hamming, Reed-Solomon,
//! CRCs, BPTC (196,96), embedded LC, short LC, CACH, rate 3/4 trellis, rate 1), the nine sync patterns and burst layout, slot type and EMB, full link
//! control, CSBK, data headers and data blocks, message text conversion and the talker alias. The demodulator, sync search, slot tracking, call logic
//! (`core/src/dmr/dmr_link.cpp`, `dmr_rx.cpp`), the generator, the AMBE vocoder glue and the talker alias PDU builder stay in C++.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // matrix / table code is clearer with explicit indices, as in the specification

pub mod abi;
pub mod bits;
pub mod burst;
pub mod codes;
pub mod data;
pub mod lc;
pub mod product;
pub mod text;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
