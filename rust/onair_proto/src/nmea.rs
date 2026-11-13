// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NMEA 0183 !AIVDM sentences: 6-bit armour, checksum, and the sentence parser.

use crate::bits::get_u;

fn armour_char(v: u32) -> u8 {
    if v < 40 {
        (v + 48) as u8
    } else {
        (v + 56) as u8
    }
}

/// 6-bit ASCII of `p` into `out` (needs `(p.len() + 5) / 6` bytes). Returns (chars written, fill bits).
pub fn armour(p: &[u8], out: &mut [u8]) -> Option<(usize, i32)> {
    let fill = ((6 - p.len() % 6) % 6) as i32;
    let mut n = 0;
    let mut i = 0;
    while i < p.len() {
        *out.get_mut(n)? = armour_char(get_u(p, i, 6));
        n += 1;
        i += 6;
    }
    Some((n, fill))
}

/// Bits of `chars` into `out` (needs `chars.len() * 6` bytes), less `fill` bits at the end when that fits. Returns the bit count.
pub fn unarmour(chars: &[u8], fill: i32, out: &mut [u8]) -> Option<usize> {
    let mut n = 0;
    for &c in chars {
        let mut v = (c as i8) as i32 - 48;
        if v > 40 {
            v -= 8;
        }
        if !(0..=63).contains(&v) {
            v = 0;
        }
        for k in 0..6 {
            *out.get_mut(n + k)? = ((v >> (5 - k)) & 1) as u8;
        }
        n += 6;
    }
    if fill > 0 && fill as usize <= n {
        n -= fill as usize;
    }
    Some(n)
}

pub fn checksum(body: &[u8]) -> u8 {
    body.iter().fold(0u8, |x, &c| x ^ c)
}

fn hex_val(c: u8) -> Option<u32> {
    match c {
        b'0'..=b'9' => Some((c - b'0') as u32),
        b'a'..=b'f' => Some((c - b'a' + 10) as u32),
        b'A'..=b'F' => Some((c - b'A' + 10) as u32),
        _ => None,
    }
}

/// C `atoi` on a field: leading white space, a sign, digits; overflow saturates like `strtol` on a 64-bit `long`, then truncates to `int`.
pub fn atoi(s: &[u8]) -> i32 {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r') {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        neg = s[i] == b'-';
        i += 1;
    }
    let mut v: i64 = 0;
    let mut sat = false;
    while i < s.len() && s[i].is_ascii_digit() {
        let d = (s[i] - b'0') as i64;
        match v.checked_mul(10).and_then(|x| x.checked_add(d)) {
            Some(x) if !sat => v = x,
            _ => sat = true,
        }
        i += 1;
    }
    let r = if sat {
        if neg {
            i64::MIN
        } else {
            i64::MAX
        }
    } else if neg {
        -v
    } else {
        v
    };
    r as i32
}

/// Result of `parse`: what was written and whether the sentence is good.
#[derive(Default, Clone, Copy)]
pub struct Parsed {
    /// 0 = rejected before any output was set; 1 = outputs set but the layout is wrong; 2 = good
    pub state: i32,
    pub chars_start: usize,
    pub chars_len: usize,
    pub fill: i32,
    pub frag_count: i32,
    pub frag_no: i32,
    pub channel: u8,
}

/// Splits `!AIVDM,count,number,seq,channel,chars,fill*CS`. The checksum is two hex digits.
pub fn parse(s: &[u8]) -> Parsed {
    let mut r = Parsed::default();
    if s.len() < 12 || (s[0] != b'!' && s[0] != b'$') {
        return r;
    }
    let star = match s.iter().position(|&c| c == b'*') {
        Some(p) => p,
        None => return r,
    };
    if star + 3 > s.len() {
        return r;
    }
    let want = match (hex_val(s[star + 1]), hex_val(s[star + 2])) {
        (Some(h), Some(l)) => h * 16 + l,
        _ => return r,
    };
    if checksum(&s[1..star]) as u32 != want {
        return r;
    }
    let body = &s[..star];
    let mut f = [(0usize, 0usize); 7];
    let mut cnt = 0;
    let mut p = 0;
    loop {
        let q = body[p..].iter().position(|&c| c == b',').map(|x| x + p);
        if cnt >= 7 {
            return r;
        }
        f[cnt] = (p, q.unwrap_or(body.len()));
        cnt += 1;
        match q {
            Some(q) => p = q + 1,
            None => break,
        }
    }
    if cnt != 7 {
        return r;
    }
    r.frag_count = atoi(&body[f[1].0..f[1].1]);
    r.frag_no = atoi(&body[f[2].0..f[2].1]);
    r.channel = if f[4].0 == f[4].1 { b'A' } else { body[f[4].0] };
    r.chars_start = f[5].0;
    r.chars_len = f[5].1 - f[5].0;
    r.fill = atoi(&body[f[6].0..f[6].1]);
    r.state = if r.frag_count >= 1 && r.frag_no >= 1 && r.frag_no <= r.frag_count && r.fill >= 0 && r.fill <= 5 { 2 } else { 1 };
    r
}
