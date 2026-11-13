// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! D-STAR radio header fields (JARL D-STAR specification): the 41 byte header (3 flag bytes, RPT2, RPT1, UR, MY of 8 characters, MY suffix of 4, CRC-CCITT low
//! byte first), the CRC (reflected 0x8408, start 0xFFFF, inverted), the flag text, and the AMBE voice bit order into mbelib's `ambe_fr[4][24]`.

/// A header's callsign fields as printable text (space padding removed), with room for the longest (8 characters).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Header {
    /// Flag bytes 1..3.
    pub flag: [u8; 3],
    /// Repeater 2 (the gateway), at most 8 characters.
    pub rpt2: Text,
    /// Repeater 1 (the local repeater).
    pub rpt1: Text,
    /// Your call (the destination).
    pub ur: Text,
    /// My call.
    pub my: Text,
    /// My call suffix, at most 4 characters.
    pub sfx: Text,
}

/// Up to eight characters of text.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Text {
    /// The characters.
    pub b: [u8; 8],
    /// How many are used.
    pub n: usize,
}

/// CRC-CCITT as D-STAR uses it (reflected 0x8408, start 0xFFFF, inverted); sent low byte first.
pub fn crc(p: &[u8]) -> u16 {
    let mut c: u16 = 0xFFFF;
    for &x in p {
        c ^= u16::from(x);
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0x8408 } else { c >> 1 };
        }
    }
    !c
}

/// Writes a callsign field of `n` characters: the text, space padded.
fn put_call(o: &mut [u8], s: &[u8]) {
    for (i, x) in o.iter_mut().enumerate() {
        *x = s.get(i).copied().unwrap_or(b' ');
    }
}

/// Reads a callsign field: printable characters (others become '?'), trailing spaces removed.
fn get_call(p: &[u8]) -> Text {
    let mut t = Text::default();
    for (i, &c) in p.iter().enumerate().take(8) {
        t.b[i] = if (32..127).contains(&c) { c } else { b'?' };
        t.n = i + 1;
    }
    while t.n > 0 && t.b[t.n - 1] == b' ' {
        t.n -= 1;
    }
    t
}

/// Packs a header into 41 bytes with its CRC; each field is cut / space padded to its width.
pub fn pack(flag: [u8; 3], rpt2: &[u8], rpt1: &[u8], ur: &[u8], my: &[u8], sfx: &[u8], out: &mut [u8; 41]) {
    out[..3].copy_from_slice(&flag);
    put_call(&mut out[3..11], rpt2);
    put_call(&mut out[11..19], rpt1);
    put_call(&mut out[19..27], ur);
    put_call(&mut out[27..35], my);
    put_call(&mut out[35..39], sfx);
    let c = crc(&out[..39]);
    out[39] = c as u8;
    out[40] = (c >> 8) as u8;
}

/// Unpacks a header; the second value is false when the CRC does not match (the fields are filled anyway).
pub fn unpack(i: &[u8; 41]) -> (Header, bool) {
    let h = Header {
        flag: [i[0], i[1], i[2]],
        rpt2: get_call(&i[3..11]),
        rpt1: get_call(&i[11..19]),
        ur: get_call(&i[19..27]),
        my: get_call(&i[27..35]),
        sfx: get_call(&i[35..39]),
    };
    let c = crc(&i[..39]);
    (h, i[39] == c as u8 && i[40] == (c >> 8) as u8)
}

/// Text of the flag bytes ("data, via repeater, EMERGENCY, ..."), appended to `out` (at most 96 bytes); returns the length.
pub fn flag_text(f: &[u8; 3], out: &mut [u8]) -> usize {
    const SUB: [&[u8]; 8] = [b"", b"relay unavailable", b"no response", b"ACK", b"resend", b"unused", b"auto reply", b"repeater control"];
    let mut n = 0;
    let mut add = |t: &[u8]| {
        if n > 0 {
            for &c in b", " {
                if n < out.len() {
                    out[n] = c;
                }
                n += 1;
            }
        }
        for &c in t {
            if n < out.len() {
                out[n] = c;
            }
            n += 1;
        }
    };
    if f[0] & 0x80 != 0 {
        add(b"data");
    }
    add(if f[0] & 0x40 != 0 { b"via repeater" } else { b"simplex" });
    if f[0] & 0x20 != 0 {
        add(b"interrupted");
    }
    if f[0] & 0x10 != 0 {
        add(b"control");
    }
    if f[0] & 0x08 != 0 {
        add(b"EMERGENCY");
    }
    let s = SUB[(f[0] & 7) as usize];
    if !s.is_empty() {
        add(s);
    }
    n.min(out.len())
}

/// Spreads the 72 voice bits into the mbelib `ambe_fr[4][24]` layout (96 bytes, row major), the order DSD uses for D-STAR.
pub fn ambe_frame(bits: &[u8; 72], fr: &mut [u8; 96]) {
    const W: [u8; 72] = [
        0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 2, 1, 1, 0, 0, 3, 2, 0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 2, 1, 1,
        0, 0, 3, 2, 0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 3, 2, 1, 0, 0, 3, 3,
    ];
    const X: [u8; 72] = [
        10, 22, 11, 9, 10, 22, 11, 23, 8, 20, 9, 21, 10, 8, 9, 21, 8, 6, 7, 19, 8, 20, 9, 7, 6, 18, 7, 5, 6, 18, 7, 19, 4, 16, 5, 17, 6, 4,
        5, 17, 4, 2, 3, 15, 4, 16, 5, 3, 2, 14, 3, 1, 2, 14, 3, 15, 0, 12, 1, 13, 2, 0, 1, 13, 0, 12, 10, 11, 0, 12, 1, 13,
    ];
    fr.iter_mut().for_each(|x| *x = 0);
    for i in 0..72 {
        fr[W[i] as usize * 24 + X[i] as usize] = bits[i] & 1;
    }
}
