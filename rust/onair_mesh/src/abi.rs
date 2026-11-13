// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The flat `repr(C)` records the decoders fill (see core/include/dect2/rs/mesh.h): text is a length plus a fixed byte array (no NUL, cut if too long),
//! numbers that are "not in this packet" keep the C++ defaults (-1). Payloads are at most 255 bytes (LoRa), the array sizes are chosen so nothing is cut.

pub use onair_common::text::Str;

/// Short text (types, node ids, hashes).
pub type S64 = Str<64>;
/// Medium text (notes, roles).
pub type S128 = Str<128>;
/// Text of one protocol field (names, messages, paths).
pub type S256 = Str<256>;
/// Detail line of a packet.
pub type S2048 = Str<2048>;

/// A node update (MeshNodeUpdate without protocol and node id).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Node {
    /// Long and short name, hardware model, role, MeshCore public key in hex (empty = not in this packet).
    pub long_name: S256,
    pub short_name: S256,
    pub hw_model: S64,
    pub role: S64,
    pub public_key: S128,
    /// Position present, environment present, MeshCore signature checked / valid.
    pub has_position: u8,
    pub has_env: u8,
    pub signature_checked: u8,
    pub signature_ok: u8,
    /// Satellites in view (-1 = none) and unix time sent with the position (0 = none).
    pub sats: i32,
    pub position_time: u32,
    pub lat: f64,
    pub lon: f64,
    pub alt_m: f64,
    /// Battery, voltage and utilisation (-1 = not in this packet).
    pub battery_pct: f64,
    pub voltage: f64,
    pub channel_util_pct: f64,
    pub air_util_tx_pct: f64,
    pub temp_c: f64,
    pub humidity: f64,
    pub pressure_hpa: f64,
    /// Uptime in seconds (-1 = not in this packet).
    pub uptime_s: i64,
}

impl Default for Node {
    fn default() -> Self {
        Node {
            long_name: S256::default(),
            short_name: S256::default(),
            hw_model: S64::default(),
            role: S64::default(),
            public_key: S128::default(),
            has_position: 0,
            has_env: 0,
            signature_checked: 0,
            signature_ok: 0,
            sats: -1,
            position_time: 0,
            lat: 0.0,
            lon: 0.0,
            alt_m: 0.0,
            battery_pct: -1.0,
            voltage: -1.0,
            channel_util_pct: -1.0,
            air_util_tx_pct: -1.0,
            temp_c: 0.0,
            humidity: 0.0,
            pressure_hpa: 0.0,
            uptime_s: -1,
        }
    }
}

/// A text message (MeshTextMessage without protocol and channel).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Message {
    pub from: S256,
    pub to: S64,
    pub text: S256,
    /// Hops taken (-1 = unknown).
    pub hops: i32,
    pub packet_id: u32,
    pub reply_id: u32,
    pub sender_time: u32,
}

impl Default for Message {
    fn default() -> Self {
        Message { from: S256::default(), to: S64::default(), text: S256::default(), hops: -1, packet_id: 0, reply_id: 0, sender_time: 0 }
    }
}
