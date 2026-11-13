// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir LoRa mesh: the packet layers of Meshtastic, MeshCore, LoRaWAN, LoRa APRS and MeshCom: protobuf reader, Meshtastic header and Data / port payloads,
//! MeshCore header and key-free payloads, LoRaWAN MAC frames (MHDR, FHDR, MAC commands, join / rejoin requests), LoRa APRS TNC2 splitting and MeshCom frames,
//! name tables and key helpers. AES / HMAC / Ed25519 and the channel tables (they need keys), the LoRa PHY, the generators and the settings tables stay in C++
//! (`core/src/mesh/`). Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::field_reassign_with_default)] // records are filled field by field after `default()`, as the C++ structs are

pub mod abi;
pub mod aprs;
pub mod lorawan;
pub mod lorawan_names;
pub mod meshcore;
pub mod meshtastic;
pub mod names;
pub mod pb;
pub mod tables;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
