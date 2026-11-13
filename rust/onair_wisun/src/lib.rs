// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir Wi-SUN FAN: the IEEE 802.15.4g SUN FSK PHY header, PN9 data whitening and the two FCS types, the 802.15.4-2015 MAC header of a Wi-SUN frame (PAN ID
//! rules, auxiliary security header), the Wi-SUN header IEs (UTT, BT, FC, RSL, EA, LBC, ...), payload IEs (schedules, PAN, network name, PAN version, PHY operating
//! modes), the MPX IE with its EAPOL / EAP summary, names and the one-line frame summary. Receive only, no keys: secured frames stay encrypted. The demodulator,
//! the generator, the channel plan tables and the MPDU builder stay in C++ (`core/src/wisun/`). Safe protocol code goes in modules next to `ffi`; only `ffi` may use
//! `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::field_reassign_with_default)] // records are filled field by field after `default()`, as the C++ structs are

pub mod ie;
pub mod mac;
pub mod names;
pub mod phy;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
