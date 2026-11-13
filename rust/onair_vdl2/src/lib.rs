// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir VDL Mode 2 (VHF data link, 136.975 MHz and the other channels): the layers above the modem. The x^15 + x + 1 scrambler, the (25,20) burst header code and
//! the burst layout, the Reed-Solomon (255,249) code with erasures, the gathering of a burst into blocks, the HDLC deframer and FCS, the AVLC frame (addresses,
//! control, I / S / U frames) and the protocols the information field carries as text: ISO 8208 / X.25 with CLNP and COTP headers, XID link management. ICAO Annex 10
//! Vol III Part I section 6.3 and Doc 9776 as dumpvdl2 implements them. The demodulator, the burst / HDLC / frame builders for the test signal, the ACARS and ADS-C
//! readers (they use the ACARS and aero code) and the tracker stay in C++ (`core/src/vdl2/`). Safe protocol code goes in modules next to `ffi`; only `ffi` may use
//! `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod avlc;
pub mod bits;
pub mod burst;
pub mod proto;
pub mod rs;
pub mod xid;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
