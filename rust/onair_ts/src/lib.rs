// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir MPEG-TS: the pure parsing layer of the MPEG-2 transport stream analyser (ISO/IEC 13818-1, ETSI EN 300 468, ATSC A/65): packet header,
//! section CRC-32, PAT / PMT / SDT / NIT / EIT / TDT / TVCT section parsers and descriptor loops, PAT packet builder. Text fields are returned as byte
//! ranges of the section: the character set conversion (iconv) and all state (section reassembly, service tables, EPG) stay in C++ (`core/src/tv/ts.cpp`).
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod crc;
pub mod eit;
pub mod packet;
pub mod si;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;

/// A byte range of the section a parser was given (text fields: the caller decodes them).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Span {
    /// First byte.
    pub off: u32,
    /// Number of bytes.
    pub len: u32,
}

/// Byte `i` of `s`, 0 past the end (reads never fail, see rust/README.md).
#[inline]
pub(crate) fn rd(s: &[u8], i: usize) -> i32 {
    s.get(i).copied().unwrap_or(0) as i32
}

/// Big-endian 16 bit value at `i`.
#[inline]
pub(crate) fn rd16(s: &[u8], i: usize) -> i32 {
    (rd(s, i) << 8) | rd(s, i + 1)
}

/// Packed BCD byte to a number (EN 300 468 Annex C); like the reference it does not check the digits.
#[inline]
pub(crate) fn bcd(v: i32) -> i32 {
    (v >> 4) * 10 + (v & 15)
}
