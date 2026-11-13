// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! EBU teletext (ETSI EN 300 706) as carried in DVB PES packets (ETSI EN 300 472): Hamming 8/4 and 24/18 decoding, bit reversal, the Latin G0 national
//! option subsets (table 36), the PES data unit walk and the decoding of one 44 byte data unit into a header / row / Fastext packet record. The page store
//! (which page a row belongs to, serial transmission, completion) stays in C++.

/// Hamming 8/4 encoder of EN 300 706 section 8.2: the transmitted byte for a nibble.
const fn hamm_encode(n: u32) -> u32 {
    let (d1, d2, d3, d4) = (n & 1, (n >> 1) & 1, (n >> 2) & 1, (n >> 3) & 1);
    let (p1, p2, p3) = (1 ^ d1 ^ d3 ^ d4, 1 ^ d1 ^ d2 ^ d4, 1 ^ d1 ^ d2 ^ d3);
    let x7 = p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
    let p4 = 1 ^ x7;
    p1 | d1 << 1 | p2 << 2 | d2 << 3 | p3 << 4 | d3 << 5 | p4 << 6 | d4 << 7
}

/// Decoder table: nibble, or -1 when the byte is further than one bit from every code word.
const HAMM_DEC: [i8; 256] = {
    let mut t = [0i8; 256];
    let mut b = 0;
    while b < 256 {
        let (mut best, mut bd) = (-1i32, 9u32);
        let mut n = 0;
        while n < 16 {
            let d = ((b as u32) ^ hamm_encode(n)).count_ones();
            if d < bd {
                bd = d;
                best = n as i32;
            }
            n += 1;
        }
        t[b] = if bd <= 1 { best as i8 } else { -1 };
        b += 1;
    }
    t
};

/// Hamming 8/4 decode: the nibble, or -1 if uncorrectable.
pub fn hamming84(b: u8) -> i32 {
    HAMM_DEC[b as usize] as i32
}

/// Reverse the bit order of a byte (teletext bytes are sent least significant bit first).
pub fn reverse8(b: u8) -> u8 {
    b.reverse_bits()
}

/// Hamming 24/18 decode of three bytes (as transmitted): the 18 data bits (D1 in bit 0), -1 if the word has two errors. A single bit error is corrected.
pub fn hamming2418(p: &[u8; 3]) -> i32 {
    let mut w = p[0] as u32 | (p[1] as u32) << 8 | (p[2] as u32) << 16;
    let mut syn = 0u32;
    for n in 1..=23u32 {
        if (w >> (n - 1)) & 1 != 0 {
            syn ^= n;
        }
    }
    let odd = (w & 0xFF_FFFF).count_ones() & 1 != 0;
    if syn == 0 {
        // fine, or only P6 is wrong
    } else if odd || syn > 23 {
        return -1;
    } else {
        w ^= 1 << (syn - 1);
    }
    let (mut d, mut k) = (0i32, 0);
    for n in 1..=23u32 {
        if n & (n - 1) == 0 {
            continue;
        }
        if (w >> (n - 1)) & 1 != 0 {
            d |= 1 << k;
        }
        k += 1;
    }
    d
}

/// The Unicode character for a G0 code (0x20..0x7F) in the Latin national option subset (C12..C14: 0 English, 1 German, 2 Swedish / Finnish / Hungarian,
/// 3 Italian, 4 French, 5 Portuguese / Spanish, 6 Czech / Slovak, others English).
pub fn g0(c: u8, national: i32) -> u32 {
    const POS: [u8; 13] = [0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E];
    const SUB: [[u32; 13]; 7] = [
        [0xA3, 0x24, 0x40, 0x2190, 0xBD, 0x2192, 0x2191, 0x23, 0x2015, 0xBC, 0x2016, 0xBE, 0xF7],
        [0x23, 0x24, 0xA7, 0xC4, 0xD6, 0xDC, 0x5E, 0x5F, 0xB0, 0xE4, 0xF6, 0xFC, 0xDF],
        [0x23, 0xA4, 0xC9, 0xC4, 0xD6, 0xC5, 0xDC, 0x5F, 0xE9, 0xE4, 0xF6, 0xE5, 0xFC],
        [0xA3, 0x24, 0xE9, 0xB0, 0xE7, 0x2192, 0x2191, 0x23, 0xF9, 0xE0, 0xF2, 0xE8, 0xEC],
        [0xE9, 0xEF, 0xE0, 0xEB, 0xEA, 0xF9, 0xEE, 0x23, 0xE8, 0xE2, 0xF4, 0xFB, 0xE7],
        [0xE7, 0x24, 0xA1, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xBF, 0xFC, 0xF1, 0xE8, 0xE0],
        [0x23, 0x16F, 0x10D, 0x165, 0x17E, 0xFD, 0xED, 0x159, 0xE9, 0xE1, 0x11B, 0xFA, 0x161],
    ];
    let set = if (0..7).contains(&national) { national as usize } else { 0 };
    for i in 0..13 {
        if c == POS[i] {
            return SUB[set][i];
        }
    }
    if c == 0x7F {
        return 0x25A0;
    }
    c as u32
}

/// What a PES packet carrying teletext holds: the offsets (into the PES) of the 44 byte payloads of the data units with id 0x02 / 0x03 and length 44.
/// Returns the number of offsets found (those past `out.len()` are counted but not stored); 0 for anything that is not an EBU teletext PES (EN 300 472).
pub fn pes_units(pes: &[u8], out: &mut [u32]) -> usize {
    let n = pes.len();
    if n < 10 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1 || pes[3] != 0xBD {
        return 0;
    }
    let hdr = 9 + pes[8] as usize;
    if hdr + 1 > n {
        return 0;
    }
    if pes[hdr] < 0x10 || pes[hdr] > 0x1F {
        return 0;
    }
    let mut d = hdr + 1;
    let mut count = 0;
    while n - d >= 2 {
        let (id, len) = (pes[d], pes[d + 1] as usize);
        if 2 + len > n - d {
            break;
        }
        if (id == 0x02 || id == 0x03) && len == 44 {
            if count < out.len() {
                out[count] = (d + 2) as u32;
            }
            count += 1;
        }
        d += 2 + len;
    }
    count
}

/// Kinds of decoded data unit.
pub const UNIT_IGNORE: u8 = 0;
/// A page header (row 0).
pub const UNIT_HEADER: u8 = 1;
/// A display row 1..24.
pub const UNIT_ROW: u8 = 2;
/// Packet X/27/0: Fastext links.
pub const UNIT_FASTEXT: u8 = 3;
/// Any other packet: counted, no effect.
pub const UNIT_OTHER: u8 = 4;

/// One decoded teletext data unit (`kind` selects the fields that mean anything).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Unit {
    /// `UNIT_*`; `UNIT_IGNORE`: bad framing code or address, not even counted.
    pub kind: u8,
    /// Header: C11 serial transmission.
    pub serial: u8,
    /// Header: the page number is decimal and not a time filler (a page is opened).
    pub page_ok: u8,
    /// Header: C4 erase page.
    pub erase: u8,
    /// Header: C5.
    pub newsflash: u8,
    /// Header: C6.
    pub subtitle: u8,
    /// Header: C10.
    pub inhibit: u8,
    /// Fastext: per link 0 = unchanged, 1 = cleared (no link), 2 = set.
    pub link_act: [u8; 6],
    /// Magazine 1..8.
    pub mag: i32,
    /// Packet address row 0..31.
    pub row: i32,
    /// Header: page number 100..899.
    pub number: i32,
    /// Header: subcode (13 bits), 0 when a nibble was bad.
    pub subcode: i32,
    /// Header: C12..C14 national option subset.
    pub national: i32,
    /// Fastext: page numbers 100..899.
    pub link_page: [i32; 6],
    /// Fastext: subcodes.
    pub link_sub: [i32; 6],
    /// Header: the 32 characters of the header text; row: the first 32 bytes of the row.
    pub head: [u8; 32],
    /// Row: the 40 characters (7 bit).
    pub row_data: [u8; 40],
}

impl Unit {
    /// A unit that is ignored.
    pub const fn ignore() -> Unit {
        Unit {
            kind: UNIT_IGNORE,
            serial: 0,
            page_ok: 0,
            erase: 0,
            newsflash: 0,
            subtitle: 0,
            inhibit: 0,
            link_act: [0; 6],
            mag: 0,
            row: 0,
            number: 0,
            subcode: 0,
            national: 0,
            link_page: [0; 6],
            link_sub: [0; 6],
            head: [0; 32],
            row_data: [0; 40],
        }
    }
}

/// Decode a data unit payload (44 bytes: line / field, framing code, 42 bytes of address and data, bit reversed as in the PES).
pub fn decode_unit(u: &[u8; 44]) -> Unit {
    let mut r = Unit::ignore();
    if u[1] != 0xE4 && u[1] != 0x27 {
        return r;
    }
    let mut b = [0u8; 42];
    for i in 0..42 {
        b[i] = reverse8(u[2 + i]);
    }
    let (a0, a1) = (hamming84(b[0]), hamming84(b[1]));
    if a0 < 0 || a1 < 0 {
        return r;
    }
    let addr = a0 | a1 << 4;
    let mut mag = addr & 7;
    if mag == 0 {
        mag = 8;
    }
    let row = addr >> 3;
    r.mag = mag;
    r.row = row;
    r.kind = UNIT_OTHER;
    if row == 0 {
        r.kind = UNIT_HEADER;
        let (un, tn) = (hamming84(b[2]), hamming84(b[3]));
        let mut nib = [0i32; 6];
        for i in 0..6 {
            nib[i] = hamming84(b[4 + i]);
        }
        r.serial = (nib[5] >= 0 && nib[5] & 1 != 0) as u8;
        if un < 0 || tn < 0 || un > 9 || tn > 9 {
            return r;
        }
        r.page_ok = 1;
        r.number = mag * 100 + tn * 10 + un;
        r.subcode = 0;
        if nib[0] >= 0 && nib[1] >= 0 && nib[2] >= 0 && nib[3] >= 0 {
            r.subcode = nib[0] | (nib[1] & 7) << 4 | nib[2] << 8 | (nib[3] & 3) << 12;
        }
        r.erase = (nib[1] >= 0 && nib[1] & 8 != 0) as u8;
        r.newsflash = (nib[3] >= 0 && nib[3] & 4 != 0) as u8;
        r.subtitle = (nib[3] >= 0 && nib[3] & 8 != 0) as u8;
        r.inhibit = (nib[4] >= 0 && nib[4] & 8 != 0) as u8;
        r.national = if nib[5] >= 0 { (nib[5] >> 1) & 7 } else { 0 };
        for i in 0..32 {
            r.head[i] = b[10 + i] & 0x7F;
        }
    } else if (1..=24).contains(&row) {
        r.kind = UNIT_ROW;
        for i in 0..40 {
            r.row_data[i] = b[2 + i] & 0x7F;
        }
    } else if row == 27 && hamming84(b[2]) == 0 {
        r.kind = UNIT_FASTEXT;
        for l in 0..6 {
            let mut n = [0i32; 6];
            let mut ok = true;
            for i in 0..6 {
                n[i] = hamming84(b[3 + l * 6 + i]);
                if n[i] < 0 {
                    ok = false;
                }
            }
            if !ok {
                continue;
            }
            let (units, tens) = (n[0], n[1]);
            if units > 9 || tens > 9 {
                r.link_act[l] = 1;
                continue;
            }
            let m = ((n[3] >> 3) & 1) | (((n[5] >> 2) & 3) << 1);
            let mut lm = (mag & 7) ^ m;
            if lm == 0 {
                lm = 8;
            }
            r.link_act[l] = 2;
            r.link_page[l] = lm * 100 + tens * 10 + units;
            r.link_sub[l] = n[2] | (n[3] & 7) << 4 | n[4] << 8 | (n[5] & 3) << 12;
        }
    }
    r
}
