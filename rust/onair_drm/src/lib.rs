// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir DRM (ETSI ES 201 980, ETSI EN 301 775 for DRM+): the parsing side of the signalling and data layers: the FAC and SDC block checks and their fields
//! (clause 6.3, 6.4), the text message segments (clause 6.5), the AAC audio super frame (clause 5.4), the descriptive tables (languages, programme types,
//! service counts, application names), MJD calendar conversion and the helpers of the data services (MSC data group payload, text display, text detection).
//! The demodulator, channel decoding, generators, the data service state, MOT objects and Journaline objects stay in C++ (`core/src/drm/`, `core/src/dab/`).
//! MOT and the packet mode headers are in `onair_dab` (shared with DAB). Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)] // bit and table code is clearer with explicit indices, as in the specification

pub mod aac;
pub mod bits;
pub mod data;
pub mod fac;
pub mod names;
pub mod sdc;
pub mod text;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
