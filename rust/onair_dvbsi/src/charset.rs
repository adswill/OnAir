// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Broadcast character sets to UTF-8 (DVB: ETSI EN 300 468 Annex A; DAB: ETSI TS 101 756 Annex C and ETSI EN 300 401 section 5.2.2.2): EBU Latin, ISO/IEC 6937,
//! UCS-2 (big endian, with surrogate pairs), the DAB charset selector, `clean` (valid UTF-8 with no control or invisible characters, spaces folded) and the DVB
//! text field with its character table prefix. Output goes into a caller buffer (`Out`: bounded, overflow is reported, never panics). Conversions
//! that need iconv tables (ISO 8859-x, GB 2312, Big5, EUC-KR, iconv's UCS-2) are done by a callback the C++ side registers; without one the bytes are kept.
use crate::tables::{EBU_LATIN, ISO6937_COMB, ISO6937_HI};

/// A bounded byte writer over a caller buffer: once something does not fit the text is cut at the buffer end and `overflow` is set.
pub struct Out<'a> {
    buf: &'a mut [u8],
    len: usize,
    /// True when the buffer was too small for everything pushed so far.
    pub overflow: bool,
}

impl<'a> Out<'a> {
    /// An empty writer over `buf`.
    pub fn new(buf: &'a mut [u8]) -> Self {
        Out { buf, len: 0, overflow: false }
    }
    /// Bytes written.
    pub fn len(&self) -> usize {
        self.len
    }
    /// True when nothing was written.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Append bytes (as many as fit).
    pub fn bytes(&mut self, s: &[u8]) {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s[..n]);
        self.len += n;
        if n < s.len() {
            self.overflow = true;
        }
    }
    /// Append one byte.
    pub fn byte(&mut self, b: u8) {
        self.bytes(&[b]);
    }
    /// The unused tail of the buffer (for a conversion routine that writes there; call `advance` with what it wrote).
    pub fn tail(&mut self) -> &mut [u8] {
        &mut self.buf[self.len..]
    }
    /// Count `n` bytes written into the tail.
    pub fn advance(&mut self, n: usize) {
        self.len = (self.len + n).min(self.buf.len());
    }
    /// The whole buffer.
    pub fn into_inner(self) -> &'a mut [u8] {
        self.buf
    }
}

/// A conversion routine `(charset name, source bytes, output) -> bytes written` (the C++ side's iconv wrapper); `None` result = keep the raw bytes.
pub type Convert<'a> = &'a dyn Fn(&[u8], &[u8], &mut [u8]) -> Option<usize>;

/// Append one code point as UTF-8 (1 to 4 bytes, no validity check, like the reference).
pub fn append_utf8(o: &mut Out, c: u32) {
    if c < 0x80 {
        o.byte(c as u8);
    } else if c < 0x800 {
        o.bytes(&[(0xC0 | (c >> 6)) as u8, (0x80 | (c & 0x3F)) as u8]);
    } else if c < 0x10000 {
        o.bytes(&[(0xE0 | (c >> 12)) as u8, (0x80 | ((c >> 6) & 0x3F)) as u8, (0x80 | (c & 0x3F)) as u8]);
    } else {
        o.bytes(&[
            (0xF0 | (c >> 18)) as u8,
            (0x80 | ((c >> 12) & 0x3F)) as u8,
            (0x80 | ((c >> 6) & 0x3F)) as u8,
            (0x80 | (c & 0x3F)) as u8,
        ]);
    }
}

/// DAB labels and dynamic labels in the complete EBU Latin repertoire (ETSI TS 101 756 Annex C, charset 0).
pub fn ebu_latin(p: &[u8], o: &mut Out) {
    for &b in p {
        append_utf8(o, EBU_LATIN[b as usize] as u32);
    }
}

/// UCS-2 big endian; a high surrogate followed by a low one is combined (UTF-16 sent as "UCS-2"). An odd last byte is dropped.
pub fn ucs2be(p: &[u8], o: &mut Out) {
    let n = p.len();
    let mut i = 0;
    while i + 1 < n {
        let mut c = ((p[i] as u32) << 8) | p[i + 1] as u32;
        if (0xD800..=0xDBFF).contains(&c) && i + 3 < n {
            let d = ((p[i + 2] as u32) << 8) | p[i + 3] as u32;
            if (0xDC00..=0xDFFF).contains(&d) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
        }
        append_utf8(o, c);
        i += 2;
    }
}

/// DAB charset field (EN 300 401 FIG 1 / dynamic label): 0 EBU Latin, 4 ISO 8859-1, 6 UCS-2 (big endian), 15 UTF-8 (copied, `clean` drops invalid bytes);
/// anything else as EBU Latin.
pub fn dab_charset(cs: i32, p: &[u8], o: &mut Out) {
    match cs {
        4 => {
            for &b in p {
                append_utf8(o, b as u32);
            }
        }
        6 => ucs2be(p, o),
        15 => o.bytes(p),
        _ => ebu_latin(p, o),
    }
}

/// DVB SI default table (EN 300 468 Annex A, table 00): ISO/IEC 6937, where 0xC1..0xCF are accents put before the letter they mark.
pub fn iso6937(p: &[u8], o: &mut Out) {
    let n = p.len();
    let mut i = 0;
    while i < n {
        let c = p[i];
        i += 1;
        if c < 0xA0 {
            append_utf8(o, c as u32);
            continue;
        }
        if (0xC1..=0xCF).contains(&c) {
            if i < n && (0x20..0x80).contains(&p[i]) {
                let u = ISO6937_COMB[(c - 0xC1) as usize][(p[i] - 0x20) as usize];
                append_utf8(o, if u != 0 { u as u32 } else { p[i] as u32 });
                i += 1;
            }
            continue;
        }
        let u = ISO6937_HI[(c - 0xA0) as usize];
        if u != 0 {
            append_utf8(o, u as u32);
        }
    }
}

/// Characters `clean` removes: C0, DEL, C1, soft hyphen, zero-width and direction marks, BOM, noncharacters, surrogates, specials, beyond U+10FFFF.
fn invisible(c: u32) -> bool {
    c < 0x20
        || (0x7F..=0x9F).contains(&c)
        || c == 0xAD
        || (0x200B..=0x200F).contains(&c)
        || (0x2028..=0x202E).contains(&c)
        || (0x2060..=0x206F).contains(&c)
        || c == 0xFEFF
        || c == 0xFFFD
        || c == 0xFFFE
        || c == 0xFFFF
        || (0xD800..=0xDFFF).contains(&c)
        || (0xFFF9..=0xFFFB).contains(&c)
        || c > 0x10FFFF
}

/// Encoded length of a code point (the reference's `appendUtf8`).
fn utf8_len(c: u32) -> usize {
    if c < 0x80 {
        1
    } else if c < 0x800 {
        2
    } else if c < 0x10000 {
        3
    } else {
        4
    }
}

/// Write a code point at `b[w..]` (room is guaranteed by the caller's invariant that output never outruns input).
fn put_utf8(b: &mut [u8], w: usize, c: u32) -> usize {
    let mut t = Out::new(&mut b[w..]);
    append_utf8(&mut t, c);
    t.len()
}

/// Valid UTF-8 with no control or invisible characters (tabs, line breaks, NBSP, ideographic space become spaces), runs of spaces folded to one, trimmed.
/// Works in place on `b[..n]` and returns the new length (never longer than `n`).
pub fn clean_in_place(b: &mut [u8], n: usize) -> usize {
    let n = n.min(b.len());
    let (mut w, mut i, mut space) = (0usize, 0usize, false);
    while i < n {
        let b0 = b[i];
        let len = if b0 < 0x80 {
            1
        } else if (b0 >> 5) == 6 {
            2
        } else if (b0 >> 4) == 14 {
            3
        } else if (b0 >> 3) == 30 {
            4
        } else {
            0
        };
        if len == 0 || i + len > n {
            i += 1;
            continue;
        }
        let mut c: u32 = match len {
            1 => b0 as u32,
            2 => (b0 & 0x1F) as u32,
            3 => (b0 & 0x0F) as u32,
            _ => (b0 & 0x07) as u32,
        };
        let mut ok = true;
        for k in 1..len {
            let t = b[i + k];
            if (t & 0xC0) != 0x80 {
                ok = false;
                break;
            }
            c = (c << 6) | (t & 0x3F) as u32;
        }
        const MIN_FOR: [u32; 5] = [0, 0, 0x80, 0x800, 0x10000];
        if !ok || c < MIN_FOR[len] {
            i += 1;
            continue;
        }
        i += len;
        let c = if matches!(c, 9 | 10 | 13 | 0x8A | 0x2028 | 0x2029 | 0xA0 | 0x3000) { b' ' as u32 } else { c };
        if c == b' ' as u32 {
            space = w > 0;
            continue;
        }
        if invisible(c) {
            continue;
        }
        if space {
            b[w] = b' ';
            w += 1;
            space = false;
        }
        debug_assert!(w + utf8_len(c) <= i);
        w += put_utf8(b, w, c);
    }
    w
}

/// `clean` of `src` into `out` (which must hold `src.len()` bytes, else `usize::MAX`); returns the length.
pub fn clean(src: &[u8], out: &mut [u8]) -> usize {
    if out.len() < src.len() {
        return usize::MAX;
    }
    out[..src.len()].copy_from_slice(src);
    clean_in_place(out, src.len())
}

/// Does unprefixed text look like a run of valid GB 2312 (EUC-CN) pairs, as some Chinese DTMB muxes send?
fn looks_gb2312(p: &[u8]) -> bool {
    let n = p.len();
    if n < 2 || p[0] < 0x81 {
        return false;
    }
    let mut pairs = 0;
    let mut i = 0;
    while i < n {
        if p[i] < 0x80 {
            i += 1;
            continue;
        }
        // GBK (a superset of GB 2312, what Chinese DTMB names really use): lead 81..FE, trail 40..FE without 7F
        if i + 1 >= n || p[i] > 0xFE || p[i + 1] < 0x40 || p[i + 1] == 0x7F || p[i + 1] > 0xFE {
            return false;
        }
        i += 2;
        pairs += 1;
    }
    pairs > 0
}

/// DVB text local clean-up of the reference `dvbText`: drops UTF-8 C1 controls (C2 80..9F) and bytes below 0x20, line feed and carriage return become
/// spaces. In place.
fn local_clean(b: &mut [u8], n: usize) -> usize {
    let mut w = 0;
    let mut i = 0;
    while i < n {
        let c = b[i];
        if c == 0xC2 && i + 1 < n && b[i + 1] >= 0x80 && b[i + 1] < 0xA0 {
            i += 2;
            continue;
        }
        i += 1;
        if c == 0x0A || c == 0x0D {
            b[w] = b' ';
            w += 1;
            continue;
        }
        if c >= 0x20 {
            b[w] = c;
            w += 1;
        }
    }
    w
}

/// Run the conversion callback (or keep the bytes) into `o`.
fn convert(conv: Option<Convert>, name: &[u8], p: &[u8], o: &mut Out) {
    if let Some(f) = conv {
        if let Some(w) = f(name, p, o.tail()) {
            o.advance(w);
            return;
        }
    }
    o.bytes(p);
}

/// A DVB text field (EN 300 468 Annex A) to clean UTF-8 in `out`; returns the length (`usize::MAX` when `out` was too small for the converted text; use
/// `4 * p.len() + 16` bytes). `gb_default`: unprefixed GB 2312 pairs are decoded as GB 2312 (DTMB).
pub fn dvb_text(p: &[u8], gb_default: bool, conv: Option<Convert>, out: &mut [u8]) -> usize {
    let n = p.len();
    if n == 0 {
        return 0;
    }
    let mut o = Out::new(out);
    let first = p[0];
    const CS: [&[u8]; 12] = [
        b"",
        b"ISO-8859-5",
        b"ISO-8859-6",
        b"ISO-8859-7",
        b"ISO-8859-8",
        b"ISO-8859-9",
        b"ISO-8859-10",
        b"ISO-8859-11",
        b"",
        b"ISO-8859-13",
        b"ISO-8859-14",
        b"ISO-8859-15",
    ];
    if gb_default && looks_gb2312(p) {
        convert(conv, b"GB2312", p, &mut o);
    } else if first >= 0x20 {
        iso6937(p, &mut o);
    } else if (1..=0x0B).contains(&first) && n > 1 {
        convert(conv, CS[first as usize], &p[1..], &mut o);
    } else if first == 0x10 && n > 3 {
        let mut nm = [0u8; 24];
        nm[..9].copy_from_slice(b"ISO-8859-");
        let mut l = 9;
        let v = ((p[1] as u32) << 8) | p[2] as u32;
        let mut digits = [0u8; 5];
        let mut d = 0;
        let mut x = v;
        loop {
            digits[d] = b'0' + (x % 10) as u8;
            d += 1;
            x /= 10;
            if x == 0 {
                break;
            }
        }
        while d > 0 {
            d -= 1;
            nm[l] = digits[d];
            l += 1;
        }
        convert(conv, &nm[..l], &p[3..], &mut o);
    } else if first == 0x11 && n > 1 {
        convert(conv, b"UCS-2BE", &p[1..], &mut o);
    } else if first == 0x15 && n > 1 {
        o.bytes(&p[1..]);
    } else if first == 0x14 && n > 1 {
        convert(conv, b"BIG5", &p[1..], &mut o);
    } else if first == 0x13 && n > 1 {
        convert(conv, b"GB2312", &p[1..], &mut o);
    } else if first == 0x12 && n > 1 {
        convert(conv, b"EUC-KR", &p[1..], &mut o);
    } else {
        iso6937(&p[1..], &mut o);
    }
    if o.overflow {
        return usize::MAX;
    }
    let len = o.len();
    let b = o.into_inner();
    let l1 = local_clean(b, len);
    clean_in_place(b, l1)
}
