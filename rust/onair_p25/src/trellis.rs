// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The P25 rate 1/2 trellis code of TSBKs and PDU headers (TIA-102.BAAA, data block coding) with its 98-dibit interleave, and the CRC-CCITT of those blocks.
//! 48 input dibits plus a flushing zero dibit give 49 four-bit outputs = 98 dibits; decoding is a Viterbi search over the 4 states on received levels
//! (about -3..3, C4FM).

const NEXT: [[u8; 4]; 4] = [[0x2, 0xC, 0x1, 0xF], [0xE, 0x0, 0xD, 0x3], [0x9, 0x7, 0xA, 0x4], [0x5, 0xB, 0x6, 0x8]];

/// Interleave: transmit position i carries encoder output dibit `IL[i]` (four runs: columns 0, 2, 4, 6 stepping by 8, pairs of dibits).
const fn interleave() -> [u8; 98] {
    let mut t = [0u8; 98];
    let mut k = 0;
    let mut c = 0;
    while c < 4 {
        let mut i = 2 * c;
        while i < 98 {
            t[k] = i as u8;
            t[k + 1] = (i + 1) as u8;
            k += 2;
            i += 8;
        }
        c += 1;
    }
    t
}
static IL: [u8; 98] = interleave();

/// C4FM dibit to level: 01 +3, 00 +1, 10 -1, 11 -3.
pub fn dibit_to_sym(d: i32) -> i32 {
    [1, 3, -1, -3][(d & 3) as usize]
}

/// CRC-CCITT (polynomial 0x1021, start 0, complemented) over `bits` (one per byte).
pub fn crc_ccitt(bits: &[u8]) -> u16 {
    let mut c: u16 = 0;
    for &b in bits {
        let fb = ((c >> 15) & 1) ^ u16::from(b & 1);
        c <<= 1;
        if fb != 0 {
            c ^= 0x1021;
        }
    }
    !c
}

/// Encodes 96 bits into the 98 interleaved dibits as sent.
pub fn encode(bits: &[u8; 96], dibits: &mut [u8; 98]) {
    let mut coded = [0u8; 98];
    let mut st = 0usize;
    for i in 0..49usize {
        let inp = if i < 48 { (usize::from(bits[2 * i] & 1) << 1) | usize::from(bits[2 * i + 1] & 1) } else { 0 };
        let o = NEXT[st][inp];
        coded[2 * i] = o >> 2;
        coded[2 * i + 1] = o & 3;
        st = inp;
    }
    for i in 0..98 {
        dibits[i] = coded[IL[i] as usize];
    }
}

/// Viterbi decoder over 98 received levels: writes the 96 bits (always; meaningless when the result is false) and returns (path metric, true when a path ends
/// in the zero state).
pub fn decode(lv: &[f32; 98], bits: &mut [u8; 96]) -> (f32, bool) {
    let mut r = [0f32; 98];
    for i in 0..98 {
        r[IL[i] as usize] = lv[i];
    }
    let mut pm = [0.0f32, 1e9, 1e9, 1e9];
    let mut from = [[0u8; 4]; 49];
    for s in 0..49usize {
        let mut nm = [1e30f32; 4];
        for st in 0..4usize {
            if pm[st] >= 1e29 {
                continue;
            }
            for inp in 0..4usize {
                if s == 48 && inp != 0 {
                    continue;
                }
                let o = NEXT[st][inp];
                let a = r[2 * s] - dibit_to_sym(i32::from(o >> 2)) as f32;
                let b = r[2 * s + 1] - dibit_to_sym(i32::from(o & 3)) as f32;
                let m = pm[st] + a * a + b * b;
                if m < nm[inp] {
                    nm[inp] = m;
                    from[s][inp] = st as u8;
                }
            }
        }
        pm = nm;
    }
    let mut st = 0usize;
    let mut inp = [0u8; 49];
    for s in (0..49).rev() {
        inp[s] = st as u8;
        st = from[s][st] as usize;
    }
    for i in 0..48 {
        bits[2 * i] = inp[i] >> 1;
        bits[2 * i + 1] = inp[i] & 1;
    }
    (pm[0], pm[0] < 1e29)
}
