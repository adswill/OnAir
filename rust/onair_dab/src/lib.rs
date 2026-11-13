// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir DAB data layers (ETSI EN 300 401, TS 101 756, EN 301 234, TS 102 563, TS 102 980, TS 102 979): the FIB / FIG parsers behind the ensemble tables and
//! labels, MOT time / header / data group / directory parsing (shared with DRM), the programme associated data split of DAB+ access units and MP2 frames,
//! dynamic label framing and DL Plus, the Journaline data group and TOC check and the packet mode headers. Ensemble, label segment, data group and MOT
//! object state stays in C++ (`core/src/dab/`). Text conversion uses `onair_dvbsi::charset`.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod crc;
pub mod ffi_types;
pub mod fig;
pub mod jl;
pub mod mot;
pub mod pad;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
