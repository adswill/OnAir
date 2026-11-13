// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir FEC: the shared forward error correction that is pure and hot. Byte-wise Reed-Solomon over GF(256) (DVB-T RS(204,188), ATSC RS(207,187), DAB
//! RS(120,110): one generic encoder / decoder), the DVB-T2 binary BCH codes (t = 1..12, normal and short frames), and the Viterbi decoders (see `viterbi`).
//! The LDPC decoders stay in C++ (see README.md). Safe code goes in the modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod bch;
pub mod bch_tables;
pub mod gf;
pub mod ldpc;
pub mod rs;
pub mod viterbi;
#[cfg(target_arch = "aarch64")]
pub mod viterbi_neon;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
