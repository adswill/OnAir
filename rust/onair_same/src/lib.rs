// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir SAME: NOAA Weather Radio SAME / EAS alert headers (47 CFR 11.31): header parsing and checking, the originator / event /
//! state / province tables, location codes in words, issue and purge times, and the county-table CSV reader.
//!
//! The format is `ZCZC-ORG-EEE-PSSCCC[-PSSCCC...]+TTTT-JJJHHMM-LLLLLLLL-` and `NNNN` for the end of a message. All code here is safe,
//! allocation-free and panic-free: text goes into caller buffers through [`text::Buf`]. Only `ffi` uses `unsafe`.
//! The county table itself is state (the user loads it) and stays on the C++ side; its text is turned into records by [`county`].
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod county;
#[allow(unsafe_code)]
pub mod ffi;
pub mod locate;
pub mod parse;
pub mod tables;
pub mod text;
pub mod time;

#[cfg(test)]
mod tests;
