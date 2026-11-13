// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR text: short data and talker alias text conversion (ETSI TS 102 361-1 table 9.50, TS 102 361-2 7.1.1.4 and 7.1.1.5): the defined data formats
//! (binary, BCD, 7 bit, ISO 8859-x, UTF-8 / 16 / 32), clean-up of control characters, format names, and the talker alias assembled from the
//! embedded link control of a voice call. Text goes into a caller buffer through [`Buf`].
use crate::bits::{get_bits, Out};
use crate::lc::{FullLc, FLCO_TA_BLOCK1, FLCO_TA_BLOCK3, FLCO_TA_HEADER};

/// Data format numbers of table 9.50 (V2.6.1).
pub const DD_BINARY: i32 = 0;
/// BCD.
pub const DD_BCD: i32 = 1;
/// 7 bit characters.
pub const DD_7BIT: i32 = 2;
/// ISO 8859-1.
pub const DD_ISO8859_1: i32 = 3;
/// ISO 8859-16 (the last of the 8 bit sets).
pub const DD_ISO8859_16: i32 = 17;
/// UTF-8.
pub const DD_UTF8: i32 = 18;
/// UTF-16 (byte order mark decides).
pub const DD_UTF16: i32 = 19;
/// UTF-16BE.
pub const DD_UTF16_BE: i32 = 20;
/// UTF-16LE.
pub const DD_UTF16_LE: i32 = 21;
/// UTF-32.
pub const DD_UTF32: i32 = 22;
/// UTF-32BE.
pub const DD_UTF32_BE: i32 = 23;
/// UTF-32LE.
pub const DD_UTF32_LE: i32 = 24;

/// Byte writer over a caller buffer; `over` is set when something did not fit.
pub struct Buf<'a> {
    b: &'a mut [u8],
    /// Bytes written.
    pub n: usize,
    /// True once a byte was dropped for lack of room.
    pub over: bool,
}

impl<'a> Buf<'a> {
    /// An empty writer over `b`.
    pub fn new(b: &'a mut [u8]) -> Self {
        Buf { b, n: 0, over: false }
    }
    /// Appends one byte.
    pub fn push(&mut self, c: u8) {
        if self.n < self.b.len() {
            self.b[self.n] = c;
            self.n += 1;
        } else {
            self.over = true;
        }
    }
    /// Appends bytes.
    pub fn push_all(&mut self, s: &[u8]) {
        for &c in s {
            self.push(c);
        }
    }
    /// Removes one trailing space, if there is one.
    pub fn pop_space(&mut self) {
        if self.n > 0 && self.b[self.n - 1] == b' ' {
            self.n -= 1;
        }
    }
    /// Removes trailing spaces.
    pub fn trim_spaces(&mut self) {
        while self.n > 0 && self.b[self.n - 1] == b' ' {
            self.n -= 1;
        }
    }
    /// The length written, or `usize::MAX` when it did not fit.
    pub fn result(&self) -> usize {
        if self.over {
            usize::MAX
        } else {
            self.n
        }
    }
    /// Appends code point `cp` as UTF-8 ('?' above U+10FFFF).
    pub fn push_utf8(&mut self, cp: u32) {
        if cp < 0x80 {
            self.push(cp as u8);
        } else if cp < 0x800 {
            self.push((0xC0 | (cp >> 6)) as u8);
            self.push((0x80 | (cp & 63)) as u8);
        } else if cp < 0x10000 {
            self.push((0xE0 | (cp >> 12)) as u8);
            self.push((0x80 | ((cp >> 6) & 63)) as u8);
            self.push((0x80 | (cp & 63)) as u8);
        } else if cp < 0x110000 {
            self.push((0xF0 | (cp >> 18)) as u8);
            self.push((0x80 | ((cp >> 12) & 63)) as u8);
            self.push((0x80 | ((cp >> 6) & 63)) as u8);
            self.push((0x80 | (cp & 63)) as u8);
        } else {
            self.push(b'?');
        }
    }
    /// Appends `c` the way the text clean-up does: CR, LF and TAB become a space, other control characters and DEL are dropped.
    pub fn push_clean(&mut self, c: u8) {
        if c == b'\r' || c == b'\n' || c == b'\t' {
            self.push(b' ');
        } else if c >= 0x20 && c != 0x7F {
            self.push(c);
        }
    }
}

/// Name of data format `dd`, NUL-terminated.
pub fn dd_name(dd: i32) -> &'static str {
    match dd {
        DD_BINARY => "binary\0",
        DD_BCD => "BCD\0",
        DD_7BIT => "7 bit\0",
        DD_ISO8859_1 => "ISO 8859-1\0",
        DD_UTF8 => "UTF-8\0",
        DD_UTF16 => "UTF-16\0",
        DD_UTF16_BE => "UTF-16BE\0",
        DD_UTF16_LE => "UTF-16LE\0",
        DD_UTF32 => "UTF-32\0",
        DD_UTF32_BE => "UTF-32BE\0",
        DD_UTF32_LE => "UTF-32LE\0",
        _ => {
            if dd > DD_ISO8859_1 && dd <= DD_ISO8859_16 {
                "ISO 8859\0"
            } else {
                "reserved\0"
            }
        }
    }
}

/// Message text of short data or a talker alias. `format` is the data format of table 9.50, or with `ta` the talker alias two bit code
/// (0 7 bit, 1 ISO 8859-1, 2 UTF-8, 3 UTF-16BE). The result is UTF-8 with control characters removed; binary data that is not printable ASCII gives "[hex dump]".
/// The text is at most 2n + 80 bytes.
pub fn text_from_bytes(p: &[u8], format: i32, ta: bool, out: &mut Buf) {
    let n = p.len();
    let mut f = format;
    if ta {
        f = match format {
            0 => DD_7BIT,
            1 => DD_ISO8859_1,
            2 => DD_UTF8,
            _ => DD_UTF16_BE,
        };
    }
    if f == DD_BINARY {
        let printable = n > 0 && p.iter().all(|&c| (0x20..=0x7E).contains(&c));
        if printable {
            out.push_all(p);
            return;
        }
        // hex dump of the first 24 bytes in brackets
        let mut s = [0u8; 80];
        let mut k = 0;
        for &c in p.iter().take(24) {
            const H: &[u8; 16] = b"0123456789ABCDEF";
            s[k] = H[(c >> 4) as usize];
            s[k + 1] = H[(c & 15) as usize];
            s[k + 2] = b' ';
            k += 3;
        }
        if n > 24 {
            s[k..k + 3].copy_from_slice(b"...");
            k += 3;
        }
        if k > 0 && s[k - 1] == b' ' {
            k -= 1;
        }
        out.push(b'[');
        out.push_all(&s[..k]);
        out.push(b']');
        return;
    }
    if f == DD_BCD {
        for &c in p {
            out.push(b'0' + ((c >> 4) & 15) % 10);
            out.push(b'0' + (c & 15) % 10);
        }
        return;
    }
    if f == DD_7BIT {
        // 7 bit characters packed one after the other (the packing is not given in TS 102 361-1: the talker alias rule is used)
        let bits = n * 8;
        let mut i = 0;
        while i + 7 <= bits {
            let mut v = 0u32;
            for k in 0..7 {
                let q = i + k;
                v = (v << 1) | u32::from((p[q / 8] >> (7 - q % 8)) & 1);
            }
            out.push_clean(v as u8);
            i += 7;
        }
        out.trim_spaces();
        return;
    }
    if (DD_ISO8859_1..=DD_ISO8859_16).contains(&f) {
        for &c in p {
            if c < 0x80 {
                out.push_clean(c);
            } else if f == DD_ISO8859_1 {
                // a Latin-1 character as UTF-8, then cleaned: the two bytes are both >= 0x80, so they pass
                out.push_utf8(u32::from(c));
            } else {
                out.push(b'?');
            }
        }
        out.trim_spaces();
        return;
    }
    if f == DD_UTF8 {
        let mut i = 0;
        while i < n {
            let c = p[i];
            if c < 0x80 {
                out.push_clean(c);
                i += 1;
                continue;
            }
            let len = if c >= 0xF0 {
                4
            } else if c >= 0xE0 {
                3
            } else if c >= 0xC0 {
                2
            } else {
                0
            };
            let mut ok = len > 0 && i + len <= n;
            for k in 1..len {
                if ok && (p[i + k] & 0xC0) != 0x80 {
                    ok = false;
                }
            }
            if ok {
                out.push_all(&p[i..i + len]);
                i += len;
            } else {
                out.push(b'?');
                i += 1;
            }
        }
        out.trim_spaces();
        return;
    }
    if (DD_UTF16..=DD_UTF16_LE).contains(&f) {
        let mut le = f == DD_UTF16_LE;
        let mut i = 0;
        if f == DD_UTF16 && n >= 2 {
            if p[0] == 0xFF && p[1] == 0xFE {
                le = true;
                i = 2;
            } else if p[0] == 0xFE && p[1] == 0xFF {
                le = false;
                i = 2;
            }
        }
        // Import radios (Baofeng, TYT, ...) label UTF-16 text big-endian but send it little-endian (#300). Latin text shows it: every
        // unit has a zero high byte, so the zeros sit at the odd offsets instead of the even ones. Only the big-endian labels are switched.
        if !le && f != DD_UTF16_LE {
            let (mut be_z, mut le_z) = (0usize, 0usize);
            let mut k = i;
            while k + 1 < n {
                if p[k] == 0 && p[k + 1] != 0 {
                    be_z += 1;
                } else if p[k + 1] == 0 && p[k] != 0 {
                    le_z += 1;
                }
                k += 2;
            }
            if le_z > be_z {
                le = true;
            }
        }
        let unit = |i: usize| -> u32 {
            if le {
                u32::from(p[i]) | (u32::from(p[i + 1]) << 8)
            } else {
                (u32::from(p[i]) << 8) | u32::from(p[i + 1])
            }
        };
        while i + 1 < n {
            let mut cp = unit(i);
            if (0xD800..0xDC00).contains(&cp) && i + 3 < n {
                let lo = unit(i + 2);
                if (0xDC00..0xE000).contains(&lo) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            push_clean_cp(out, cp);
            i += 2;
        }
        out.trim_spaces();
        return;
    }
    if (DD_UTF32..=DD_UTF32_LE).contains(&f) {
        let le = f == DD_UTF32_LE;
        let mut i = 0;
        while i + 3 < n {
            let cp = if le {
                u32::from(p[i]) | (u32::from(p[i + 1]) << 8) | (u32::from(p[i + 2]) << 16) | (u32::from(p[i + 3]) << 24)
            } else {
                (u32::from(p[i]) << 24) | (u32::from(p[i + 1]) << 16) | (u32::from(p[i + 2]) << 8) | u32::from(p[i + 3])
            };
            push_clean_cp(out, cp);
            i += 4;
        }
        out.trim_spaces();
    }
}

/// A code point as UTF-8 run through the clean-up (the single-byte ones are filtered, longer sequences pass as their bytes).
fn push_clean_cp(out: &mut Buf, cp: u32) {
    if cp < 0x80 {
        out.push_clean(cp as u8);
    } else {
        out.push_utf8(cp);
    }
}

/// Number of PDUs a talker alias of `length` characters in `format` needs (header + blocks).
pub fn alias_pieces(format: i32, length: i32) -> i32 {
    let per_header = if format == 0 {
        7
    } else if format == 3 {
        3
    } else {
        6
    };
    let per_block = if format == 0 {
        8
    } else if format == 3 {
        3
    } else {
        7
    };
    // 16 bit characters: the header holds 3 characters, the blocks 3.5; the counts of the specification (3, 6, 10, 13) are used
    if format == 3 {
        return if length <= 3 {
            1
        } else if length <= 6 {
            2
        } else if length <= 10 {
            3
        } else {
            4
        };
    }
    if length <= per_header {
        return 1;
    }
    1 + 3.min((length - per_header + per_block - 1) / per_block)
}

/// A talker alias being assembled from the embedded LC of a voice call.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct TalkerAlias {
    /// Format code (0 7 bit, 1 ISO 8859-1, 2 UTF-8, 3 UTF-16BE).
    pub format: i32,
    /// Number of characters.
    pub length: i32,
    /// Bit 0 header, bits 1..3 blocks received.
    pub have: u8,
    /// The seven octets (2..8) of the header and of each block.
    pub data: [u8; 28],
}

impl TalkerAlias {
    /// Takes in a talker alias header or block LC; other opcodes are ignored. A header starts a new alias.
    pub fn add(&mut self, lc: &FullLc) {
        if lc.flco == FLCO_TA_HEADER {
            *self = TalkerAlias::default();
            self.format = i32::from(lc.raw[2] >> 6);
            self.length = i32::from((lc.raw[2] >> 1) & 31);
            self.data[..7].copy_from_slice(&lc.raw[2..9]);
            self.have = 1;
        } else if (FLCO_TA_BLOCK1..=FLCO_TA_BLOCK3).contains(&lc.flco) {
            let i = (lc.flco - FLCO_TA_BLOCK1 + 1) as usize;
            self.data[7 * i..7 * i + 7].copy_from_slice(&lc.raw[2..9]);
            self.have |= 1 << i;
        }
    }

    /// True when the header and all blocks the length needs have arrived.
    pub fn complete(&self) -> bool {
        if self.have & 1 == 0 {
            return false;
        }
        let n = alias_pieces(self.format, self.length);
        for i in 0..n {
            if self.have & (1 << i) == 0 {
                return false;
            }
        }
        self.length > 0
    }

    /// The alias text (nothing until [`complete`](Self::complete)).
    pub fn text(&self, out: &mut Buf) {
        if !self.complete() {
            return;
        }
        if self.format == 0 {
            // 7 bit: 49 bits in the header, then 56 per block
            let mut bits = [0u8; 217];
            let mut o = Out::new(&mut bits);
            o.put(u64::from(self.data[0] & 1), 1);
            for i in 1..7 {
                o.put(u64::from(self.data[i]), 8);
            }
            for b in 1..4 {
                for i in 0..7 {
                    o.put(u64::from(self.data[7 * b + i]), 8);
                }
            }
            let mut c = 0;
            while c < self.length && ((c * 7 + 7) as usize) <= bits.len() {
                out.push_clean(get_bits(&bits, (c * 7) as usize, 7) as u8);
                c += 1;
            }
            out.trim_spaces();
            return;
        }
        let mut bytes = [0u8; 28];
        let mut n = 0;
        for i in 1..7 {
            bytes[n] = self.data[i];
            n += 1;
        }
        for b in 1..4 {
            for i in 0..7 {
                bytes[n] = self.data[7 * b + i];
                n += 1;
            }
        }
        let len = if self.format == 3 { self.length as usize * 2 } else { self.length as usize };
        text_from_bytes(&bytes[..len.min(n)], self.format, true, out);
    }
}
