// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Bit access. Two layouts: packed (8 bits per byte, `get_bits_*`/`put_bits_*`, `BitReader`/`BitWriter`) and unpacked
//! (one bit per byte, only bit 0 counts, `unpacked`). Reads past the end give 0; writes past the end fail instead of panicking.

/// `n` (0..=64) bits starting at bit `pos`, most significant bit of each byte first; the first bit read is the most significant of the result.
pub fn get_bits_msb(buf: &[u8], pos: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..n.min(64) {
        let p = pos.wrapping_add(i);
        let bit = buf.get(p >> 3).map_or(0, |b| (b >> (7 - (p & 7))) & 1);
        v = (v << 1) | bit as u64;
    }
    v
}

/// As `get_bits_msb` but bits are numbered from the least significant bit of each byte and the first bit read is the least significant of the result.
pub fn get_bits_lsb(buf: &[u8], pos: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..n.min(64) {
        let p = pos.wrapping_add(i);
        let bit = buf.get(p >> 3).map_or(0, |b| (b >> (p & 7)) & 1);
        v |= (bit as u64) << i;
    }
    v
}

/// Writes the low `n` bits of `val` (MSB first) at bit `pos`. Returns false, writing nothing, when it does not fit.
pub fn put_bits_msb(buf: &mut [u8], pos: usize, n: usize, val: u64) -> bool {
    if n > 64 || pos.checked_add(n).map_or(true, |e| e > buf.len() * 8) {
        return false;
    }
    for i in 0..n {
        let p = pos + i;
        let bit = ((val >> (n - 1 - i)) & 1) as u8;
        let m = 1u8 << (7 - (p & 7));
        buf[p >> 3] = (buf[p >> 3] & !m) | if bit != 0 { m } else { 0 };
    }
    true
}

/// Writes the low `n` bits of `val` (first written bit = least significant of `val`) at bit `pos`, LSB-first numbering.
pub fn put_bits_lsb(buf: &mut [u8], pos: usize, n: usize, val: u64) -> bool {
    if n > 64 || pos.checked_add(n).map_or(true, |e| e > buf.len() * 8) {
        return false;
    }
    for i in 0..n {
        let p = pos + i;
        let m = 1u8 << (p & 7);
        buf[p >> 3] = (buf[p >> 3] & !m) | if (val >> i) & 1 != 0 { m } else { 0 };
    }
    true
}

/// Sequential reader over a packed buffer.
pub struct BitReader<'a> {
    buf: &'a [u8],
    pos: usize,
    msb: bool,
}

impl<'a> BitReader<'a> {
    pub fn new_msb(buf: &'a [u8]) -> Self {
        BitReader { buf, pos: 0, msb: true }
    }
    pub fn new_lsb(buf: &'a [u8]) -> Self {
        BitReader { buf, pos: 0, msb: false }
    }
    /// Reads `n` (0..=64) bits; past the end the missing bits read as 0 (check `remaining` when that matters).
    pub fn read(&mut self, n: usize) -> u64 {
        let v = if self.msb { get_bits_msb(self.buf, self.pos, n) } else { get_bits_lsb(self.buf, self.pos, n) };
        self.pos = self.pos.saturating_add(n);
        v
    }
    pub fn position(&self) -> usize {
        self.pos
    }
    pub fn remaining(&self) -> usize {
        (self.buf.len() * 8).saturating_sub(self.pos)
    }
}

/// Sequential writer into a caller-owned packed buffer.
pub struct BitWriter<'a> {
    buf: &'a mut [u8],
    pos: usize,
    msb: bool,
}

impl<'a> BitWriter<'a> {
    pub fn new_msb(buf: &'a mut [u8]) -> Self {
        BitWriter { buf, pos: 0, msb: true }
    }
    pub fn new_lsb(buf: &'a mut [u8]) -> Self {
        BitWriter { buf, pos: 0, msb: false }
    }
    /// Appends the low `n` bits of `val`; false when the buffer is full (nothing written).
    pub fn write(&mut self, n: usize, val: u64) -> bool {
        let ok = if self.msb { put_bits_msb(self.buf, self.pos, n, val) } else { put_bits_lsb(self.buf, self.pos, n, val) };
        if ok {
            self.pos += n;
        }
        ok
    }
    /// Bits written so far.
    pub fn position(&self) -> usize {
        self.pos
    }
}

/// One bit per byte, transmission order, only bit 0 of each byte counts.
pub mod unpacked {
    /// `len` bits from `pos`, most significant first; bits past the end read as 0. Fields wider than 32 bits keep the last 32.
    pub fn get_u(b: &[u8], pos: usize, len: usize) -> u32 {
        let mut v: u32 = 0;
        for i in 0..len {
            let bit = match pos.checked_add(i).and_then(|p| b.get(p)) {
                Some(x) => (*x & 1) as u32,
                None => 0,
            };
            v = (v << 1) | bit;
        }
        v
    }

    /// Two's complement field.
    pub fn get_s(b: &[u8], pos: usize, len: usize) -> i32 {
        let mut v = get_u(b, pos, len);
        if len > 0 && len < 32 && (v >> (len - 1)) & 1 != 0 {
            v |= !0u32 << len;
        }
        v as i32
    }

    /// Unpacks `n` bits of a packed MSB-first buffer into `out` (one per byte). Returns the number written (limited by both buffers).
    pub fn unpack_msb(packed: &[u8], n: usize, out: &mut [u8]) -> usize {
        let n = n.min(packed.len() * 8).min(out.len());
        for (i, o) in out.iter_mut().take(n).enumerate() {
            *o = (packed[i >> 3] >> (7 - (i & 7))) & 1;
        }
        n
    }

    /// Packs unpacked bits MSB first; the last byte is zero padded. Returns the bytes written.
    pub fn pack_msb(bits: &[u8], out: &mut [u8]) -> usize {
        let nbytes = ((bits.len() + 7) / 8).min(out.len());
        for o in out.iter_mut().take(nbytes) {
            *o = 0;
        }
        for (i, &b) in bits.iter().enumerate().take(nbytes * 8) {
            out[i >> 3] |= (b & 1) << (7 - (i & 7));
        }
        nbytes
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn msb_lsb_reads() {
        let b = [0b1010_0101, 0xF0];
        assert_eq!(get_bits_msb(&b, 0, 4), 0b1010);
        assert_eq!(get_bits_msb(&b, 4, 8), 0b0101_1111);
        assert_eq!(get_bits_lsb(&b, 0, 4), 0b0101);
        assert_eq!(get_bits_lsb(&b, 0, 16), 0xF0A5);
        assert_eq!(get_bits_msb(&b, 12, 8), 0, "past the end reads 0");
    }

    #[test]
    fn write_read_round_trip() {
        for msb in [true, false] {
            let mut buf = [0u8; 4];
            {
                let mut w = if msb { BitWriter::new_msb(&mut buf) } else { BitWriter::new_lsb(&mut buf) };
                assert!(w.write(5, 0x15));
                assert!(w.write(11, 0x5A5));
                assert!(w.write(16, 0xBEEF));
                assert!(!w.write(1, 1));
                assert_eq!(w.position(), 32);
            }
            let mut r = if msb { BitReader::new_msb(&buf) } else { BitReader::new_lsb(&buf) };
            assert_eq!(r.read(5), 0x15);
            assert_eq!(r.read(11), 0x5A5);
            assert_eq!(r.read(16), 0xBEEF);
            assert_eq!(r.remaining(), 0);
        }
    }

    #[test]
    fn wide_fields_and_overflow() {
        let mut buf = [0u8; 8];
        assert!(put_bits_msb(&mut buf, 0, 64, 0x0123_4567_89AB_CDEF));
        assert_eq!(buf, [0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF]);
        assert_eq!(get_bits_msb(&buf, 0, 64), 0x0123_4567_89AB_CDEF);
        assert!(!put_bits_msb(&mut buf, usize::MAX, 2, 0));
        assert!(!put_bits_lsb(&mut buf, 60, 8, 0));
    }

    #[test]
    fn unpacked_fields() {
        let b = [1, 0, 1, 1, 0, 0, 0, 1];
        assert_eq!(unpacked::get_u(&b, 0, 4), 0b1011);
        assert_eq!(unpacked::get_s(&b, 0, 4), -5);
        let mut packed = [0u8; 1];
        assert_eq!(unpacked::pack_msb(&b, &mut packed), 1);
        assert_eq!(packed[0], 0b1011_0001);
        let mut back = [0u8; 8];
        assert_eq!(unpacked::unpack_msb(&packed, 8, &mut back), 8);
        assert_eq!(back, b);
    }
}
