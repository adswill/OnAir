// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! IEEE 802.15.4g SUN FSK PHY pieces: the 2-octet PHR, PN9 data whitening and the two FCS types. Wi-SUN FAN 1.0 PHY specification, IEEE 802.15.4-2015
//! clause 20 (SUN PHY): PHR bit fields (20.2.2), data whitening (20.2.3), FCS (7.2.11).
use onair_common::crc;

/// Decoded PHR: mode switch, FCS type (true: 2-octet FCS), whitening on, 11 bit PSDU length (FCS included).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Phr {
    pub mode_switch: u8,
    pub fcs2: u8,
    pub whitened: u8,
    pub length: i32,
}

/// Splits the 16 bit PHR (sent bit 15 first): mode switch bit 15, FCS type bit 12, data whitening bit 11, length bits 10..0.
pub fn parse_phr(v: u16) -> Phr {
    Phr { mode_switch: ((v >> 15) & 1) as u8, fcs2: ((v >> 12) & 1) as u8, whitened: ((v >> 11) & 1) as u8, length: (v & 0x7FF) as i32 }
}

/// Builds the PHR from its fields (length masked to 11 bits).
pub fn build_phr(p: &Phr) -> u16 {
    (if p.mode_switch != 0 { 1u16 << 15 } else { 0 })
        | (if p.fcs2 != 0 { 1 << 12 } else { 0 })
        | (if p.whitened != 0 { 1 << 11 } else { 0 })
        | ((p.length as u32 & 0x7FF) as u16)
}

/// PN9 whitening (x^9 + x^5 + 1, register all ones at the start of the PSDU, one step per bit; each octet's bits are taken least significant first):
/// the whitening octets repeat every 2048 octets in this implementation (as the reference does). XORs in place, so it is its own inverse.
pub fn whiten(d: &mut [u8]) {
    let mut r: u32 = 0x1FF;
    let mut step = || {
        let o = r & 1;
        let nb = (r ^ (r >> 5)) & 1;
        r = (r >> 1) | (nb << 8);
        o
    };
    for _ in 0..9 {
        step();
    }
    let mut table = [0u8; 2048];
    let need = d.len().min(2048);
    for t in table.iter_mut().take(need) {
        let mut v = 0u32;
        for j in 0..8 {
            v |= step() << j;
        }
        *t = v as u8;
    }
    for (i, b) in d.iter_mut().enumerate() {
        *b ^= table[i % 2048];
    }
}

/// The 2-octet FCS: CRC-16/KERMIT (reflected 0x1021, start 0, no final xor) as the 802.15.4 2.4 GHz PHY uses it ("123456789" gives 0x2189).
pub fn crc16(d: &[u8]) -> u16 {
    let mut c: u16 = 0;
    for &b in d {
        c ^= b as u16;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0x8408 } else { c >> 1 };
        }
    }
    c
}

/// The 4-octet FCS: the IEEE 802.3 CRC-32 ("123456789" gives 0xCBF43926).
pub fn crc32(d: &[u8]) -> u32 {
    crc::crc32(d)
}

/// True when the FCS at the end of the PSDU matches (stored least significant octet first); needs at least one octet of data.
pub fn fcs_ok(p: &[u8], fcs2: bool) -> bool {
    let n = p.len();
    if fcs2 {
        if n < 3 {
            return false;
        }
        return crc16(&p[..n - 2]) == (p[n - 2] as u16 | (p[n - 1] as u16) << 8);
    }
    if n < 5 {
        return false;
    }
    let f = u32::from_le_bytes([p[n - 4], p[n - 3], p[n - 2], p[n - 1]]);
    crc32(&p[..n - 4]) == f
}
