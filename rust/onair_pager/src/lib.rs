// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir POCSAG/FLEX: the pager protocol layer. BCH(31,21) code words (`bch`), POCSAG word classification (`pocsag`), message text and the
//! "looks like text" heuristic (`text`), FLEX sync table, FIW and phase decoding (`flex`). Generators (`pocsagBits`, `flexPhaseBits`), the
//! POCSAG message assembler state and the demodulator stay in C++.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod bch;
pub mod flex;
pub mod pocsag;
pub mod text;

#[allow(unsafe_code)]
pub mod ffi;
