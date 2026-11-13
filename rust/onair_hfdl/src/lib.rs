// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir HFDL (HF data link, ARINC 635 / ICAO Doc 9741, 1800 symbols/s in the HF aeronautical bands): the layers above the modem. Burst structure (rates,
//! known-symbol preamble), the data scrambler, the block interleaver, the rate 1/2 K = 7 convolutional code with a soft Viterbi decoder, the FCS, the ground
//! station table, and the PDU parser (SPDU squitters, MPDU headers, LPDUs, HFNPDUs with performance / frequency data, system tables and positions). The PSK
//! modulation maps (they use libm), the burst / PDU builders for the test signal, the ACARS reader (it uses the ACARS and aero code) and the receiver stay in C++
//! (`core/src/hfdl/`). Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod parse;
pub mod phy;
pub mod stations;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
