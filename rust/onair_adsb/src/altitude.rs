// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Altitude, identity and character codes (ICAO Annex 10 Vol. IV 3.1.2.6.5 altitude, 3.1.2.6.7 identity; DO-260B 2.2.3.2.5 identification).

/// Gillham code C1 C2 C4 as a 3 bit number, lowest 100 ft level first: each 500 ft step is split into five levels in this order for even steps and
/// reversed for odd steps.
const C_ORDER: [u32; 5] = [1, 3, 2, 6, 4];

/// Gillham (Mode C) code of 11 bits `C1 A1 C2 A2 C4 A4 B1 B2 D2 B4 D4` (D1 is 0) to altitude in feet; `None` for an undefined code. Bits D2 D4 A1 A2
/// A4 B1 B2 B4 are a reflected binary (Gray) count of 500 ft steps above -1200 ft, C1 C2 C4 give the 100 ft level (ICAO Annex 10 Vol. IV; the layout
/// as described at en.wikipedia.org/wiki/Gillham_code, which also lists 0 ft = 000 000 011 010 for D A B C).
pub fn gillham_to_altitude(code: u32) -> Option<i32> {
    let bit = |n: u32| (code >> n) & 1;
    let (c1, a1, c2, a2, c4, a4) = (bit(10), bit(9), bit(8), bit(7), bit(6), bit(5));
    let (b1, b2, d2, b4, d4) = (bit(4), bit(3), bit(2), bit(1), bit(0));
    let gray = (d2 << 7) | (d4 << 6) | (a1 << 5) | (a2 << 4) | (a4 << 3) | (b1 << 2) | (b2 << 1) | b4;
    let mut n = gray;
    let mut s = 1;
    while s < 8 {
        n ^= n >> s; // Gray to binary
        s <<= 1;
    }
    let c = (c1 << 2) | (c2 << 1) | c4;
    let r = C_ORDER.iter().rposition(|&x| x == c)? as i32;
    let level = if n & 1 != 0 { 4 - r } else { r }; // odd steps run the other way
    let alt = -1200 + 500 * n as i32 + 100 * level;
    (-1200..=126700).contains(&alt).then_some(alt)
}

/// The inverse (-1200 to 126700 ft in 100 ft steps): the 11 bit code; `None` when out of range.
pub fn altitude_to_gillham(alt_ft: i32) -> Option<u32> {
    if !(-1200..=126700).contains(&alt_ft) {
        return None;
    }
    let a = (alt_ft + 1200 + 50) / 100; // 100 ft steps
    if !(0..=1279).contains(&a) {
        return None;
    }
    let n = (a / 5) as u32;
    let level = a % 5;
    let r = if n & 1 != 0 { 4 - level } else { level };
    let c = C_ORDER[r as usize];
    let gray = n ^ (n >> 1);
    let bit = |v: u32, n: u32| (v >> n) & 1;
    let (d2, d4, a1, a2, a4, b1, b2, b4) =
        (bit(gray, 7), bit(gray, 6), bit(gray, 5), bit(gray, 4), bit(gray, 3), bit(gray, 2), bit(gray, 1), bit(gray, 0));
    let (c1, c2, c4) = (bit(c, 2), bit(c, 1), bit(c, 0));
    Some((c1 << 10) | (a1 << 9) | (c2 << 8) | (a2 << 7) | (c4 << 6) | (a4 << 5) | (b1 << 4) | (b2 << 3) | (d2 << 2) | (b4 << 1) | d4)
}

/// The 13 bit AC field of DF0 / 4 / 16 / 20 (bits 20-32: `C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4`): the altitude in feet, `None` when it carries none
/// or a code that is not defined. M = 1 (metric altitude) is not decoded; Q = 1 means 25 ft steps, otherwise Gillham.
pub fn decode_ac13(ac: u32) -> Option<i32> {
    let ac = ac & 0x1FFF;
    if ac == 0 {
        return None; // no altitude information
    }
    if ac & 0x0040 != 0 {
        return None; // M = 1: metric altitude
    }
    // the 11 bits without M and Q
    let code = ((ac & 0x1F80) >> 2) | ((ac & 0x0020) >> 1) | (ac & 0x000F);
    if ac & 0x0010 != 0 {
        return Some(code as i32 * 25 - 1000); // Q = 1: 25 ft steps
    }
    gillham_to_altitude(code)
}

/// The 12 bit altitude of an airborne position message (the AC field without the M bit).
pub fn decode_alt12(alt12: u32) -> Option<i32> {
    let alt12 = alt12 & 0xFFF;
    decode_ac13(((alt12 & 0xFC0) << 1) | (alt12 & 0x3F))
}

/// The 13 bit identity field (`C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4`) to the four octal digits of the squawk, written as a decimal number (7700).
pub fn decode_id13(id: u32) -> i32 {
    let bit = |n: u32| (id >> n) & 1;
    let (c1, a1, c2, a2, c4, a4) = (bit(12), bit(11), bit(10), bit(9), bit(8), bit(7));
    let (b1, d1, b2, d2, b4, d4) = (bit(5), bit(4), bit(3), bit(2), bit(1), bit(0));
    ((a4 * 4 + a2 * 2 + a1) * 1000 + (b4 * 4 + b2 * 2 + b1) * 100 + (c4 * 4 + c2 * 2 + c1) * 10 + (d4 * 4 + d2 * 2 + d1)) as i32
}

/// The inverse of [`decode_id13`] (digits above 7 are masked to three bits).
pub fn encode_id13(sq: i32) -> u32 {
    let a = ((sq / 1000) as u32) & 7;
    let b = (((sq / 100) as u32) % 10) & 7;
    let c = (((sq / 10) as u32) % 10) & 7;
    let d = ((sq as u32) % 10) & 7;
    let bit = |v: u32, n: u32| (v >> n) & 1;
    (bit(c, 0) << 12)
        | (bit(a, 0) << 11)
        | (bit(c, 1) << 10)
        | (bit(a, 1) << 9)
        | (bit(c, 2) << 8)
        | (bit(a, 2) << 7)
        | (bit(b, 0) << 5)
        | (bit(d, 0) << 4)
        | (bit(b, 1) << 3)
        | (bit(d, 1) << 2)
        | (bit(b, 2) << 1)
        | bit(d, 2)
}

/// Aircraft identification character set (Annex 10 Vol. IV 3.1.2.9, DO-260B): 1 - 26 A - Z, 32 space, 48 - 57 digits; '#' for a code not defined.
pub fn char_from_6bit(c: u32) -> u8 {
    match c {
        1..=26 => b'A' + (c - 1) as u8,
        32 => b' ',
        48..=57 => b'0' + (c - 48) as u8,
        _ => b'#',
    }
}

/// The 6 bit code of a character (lower case is accepted), -1 when it has none.
pub fn char_to_6bit(ch: u8) -> i32 {
    match ch {
        b'A'..=b'Z' => (ch - b'A') as i32 + 1,
        b'a'..=b'z' => (ch - b'a') as i32 + 1,
        b' ' => 32,
        b'0'..=b'9' => 48 + (ch - b'0') as i32,
        _ => -1,
    }
}

/// Category code from the type code (1 - 4: sets D, C, B, A) and the CA field: "A3". `None` outside those ranges.
pub fn category_code(tc: i32, ca: i32) -> Option<[u8; 2]> {
    if !(1..=4).contains(&tc) || !(0..=7).contains(&ca) {
        return None;
    }
    Some([b'A' + (4 - tc) as u8, b'0' + ca as u8])
}

/// Emitter category name (DO-260B, as listed at adsbexchange.com "Emitter Category"); codes the standard leaves reserved say "Reserved". NUL-terminated.
pub fn category_name(tc: i32, ca: i32) -> &'static str {
    const A: [&str; 8] = [
        "\0",
        "Light (< 15500 lb)\0",
        "Small (15500 - 75000 lb)\0",
        "Large (75000 - 300000 lb)\0",
        "High vortex large\0",
        "Heavy (> 300000 lb)\0",
        "High performance\0",
        "Rotorcraft\0",
    ];
    const B: [&str; 8] =
        ["\0", "Glider\0", "Lighter than air\0", "Parachutist\0", "Ultralight\0", "\0", "Unmanned aircraft\0", "Space vehicle\0"];
    const C: [&str; 8] = ["\0", "Surface emergency vehicle\0", "Surface service vehicle\0", "Point obstacle\0", "\0", "\0", "\0", "\0"];
    if !(1..=4).contains(&tc) {
        return "\0";
    }
    if ca == 0 {
        return "No information\0";
    }
    if !(0..=7).contains(&ca) {
        return "\0";
    }
    let s = match tc {
        4 => A[ca as usize],
        3 => B[ca as usize],
        2 => C[ca as usize],
        _ => "\0",
    };
    if s == "\0" {
        "Reserved\0"
    } else {
        s
    }
}

/// Aircraft status emergency state 0 - 7 (DO-260B 2.2.3.2.7.8.1); "" outside. NUL-terminated.
pub fn emergency_name(s: i32) -> &'static str {
    const N: [&str; 8] = [
        "None\0",
        "General emergency\0",
        "Medical\0",
        "Minimum fuel\0",
        "No communications\0",
        "Unlawful interference\0",
        "Downed aircraft\0",
        "Reserved\0",
    ];
    if (0..8).contains(&s) {
        N[s as usize]
    } else {
        "\0"
    }
}
