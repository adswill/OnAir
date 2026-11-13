// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Meshtastic helpers that are plain functions of their input: the hardware model value by name, the channel hash (Channels.cpp generateHash: xor of the name
//! bytes, then of the key bytes) and the key expansion of Channels.cpp getKey() (one byte N is the default key with the last byte raised by N - 1, 0 means no
//! encryption; other short keys are padded with zeros to 16 or 32 bytes).
use crate::names::HW_MODELS;

/// The default channel key (MeshRadio.h defaultpsk).
pub const DEFAULT_PSK: [u8; 16] = [0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01];

/// The value of a hardware model name, -1 when unknown.
pub fn hw_model_value(name: &[u8]) -> i32 {
    HW_MODELS.iter().find(|e| e.1.as_bytes() == name).map(|e| e.0).unwrap_or(-1)
}

/// Channel hash byte: xor of the name bytes and of the key bytes.
pub fn channel_hash(name: &[u8], key: &[u8]) -> u8 {
    name.iter().chain(key.iter()).fold(0u8, |h, &b| h ^ b)
}

/// Expands a pre-shared key: returns the key length written into `out` (0 = no encryption) or `None` for a key longer than 32 bytes.
pub fn expand_psk(psk: &[u8], out: &mut [u8; 32]) -> Option<usize> {
    *out = [0; 32];
    if psk.is_empty() {
        return Some(0);
    }
    if psk.len() == 1 {
        if psk[0] == 0 {
            return Some(0);
        }
        out[..16].copy_from_slice(&DEFAULT_PSK);
        out[15] = out[15].wrapping_add(psk[0]).wrapping_sub(1);
        return Some(16);
    }
    if psk.len() > 32 {
        return None;
    }
    out[..psk.len()].copy_from_slice(psk);
    Some(if psk.len() < 16 {
        16
    } else if psk.len() > 16 && psk.len() < 32 {
        32
    } else {
        psk.len()
    })
}
