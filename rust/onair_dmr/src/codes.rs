// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR short block codes and Reed-Solomon (ETSI TS 102 361-1 annex B): Golay (20,8), quadratic residue (16,7,6), Hamming (7,4,3), the Hamming
//! codes of the block product codes (13,9,3), (15,11,3), (16,11,4), (17,12,3), and Reed-Solomon (12,9) over GF(256) for header / terminator link control.
//! The generator matrices are those of tables B.11 to B.17 (parity part of the systematic matrix, first information bit first, first parity column on top).
use crate::bits::popcount32;

const GOLAY_ROWS: [u16; 8] = [0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB]; // table B.11 (20,8)
const QR_ROWS: [u16; 7] = [0x04F, 0x11E, 0x1B7, 0x1E2, 0x1C9, 0x0E5, 0x073]; // table B.12 (16,7,6)
const HAM743_ROWS: [u16; 4] = [0x5, 0x7, 0x6, 0x3]; // table B.17
const HAM1393_ROWS: [u16; 9] = [0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3]; // table B.14
const HAM15113_ROWS: [u16; 11] = [0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3]; // table B.15
const HAM16114_ROWS: [u16; 11] = [0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15, 0x0B, 0x16, 0x19, 0x0D, 0x07]; // table B.16
const HAM17123_ROWS: [u16; 12] = [0x1B, 0x1F, 0x1D, 0x1C, 0x0E, 0x07, 0x11, 0x1A, 0x0D, 0x14, 0x0A, 0x05]; // table B.13

/// Parity of `info` (k bits) under the parity rows of a code: XOR of the rows of the set information bits.
const fn encode_rows(rows: &[u16], k: usize, info: u32) -> u32 {
    let mut p = 0u32;
    let mut i = 0;
    while i < k {
        if (info >> (k - 1 - i)) & 1 != 0 {
            p ^= rows[i] as u32;
        }
        i += 1;
    }
    p
}

/// All codewords `(info << p) | parity(info)` of a short code, indexed by the information word.
const fn code_words<const N: usize>(rows: &[u16], k: usize, p: usize) -> [u32; N] {
    let mut w = [0u32; N];
    let mut i = 0;
    while i < N {
        w[i] = ((i as u32) << p) | encode_rows(rows, k, i as u32);
        i += 1;
    }
    w
}

static GOLAY_WORDS: [u32; 256] = code_words::<256>(&GOLAY_ROWS, 8, 12);
static QR_WORDS: [u32; 128] = code_words::<128>(&QR_ROWS, 7, 9);
static HAM743_WORDS: [u32; 16] = code_words::<16>(&HAM743_ROWS, 4, 3);

/// Nearest codeword by exhaustive search (first of the minimum distance); `None` when farther than `max_err`. Returns (errors, information word).
fn nearest(words: &[u32], w: u32, max_err: i32) -> Option<(i32, u32)> {
    let mut best = 99;
    let mut bi = 0u32;
    for (i, &cw) in words.iter().enumerate() {
        let d = popcount32(cw ^ w);
        if d < best {
            best = d;
            bi = i as u32;
        }
    }
    if best > max_err {
        None
    } else {
        Some((best, bi))
    }
}

/// Slot type (B.3.1): 4 bit colour code and 4 bit data type -> 12 parity bits.
pub fn golay2008_parity(info8: u32) -> u32 {
    encode_rows(&GOLAY_ROWS, 8, info8 & 0xFF)
}
/// Nearest Golay (20,8) codeword within 3 bit errors: (errors, info), or `None`.
pub fn golay2008_decode(word20: u32) -> Option<(i32, u32)> {
    nearest(&GOLAY_WORDS, word20 & 0xFFFFF, 3)
}
/// EMB (B.3.2): 4 bit colour code, PI, 2 bit LCSS -> 9 parity bits.
pub fn qr1676_parity(info7: u32) -> u32 {
    encode_rows(&QR_ROWS, 7, info7 & 0x7F)
}
/// Nearest QR (16,7,6) codeword within 2 bit errors.
pub fn qr1676_decode(word16: u32) -> Option<(i32, u32)> {
    nearest(&QR_WORDS, word16 & 0xFFFF, 2)
}
/// TACT (B.3.5): access type, channel, 2 bit LCSS -> 3 parity bits.
pub fn hamming743_parity(info4: u32) -> u32 {
    encode_rows(&HAM743_ROWS, 4, info4 & 0xF)
}
/// Hamming (7,4) single error correction.
pub fn hamming743_decode(word7: u32) -> Option<(i32, u32)> {
    nearest(&HAM743_WORDS, word7 & 0x7F, 1)
}

/// The Hamming codes used by the block product codes; the word holds k information bits followed by the check bits.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Hamming {
    /// (13,9,3), columns of the BPTC (196,96).
    H1393 = 0,
    /// (15,11,3), rows of the BPTC (196,96).
    H15113 = 1,
    /// (16,11,4), rows of the embedded LC.
    H16114 = 2,
    /// (17,12,3), the short LC in the CACH.
    H17123 = 3,
}

impl Hamming {
    /// From the C enum value (anything unknown is (17,12,3), like the C++ switch default).
    pub fn from_i32(v: i32) -> Hamming {
        match v {
            0 => Hamming::H1393,
            1 => Hamming::H15113,
            2 => Hamming::H16114,
            _ => Hamming::H17123,
        }
    }
    fn rows(self) -> &'static [u16] {
        match self {
            Hamming::H1393 => &HAM1393_ROWS,
            Hamming::H15113 => &HAM15113_ROWS,
            Hamming::H16114 => &HAM16114_ROWS,
            Hamming::H17123 => &HAM17123_ROWS,
        }
    }
    /// Number of information bits k.
    pub fn info_bits(self) -> usize {
        self.rows().len()
    }
    /// Number of check bits p.
    pub fn parity_bits(self) -> usize {
        match self {
            Hamming::H1393 | Hamming::H15113 => 4,
            _ => 5,
        }
    }
}

/// Syndrome -> bit position (0 = first bit of the word) of a single error, -1 none; index is the syndrome (at most 5 bits).
const fn syndrome_positions(rows: &[u16], k: usize, p: usize) -> [i8; 32] {
    let mut pos = [-1i8; 32];
    let mut j = 0;
    while j < k + p {
        let s = if j < k { rows[j] as usize } else { 1usize << (p - 1 - (j - k)) };
        pos[s] = j as i8;
        j += 1;
    }
    pos
}
static POS1393: [i8; 32] = syndrome_positions(&HAM1393_ROWS, 9, 4);
static POS15113: [i8; 32] = syndrome_positions(&HAM15113_ROWS, 11, 4);
static POS16114: [i8; 32] = syndrome_positions(&HAM16114_ROWS, 11, 5);
static POS17123: [i8; 32] = syndrome_positions(&HAM17123_ROWS, 12, 5);

/// Check bits of `info` for Hamming code `k`.
pub fn hamming_parity(k: Hamming, info: u32) -> u32 {
    encode_rows(k.rows(), k.info_bits(), info)
}

/// Corrects a single bit error in `word`: 0 clean, 1 one bit corrected, -1 not correctable (word untouched).
pub fn hamming_correct(kind: Hamming, word: &mut u32) -> i32 {
    let (k, p) = (kind.info_bits(), kind.parity_bits());
    let info = *word >> p;
    let par = *word & ((1u32 << p) - 1);
    let syn = encode_rows(kind.rows(), k, info) ^ par;
    if syn == 0 {
        return 0;
    }
    let pos = match kind {
        Hamming::H1393 => &POS1393,
        Hamming::H15113 => &POS15113,
        Hamming::H16114 => &POS16114,
        Hamming::H17123 => &POS17123,
    };
    let j = pos[syn as usize];
    if j < 0 {
        return -1;
    }
    *word ^= 1u32 << (k + p - 1 - j as usize);
    1
}

/// GF(256) tables for x^8 + x^4 + x^3 + x^2 + 1 (B.14): (exp doubled to 512 entries, log).
const fn gf_tables() -> ([u8; 512], [u8; 256]) {
    let mut ex = [0u8; 512];
    let mut lg = [0u8; 256];
    let mut x: u32 = 1;
    let mut i = 0;
    while i < 255 {
        ex[i] = x as u8;
        lg[x as usize] = i as u8;
        x <<= 1;
        if x & 0x100 != 0 {
            x ^= 0x11D;
        }
        i += 1;
    }
    let mut i = 255;
    while i < 512 {
        ex[i] = ex[i - 255];
        i += 1;
    }
    (ex, lg)
}
static GF: ([u8; 512], [u8; 256]) = gf_tables();

/// GF(256) product.
fn mul(a: u8, b: u8) -> u8 {
    if a != 0 && b != 0 {
        GF.0[GF.1[a as usize] as usize + GF.1[b as usize] as usize]
    } else {
        0
    }
}

const RS_GEN: [u8; 3] = [0x0E, 0x38, 0x40]; // g(x) = x^3 + 0e x^2 + 38 x + 40 (B.10)

/// Reed-Solomon (12,9) parity of 9 message bytes.
pub fn rs129_parity(msg: &[u8; 9]) -> [u8; 3] {
    let mut r = [0u8; 3];
    for &m in msg {
        let fb = m ^ r[0];
        r[0] = r[1] ^ mul(fb, RS_GEN[0]);
        r[1] = r[2] ^ mul(fb, RS_GEN[1]);
        r[2] = mul(fb, RS_GEN[2]);
    }
    r
}

/// Corrects one wrong symbol of a (12,9) word (parity bytes unmasked): 0 clean, 1 corrected, -1 failed.
pub fn rs129_correct(word: &mut [u8; 12]) -> i32 {
    let (ex, lg) = (&GF.0, &GF.1);
    let mut s = [0u8; 3];
    for j in 1..=3usize {
        let mut acc = 0u8;
        for &w in word.iter() {
            acc = mul(acc, ex[j]) ^ w;
        }
        s[j - 1] = acc;
    }
    if s[0] == 0 && s[1] == 0 && s[2] == 0 {
        return 0;
    }
    if s[0] == 0 || s[1] == 0 || s[2] == 0 {
        return -1;
    }
    // one error of value e at power p: S1 = e a^p, S2 = e a^2p, S3 = e a^3p
    let p = (lg[s[1] as usize] as i32 - lg[s[0] as usize] as i32 + 255) % 255;
    if p > 11 {
        return -1;
    }
    let e = ex[((lg[s[0] as usize] as i32 - p + 255) % 255) as usize];
    if mul(e, ex[((3 * p) % 255) as usize]) != s[2] {
        return -1;
    }
    word[(11 - p) as usize] ^= e;
    1
}

/// CRC-CCITT of B.3.8 with the final inversion (still to be masked per data type).
pub fn crc_ccitt(d: &[u8]) -> u16 {
    let mut c: u16 = 0;
    for &x in d {
        c ^= u16::from(x) << 8;
        for _ in 0..8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x1021 } else { c << 1 };
        }
    }
    !c
}

/// CRC-8 of the short LC and hash (B.3.7), over 28 bits.
pub fn crc8_short(bits28: u32) -> u8 {
    let mut c: u32 = 0;
    for i in (0..=27).rev() {
        c = (c << 1) | ((bits28 >> i) & 1);
        if c & 0x100 != 0 {
            c ^= 0x107;
        }
    }
    for _ in 0..8 {
        c <<= 1;
        if c & 0x100 != 0 {
            c ^= 0x107;
        }
    }
    c as u8
}

/// CRC-9 of confirmed data blocks (B.3.10) before the data type mask: the data octets, then the 7 bit serial number.
pub fn crc9(d: &[u8], dbsn: u32) -> u16 {
    let mut c: u32 = 0;
    let mut shift = |bit: u32| {
        let top = (c >> 8) & 1;
        c = ((c << 1) | bit) & 0x1FF;
        if top != 0 {
            c ^= 0x059; // G9 = x^9 + x^6 + x^4 + x^3 + 1 (B.23)
        }
    };
    for &x in d {
        for b in (0..=7).rev() {
            shift(u32::from(x >> b) & 1);
        }
    }
    for b in (0..=6).rev() {
        shift((dbsn >> b) & 1);
    }
    for _ in 0..9 {
        shift(0);
    }
    (c ^ 0x1FF) as u16
}

/// Message CRC-32 of B.3.9 (octets of each 16-bit word least significant first, initial value 0, no inversion).
pub fn crc32_msg(d: &[u8]) -> u32 {
    let mut c: u32 = 0;
    let mut feed = |b: u8| {
        c ^= u32::from(b) << 24;
        for _ in 0..8 {
            c = if c & 0x8000_0000 != 0 { (c << 1) ^ 0x04C1_1DB7 } else { c << 1 };
        }
    };
    let n = d.len();
    let mut i = 0;
    while i + 1 < n {
        feed(d[i + 1]);
        feed(d[i]);
        i += 2;
    }
    if n & 1 != 0 {
        feed(d[n - 1]);
    }
    c
}

/// Embedded LC checksum (B.3.11): the sum of the nine octets modulo 31.
pub fn checksum5(lc: &[u8; 9]) -> u8 {
    let s: u32 = lc.iter().map(|&x| u32::from(x)).sum();
    (s % 31) as u8
}
