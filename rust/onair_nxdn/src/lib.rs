// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir NXDN: the common air interface frame layer of NXDN TS 1-A: the PN9 scrambler, LICH, the CRC-6 / 12 / 16, the three coded channels
//! (SACCH, FACCH1, CAC: K=5 convolutional code, puncturing, block interleaver, soft Viterbi), the layer 3 messages (build / parse), names, and
//! the AMBE+2 voice bit order. Demodulator, frame sync search, SACCH superframe assembly and call state stay in C++ (`core/src/nxdn/nxdn_rx.cpp`).
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // matrix / table code is clearer with explicit indices, as in the specification

pub mod chan;
pub mod crc;
pub mod lich;
pub mod msg;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
