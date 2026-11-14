// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The container around saved results (`core/include/dect2/results/results_io.h`): the magic `OARS`, the format number, the hash of the saved types' signature, then the
//! payload. The payload encoding itself is C++ (it walks the program's own structs); here is the header check and the hash.

/// The format number of the encoding (raise it in `results_io.h` and here together when the encoding changes).
pub const FORMAT: u32 = 1;

/// FNV-1a, 64 bit.
pub fn fnv1a(s: &[u8]) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for &c in s {
        h ^= c as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

/// The signature hash for a signature text: its FNV-1a xor the format number.
pub fn signature_hash(sig: &[u8]) -> u64 {
    fnv1a(sig) ^ FORMAT as u64
}

/// Checks the 16 byte header (`OARS`, format, hash — integers in the host's byte order, like the writer) of a blob against the expected signature hash and
/// returns the offset of the payload (16), or None for a damaged or foreign blob.
pub fn check_header(blob: &[u8], expected_hash: u64) -> Option<usize> {
    if blob.len() < 16 || &blob[..4] != b"OARS" {
        onair_common::diag_debug!(633356, "A saved blob of {} bytes is too short or does not start with OARS, so it is ignored and rebuilt.", blob.len());
        return None;
    }
    let fmt = u32::from_ne_bytes([blob[4], blob[5], blob[6], blob[7]]);
    if fmt != FORMAT {
        onair_common::diag_debug!(514747, "A saved blob has format {} but this version reads format {}, so it is ignored and rebuilt.", fmt, FORMAT);
        return None;
    }
    let mut h = [0u8; 8];
    h.copy_from_slice(&blob[8..16]);
    if u64::from_ne_bytes(h) != expected_hash {
        onair_common::diag_debug!(442183, "A saved blob was written for a different layout (signature hash mismatch), so it is ignored and rebuilt.");
        return None;
    }
    Some(16)
}

/// Writes the 16 byte header for a signature hash into `out` (at least 16 bytes); false if `out` is shorter.
pub fn write_header(hash: u64, out: &mut [u8]) -> bool {
    if out.len() < 16 {
        return false;
    }
    out[..4].copy_from_slice(b"OARS");
    out[4..8].copy_from_slice(&FORMAT.to_ne_bytes());
    out[8..16].copy_from_slice(&hash.to_ne_bytes());
    true
}
