// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! SHA-1 (RFC 3174) and base64 (RFC 4648), only for the WebSocket handshake (`Sec-WebSocket-Accept`, RFC 6455 section 4.2.2): the key is not a secret, so neither
//! algorithm needs to be constant time or collision resistant.

/// An incremental SHA-1.
pub struct Sha1 {
    h: [u32; 5],
    buf: [u8; 64],
    fill: usize,
    len: u64,
}

impl Default for Sha1 {
    fn default() -> Sha1 {
        Sha1::new()
    }
}

impl Sha1 {
    /// A fresh hash.
    pub fn new() -> Sha1 {
        Sha1 { h: [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476, 0xC3D2_E1F0], buf: [0; 64], fill: 0, len: 0 }
    }

    /// Adds bytes.
    pub fn update(&mut self, mut d: &[u8]) {
        self.len = self.len.wrapping_add(d.len() as u64);
        while !d.is_empty() {
            let take = (64 - self.fill).min(d.len());
            self.buf[self.fill..self.fill + take].copy_from_slice(&d[..take]);
            self.fill += take;
            d = &d[take..];
            if self.fill == 64 {
                let b = self.buf;
                compress(&mut self.h, &b);
                self.fill = 0;
            }
        }
    }

    /// Pads and returns the 20 byte digest.
    pub fn finish(mut self) -> [u8; 20] {
        let bits = self.len.wrapping_mul(8);
        let mut pad = [0u8; 72];
        pad[0] = 0x80;
        // zeros up to 56 mod 64, then the 64 bit length
        let zeros = if self.fill < 56 { 56 - self.fill } else { 120 - self.fill };
        let total = zeros + 8;
        pad[zeros..total].copy_from_slice(&bits.to_be_bytes());
        let len_keep = self.len;
        self.update(&pad[..total]);
        self.len = len_keep;
        let mut out = [0u8; 20];
        for (i, v) in self.h.iter().enumerate() {
            out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
        }
        out
    }
}

/// SHA-1 of `data` (20 bytes).
pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut s = Sha1::new();
    s.update(data);
    s.finish()
}

fn compress(h: &mut [u32; 5], block: &[u8; 64]) {
    let mut w = [0u32; 80];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
    }
    for i in 16..80 {
        w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
    }
    let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
    for (i, wi) in w.iter().enumerate() {
        let (f, k) = if i < 20 {
            ((b & c) | (!b & d), 0x5A82_7999u32)
        } else if i < 40 {
            (b ^ c ^ d, 0x6ED9_EBA1)
        } else if i < 60 {
            ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC)
        } else {
            (b ^ c ^ d, 0xCA62_C1D6)
        };
        let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
        e = d;
        d = c;
        c = b.rotate_left(30);
        b = a;
        a = t;
    }
    h[0] = h[0].wrapping_add(a);
    h[1] = h[1].wrapping_add(b);
    h[2] = h[2].wrapping_add(c);
    h[3] = h[3].wrapping_add(d);
    h[4] = h[4].wrapping_add(e);
}

/// Length of the base64 text of `n` bytes (with `=` padding).
pub const fn base64_len(n: usize) -> usize {
    (n + 2) / 3 * 4
}

/// Base64 of `d` into `out` (at least `base64_len(d.len())` bytes); returns the length, or `usize::MAX` when `out` is too short.
pub fn base64(d: &[u8], out: &mut [u8]) -> usize {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let n = base64_len(d.len());
    if out.len() < n {
        return usize::MAX;
    }
    let mut o = 0;
    let mut i = 0;
    while i + 2 < d.len() {
        let v = (d[i] as u32) << 16 | (d[i + 1] as u32) << 8 | d[i + 2] as u32;
        out[o] = T[(v >> 18) as usize];
        out[o + 1] = T[((v >> 12) & 63) as usize];
        out[o + 2] = T[((v >> 6) & 63) as usize];
        out[o + 3] = T[(v & 63) as usize];
        o += 4;
        i += 3;
    }
    if i + 1 == d.len() {
        let v = (d[i] as u32) << 16;
        out[o] = T[(v >> 18) as usize];
        out[o + 1] = T[((v >> 12) & 63) as usize];
        out[o + 2] = b'=';
        out[o + 3] = b'=';
        o += 4;
    } else if i + 2 == d.len() {
        let v = (d[i] as u32) << 16 | (d[i + 1] as u32) << 8;
        out[o] = T[(v >> 18) as usize];
        out[o + 1] = T[((v >> 12) & 63) as usize];
        out[o + 2] = T[((v >> 6) & 63) as usize];
        out[o + 3] = b'=';
        o += 4;
    }
    o
}

/// The `Sec-WebSocket-Accept` value for a client key: base64(sha1(key + GUID)), 28 characters.
pub fn ws_accept_key(key: &[u8], out: &mut [u8; 28]) {
    const GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    let mut s = Sha1::new();
    s.update(key);
    s.update(GUID);
    base64(&s.finish(), out);
}
