// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir ACARS: the ARINC 618 block layer: check sequence (CRC-16 KERMIT), odd parity, acarsdec-style parity and check-sequence repair,
//! block field parsing (mode, address, label, block id, downlink message number and flight id, text, H1 sublabel and MFI), the label names and the
//! short reading of fixed-format text (OOOI times, airports, media advisory). The block builder for the test signal stays in C++.
//!
//! Facts from acarsdec (acars.c, syndrom.h), libacars (acars.c, media-adv.c) and ARINC 618 as the decoders implement it. All code is safe,
//! allocation-free and panic-free; only `ffi` uses `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod block;
pub mod crc;
#[allow(unsafe_code)]
pub mod ffi;
pub mod label;
pub mod repair;
pub mod text;

#[cfg(test)]
mod tests;
