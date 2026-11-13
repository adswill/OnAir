// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir AX.25/APRS: the packet radio protocol layer. `frame` (AX.25 v2.2 frame parser, FCS, callsign / path / TNC-2 text, frame type names),
//! `hdlc` (flag, bit-unstuffing and FCS receiver state machine), `aprs` with `aprs_pos` and `aprs_wx` (APRS 1.0.1 information field parser),
//! `arena` (text results without allocation). Frame building, HDLC / NRZI / G3RUH transmit helpers and the demodulator stay in C++.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod aprs;
pub mod aprs_pos;
pub mod aprs_wx;
pub mod arena;
pub mod frame;
pub mod hdlc;

#[allow(unsafe_code)]
pub mod ffi;
