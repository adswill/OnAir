// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir ADS-B: the Mode S / ADS-B (1090 MHz) message layer: CRC-24, single-bit error correction, altitude and identity codes, compact position
//! reporting (CPR), Comm-B register inference, magnetic declination and wind helpers, and the field extraction of a whole message (DF types,
//! extended squitter type codes, velocity, target state, operational status). No signal processing here.
//!
//! Sources: ICAO Annex 10 Vol. IV (message formats, CRC), RTCA DO-260B (extended squitter formats), ICAO Doc 9871 (Comm-B registers) and
//! "The 1090 Megahertz Riddle" (Junzi Sun, https://mode-s.org/1090mhz) for the worked examples. Bit numbers in comments are the standard's: bit 1 is
//! the first (most significant) bit of the message, the ME field is bits 33-88.
//!
//! All code is safe, allocation-free and panic-free. The few C library math functions that `no_std` Rust does not have (the C++ implementation
//! uses the same ones, which keeps the results the same) are imported inside `ffi`, the only module with `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod altitude;
pub mod bits;
pub mod commb;
pub mod cpr;
pub mod crc;
#[allow(unsafe_code)]
pub mod ffi;
pub mod geo;
pub mod msg;
pub mod text;

#[cfg(test)]
mod tests;
