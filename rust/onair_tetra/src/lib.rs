// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir TETRA: the network information layer of ETSI EN 300 392-2, i.e. only what every cell broadcasts in the clear to all terminals:
//! the synchronisation PDU (system code, colour code, timeslot / frame / multiframe numbers, sharing mode, MCC, MNC), the system information
//! broadcast (carrier, band, duplex spacing, access parameters, location area, BS service details, security class), the neighbour cell and
//! network time broadcast (D-NWRK-BROADCAST), the ACCESS-ASSIGN slot usage and a counter-only walk over a signalling block. Nothing addressed to a
//! terminal is read: no addresses are kept, no voice, no call or short data content. The burst layers, receiver, generator and the encoders stay in C++.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod abi;
pub mod bits;
pub mod mac;
pub mod sysinfo;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
