// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir D-STAR: the digital voice (DV) frame layer of the JARL D-STAR specification: the radio header (flags, RPT2 / RPT1 / UR / MY callsigns, suffix,
//! CRC-CCITT) with its FEC (K=3 convolutional code, 24-column interleaver, PN scrambler, soft Viterbi), flag texts, byte / bit order helpers, the AMBE voice bit
//! order, and the slow data GPS text parsers (NMEA $GPRMC / $GPGGA and D-PRS). The demodulator, frame sync search, slow data assembly and call state stay in
//! C++ (`core/src/dstar/`). Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // table code is clearer with explicit indices, as in the specification

pub mod fec;
pub mod gps;
pub mod header;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
