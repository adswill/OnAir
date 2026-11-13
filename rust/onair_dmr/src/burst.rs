// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR burst layout (ETSI TS 102 361-1 clauses 6 and 9): the nine frame sync patterns (table 9.2) and how a received centre field is matched to
//! them, the data type names, assembly and slicing of the 264-bit burst (132 symbols; payload, slot type, centre field), the slot type (Golay 20,8)
//! and EMB (QR 16,7,6) fields, and the 4FSK symbol mapping.
use crate::bits::{get_bits, Out};
use crate::codes::{golay2008_decode, golay2008_parity, qr1676_decode, qr1676_parity};
use crate::product::dibit_to_symbol;

/// Number of sync patterns.
pub const SYNC_COUNT: usize = 9;
/// The 48-bit sync patterns: BS voice, BS data, MS voice, MS data, standalone RC, direct mode slot 1 voice / data, slot 2 voice / data.
pub const SYNC_WORDS: [u64; SYNC_COUNT] = [
    0x755F_D7DF_75F7,
    0xDFF5_7D75_DF5D,
    0x7F7D_5DD5_7DFD,
    0xD5D7_F77F_D757,
    0x77D5_5F7D_FD77,
    0x5D57_7F77_57FF,
    0xF7FD_D5DD_FD55,
    0x7DFF_D5F5_5D5F,
    0xD755_7F5F_F7F5,
];

/// Name of sync pattern `s` ("none" when out of range), NUL-terminated.
pub fn sync_name(s: i32) -> &'static str {
    const N: [&str; SYNC_COUNT] = [
        "BS voice\0",
        "BS data\0",
        "MS voice\0",
        "MS data\0",
        "RC\0",
        "direct 1 voice\0",
        "direct 1 data\0",
        "direct 2 voice\0",
        "direct 2 data\0",
    ];
    if (0..SYNC_COUNT as i32).contains(&s) {
        N[s as usize]
    } else {
        "none\0"
    }
}

/// 1 or 2 for the direct mode patterns, 0 otherwise.
pub fn sync_direct_slot(s: i32) -> i32 {
    match s {
        5 | 6 => 1,
        7 | 8 => 2,
        _ => 0,
    }
}

/// Pattern of a family (0 BS, 1 MS, 2 direct slot 1, anything else direct slot 2) and kind.
pub fn sync_of(family: i32, voice: bool) -> i32 {
    match family {
        0 => {
            if voice {
                0
            } else {
                1
            }
        }
        1 => {
            if voice {
                2
            } else {
                3
            }
        }
        2 => {
            if voice {
                5
            } else {
                6
            }
        }
        _ => {
            if voice {
                7
            } else {
                8
            }
        }
    }
}

/// The 24 symbols (+3 / -3) of pattern `s`; false when `s` is out of range.
pub fn sync_symbols(s: i32, out: &mut [i8; 24]) -> bool {
    if !(0..SYNC_COUNT as i32).contains(&s) {
        return false;
    }
    let w = SYNC_WORDS[s as usize];
    for (i, o) in out.iter_mut().enumerate() {
        *o = dibit_to_symbol(((w >> (46 - 2 * i)) & 3) as u32) as i8;
    }
    true
}

/// How many of the 24 dibits of a received centre field differ from each pattern: (best pattern or -1 when none is within `max_diff`, best distance).
pub fn match_sync(centre48: &[u8; 48], max_diff: i32) -> (i32, i32) {
    let mut best = -1i32;
    let mut bd = 99;
    for (s, &word) in SYNC_WORDS.iter().enumerate() {
        let mut d = 0;
        for i in 0..24usize {
            let want = ((word >> (46 - 2 * i)) & 3) as u32;
            let got = (u32::from(centre48[2 * i]) << 1) | u32::from(centre48[2 * i + 1]);
            if want != got {
                d += 1;
            }
        }
        if d < bd {
            bd = d;
            best = s as i32;
        }
    }
    (if bd <= max_diff { best } else { -1 }, bd)
}

/// Data type name (table 9.x) by the low 4 bits of `dt`, NUL-terminated.
pub fn data_type_name(dt: i32) -> &'static str {
    const N: [&str; 16] = [
        "PI header\0",
        "Voice LC header\0",
        "Terminator with LC\0",
        "CSBK\0",
        "MBC header\0",
        "MBC continuation\0",
        "Data header\0",
        "Rate 1/2 data\0",
        "Rate 3/4 data\0",
        "Idle\0",
        "Rate 1 data\0",
        "Unified single block data\0",
        "reserved\0",
        "reserved\0",
        "reserved\0",
        "reserved\0",
    ];
    N[(dt & 15) as usize]
}

/// Short data type name by the low 4 bits of `dt`, NUL-terminated.
pub fn data_type_short(dt: i32) -> &'static str {
    const N: [&str; 16] = [
        "PI\0", "LC\0", "TLC\0", "CSBK\0", "MBCH\0", "MBCC\0", "DH\0", "R1/2\0", "R3/4\0", "idle\0", "R1\0", "USBD\0", "?\0", "?\0", "?\0",
        "?\0",
    ];
    N[(dt & 15) as usize]
}

/// Appends the 10 slot type bits: the first half (8 info bits + 2 parity) or the second half (10 parity bits).
fn put_slot_type(o: &mut Out, cc: i32, dt: i32, second: bool) {
    let info = (((cc & 15) << 4) | (dt & 15)) as u32;
    let par = golay2008_parity(info);
    if !second {
        o.put(u64::from(info), 8);
        o.put(u64::from(par >> 10), 2);
    } else {
        o.put(u64::from(par & 0x3FF), 10);
    }
}

/// Builds a 264-bit data burst from the 196 payload bits (transmit order): `sync` >= 0 puts that pattern in the centre, otherwise embedded
/// signalling with no LC fragment. False when `sync` is not a pattern index.
pub fn make_data_burst(cc: i32, dt: i32, payload: &[u8; 196], sync: i32, out: &mut [u8; 264]) -> bool {
    if sync >= SYNC_COUNT as i32 {
        return false;
    }
    let mut o = Out::new(out);
    o.put_slice(&payload[..98]);
    put_slot_type(&mut o, cc, dt, false);
    if sync >= 0 {
        o.put(SYNC_WORDS[sync as usize], 48);
    } else {
        // embedded signalling in place of the sync pattern: EMB (colour code, no privacy, no LC fragment), 32 bits of nothing
        let info = ((cc & 15) << 3) as u32;
        let qr = qr1676_parity(info);
        o.put(u64::from(info), 7);
        o.put(u64::from(qr >> 8), 1);
        o.put(0, 32);
        o.put(u64::from(qr & 0xFF), 8);
    }
    put_slot_type(&mut o, cc, dt, true);
    o.put_slice(&payload[98..]);
    true
}

/// Builds a voice burst from `vs216` (VS(215)..VS(0)); with `sync` >= 0 the centre carries that pattern, otherwise EMB (cc, pi, lcss) and `emb32`.
pub fn make_voice_burst(vs: &[u8; 216], sync: i32, cc: i32, pi: i32, lcss: i32, emb32: &[u8; 32], out: &mut [u8; 264]) -> bool {
    if sync >= SYNC_COUNT as i32 {
        return false;
    }
    let mut o = Out::new(out);
    o.put_slice(&vs[..108]);
    if sync >= 0 {
        o.put(SYNC_WORDS[sync as usize], 48);
    } else {
        let info = (((cc & 15) << 3) | ((pi & 1) << 2) | (lcss & 3)) as u32;
        let qr = qr1676_parity(info);
        o.put(u64::from(info), 7);
        o.put(u64::from(qr >> 8), 1);
        o.put_slice(emb32);
        o.put(u64::from(qr & 0xFF), 8);
    }
    o.put_slice(&vs[108..]);
    true
}

/// The slot type field of a burst (bits 98..108 and 156..166): (colour code, data type, errors) or `None` when Golay (20,8) cannot correct it.
pub fn burst_slot_type(burst: &[u8]) -> Option<(i32, i32, i32)> {
    let w = ((get_bits(burst, 98, 10) << 10) | get_bits(burst, 156, 10)) as u32;
    let (e, info) = golay2008_decode(w)?;
    Some(((info >> 4) as i32, (info & 15) as i32, e))
}

/// The EMB field of a burst (bits 108..116 and 148..156): the 32 embedded bits are always returned (bits 116..148, zeros past the end);
/// the fields (colour code, PI, LCSS, errors) only when QR (16,7,6) corrects it.
pub fn burst_emb(burst: &[u8], emb32: &mut [u8; 32]) -> Option<(i32, i32, i32, i32)> {
    let w = ((get_bits(burst, 108, 8) << 8) | get_bits(burst, 148, 8)) as u32;
    for (i, o) in emb32.iter_mut().enumerate() {
        *o = burst.get(116 + i).copied().unwrap_or(0);
    }
    let (e, out) = qr1676_decode(w)?;
    Some(((out >> 3) as i32, ((out >> 2) & 1) as i32, (out & 3) as i32, e))
}

/// 4FSK symbols (+3, +1, -1, -3) of a bit string, one per bit pair; returns how many were written (`bits.len() / 2` at most `out.len()`).
pub fn bits_to_symbols(bits: &[u8], out: &mut [i8]) -> usize {
    let n = (bits.len() / 2).min(out.len());
    for (i, o) in out.iter_mut().enumerate().take(n) {
        *o = dibit_to_symbol((u32::from(bits[2 * i]) << 1) | u32::from(bits[2 * i + 1])) as i8;
    }
    n
}
