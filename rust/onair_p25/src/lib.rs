// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir P25: the Project 25 Phase 1 common air interface frame layer of TIA-102.BAAA: NID (BCH (63,16,23) + parity), the Reed-Solomon codes over GF(64)
//! of the header / link control / encryption sync, Hamming (10,6,3) of the LDU hexbits, Golay (24,12,8) and (18,6,8), CRC-CCITT, the rate 1/2 trellis code
//! with its 98-dibit interleave (soft decode), TSBK / link control / encryption sync / header field parsing, names, the IMBE voice frame interleave and the
//! channel number text. The demodulator, frame sync search, status symbol handling, call state and the IMBE vocoder glue stay in C++ (`core/src/p25/`).
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // matrix / table code is clearer with explicit indices, as in the specification

pub mod gf;
pub mod golay;
pub mod imbe;
pub mod msg;
pub mod nid;
pub mod rs;
pub mod trellis;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
