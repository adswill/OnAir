//! OnAir DVB service information text and the DVB data decoders: broadcast character sets to UTF-8 (ETSI EN 300 468 Annex A, ETSI TS 101 756 Annex C), the DVB
//! text field with its character table prefix, teletext (ETSI EN 300 706 / EN 300 472: Hamming codes, G0 national option subsets, data unit decoding, page
//! renderer) and DVB subtitles (ETSI EN 300 743: PES header, segment walk and parsers, run-length pixel decoding, colour tables, region blending). Page and
//! stream state, PES reassembly and iconv stay in C++ (`core/src/tv/`, `core/src/common/text_charset.cpp`).
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // table code is clearer with explicit indices, as in the specification

pub mod charset;
mod iso8859;
pub mod subpix;
pub mod subs;
pub mod tables;
pub mod teletext;
pub mod ttxrender;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
