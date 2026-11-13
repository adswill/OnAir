// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! VDL Mode 2 burst bit layer: the x^15 + x + 1 scrambler, the (25,20) burst header code and the layout of the data and FEC octets of a burst. ICAO Annex 10
//! Vol III Part I section 6.3 and ICAO Doc 9776 (VDL Mode 2 technical manual), as dumpvdl2 implements them.

/// Number of data octets of one Reed-Solomon block (255,249).
pub const RS_K: usize = 249;
/// Codeword length.
pub const RS_N: usize = 255;
/// The scrambler register at the first header bit.
pub const LFSR_SEED: u16 = 0x6959;

/// XORs the scrambler sequence (one pass scrambles, the same pass descrambles) over `bits` (one bit per byte); the sequence starts with the first element.
pub fn scramble(bits: &mut [u8]) {
    let mut s: u16 = LFSR_SEED;
    for b in bits.iter_mut() {
        let x = ((s ^ (s >> 14)) & 1) as u8;
        s = (s >> 1) | ((x as u16) << 14);
        *b ^= x;
    }
}

/// Parity rows of the header code over the 25 bit word (bit 24 = first sent); the low five bits are the parity bits.
const ROWS: [&[u8; 25]; 5] = [
    b"0000000011111111111110000",
    b"0011111100001111111101000",
    b"1100011100110000111100100",
    b"1101101101010011001100010",
    b"0110100111100101010100001",
];

/// Error patterns by syndrome (bit 4 of the syndrome = row 0).
const SYND: [&[u8; 25]; 32] = [
    b"0000000000000000000000000",
    b"0000000000000000000000001",
    b"0000000000000000000000010",
    b"0100000000000000000000100",
    b"0000000000000000000000100",
    b"0100000000000000000000010",
    b"1000000000000000000000000",
    b"0100000000000000000000000",
    b"0000000000000000000001000",
    b"0010000000000000000000000",
    b"0001000000000000000000000",
    b"0000100000000000000000000",
    b"0000010000000000000000000",
    b"1000100000000000000000000",
    b"0000001000000000000000000",
    b"0000000100000000000000000",
    b"0000000000000000000010000",
    b"0000000010000000000000000",
    b"0100000000100000000000000",
    b"0000000001000000000000000",
    b"0100000001000000000000000",
    b"0000000000100000000000000",
    b"0000000000010000000000000",
    b"1000000010000000000000000",
    b"0000000000001000000000000",
    b"0000000000000100000000000",
    b"0000000000000010000000000",
    b"0000000000000001000000000",
    b"0000000000000000100000000",
    b"0000000000000000010000000",
    b"0000000000000000001000000",
    b"0000000000000000000100000",
];

fn bits25(t: &[u8; 25]) -> u32 {
    t.iter().fold(0u32, |v, &c| (v << 1) | (c - b'0') as u32)
}

/// Reverses the low `n` bits of `v`.
pub fn rev(mut v: u32, n: u32) -> u32 {
    let mut r = 0;
    for _ in 0..n {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    r
}

fn par(v: u32) -> u32 {
    v.count_ones() & 1
}

fn syndrome(w: u32) -> usize {
    let mut s = 0usize;
    for (i, r) in ROWS.iter().enumerate() {
        s |= (par(w & bits25(r)) as usize) << (4 - i);
    }
    s
}

/// The 25 header bits for a burst of `len_bits` data bits: 3 reserved zero bits, the 17 bit length sent least significant bit first, 5 parity bits.
pub fn header_word(len_bits: u32) -> u32 {
    let mut w = rev(len_bits & 0x1FFFF, 17) << 5;
    for (i, r) in ROWS.iter().enumerate() {
        w |= par(w & bits25(r)) << (4 - i);
    }
    w
}

/// What decoding a header word gives: whether it is a header, whether a correction was applied, the length, and whether the length was computed at all (the
/// reference writes its outputs in that order and leaves the length alone when the corrected word still has bits above the 22 bit field).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Header {
    pub ok: bool,
    pub corrected: bool,
    pub len: u32,
    pub have_len: bool,
}

/// Corrects the 25 bit header word and reads the length. The reserved bits are not checked; an uncorrected length may be up to 0x3FFF, a corrected one up to
/// 0x1FFF.
pub fn header_decode(word: u32) -> Header {
    let mut word = word & ((1u32 << 22) - 1);
    let s = syndrome(word);
    let mut h = Header { corrected: s != 0, ..Header::default() };
    word ^= bits25(SYND[s]);
    if word >> 22 != 0 {
        return h;
    }
    h.len = rev((word >> 5) & 0x1FFFF, 17);
    h.have_len = true;
    h.ok = h.len != 0 && h.len <= if h.corrected { 0x1FFF } else { 0x3FFF };
    h
}

/// Parity octets carried by the last (short) block of `n` data octets: 0 under 3, 2 for 3 - 30, 4 for 31 - 67, 6 above.
pub fn last_fec(n: i32) -> i32 {
    if n < 3 {
        0
    } else if n <= 30 {
        2
    } else if n <= 67 {
        4
    } else {
        6
    }
}

/// How a burst's data and FEC octets are laid out: octet counts, number of blocks, length and parity of the last block.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Layout {
    pub data_oct: i32,
    pub fec_oct: i32,
    pub blocks: i32,
    pub last_len: i32,
    pub last_fec: i32,
}

/// The layout for `len_bits` data bits; `None` when it cannot be a burst (the layout is still returned for the C ABI).
pub fn layout(len_bits: u32) -> (Layout, bool) {
    let mut l = Layout { data_oct: (len_bits.wrapping_add(7) / 8) as i32, ..Layout::default() };
    l.blocks = l.data_oct / RS_K as i32;
    l.fec_oct = l.blocks * (RS_N - RS_K) as i32;
    l.last_len = l.data_oct % RS_K as i32;
    l.last_fec = 6;
    if l.last_len != 0 {
        l.blocks += 1;
        l.last_fec = last_fec(l.last_len);
        l.fec_oct += l.last_fec;
    } else {
        l.last_len = RS_K as i32;
    }
    let ok = l.data_oct > 0 && l.fec_oct > 0;
    (l, ok)
}
