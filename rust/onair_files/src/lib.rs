// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir file formats: the pure parsing and writing layers of the files the program reads and writes. CSV (channel lists), SigMF meta files (with the small JSON reader
//! they need), WAV headers of IQ recordings, NORAD two-line elements, the SQLite page / record / cell decoding behind the MBTiles map reader, and the header of the saved
//! results container. File access, directory searches, the b-tree walks and every piece of state stay in C++. Safe code goes in the modules next to `ffi`; only `ffi`
//! may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod cfmt;
pub mod container;
pub mod csv;
pub mod json;
pub mod sigmf;
pub mod sqlite;
pub mod tle;
pub mod wav;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;

/// A byte range of a text the caller gave (text fields come back as ranges; the caller copies them out).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Span {
    /// First byte.
    pub off: u32,
    /// Number of bytes.
    pub len: u32,
}
