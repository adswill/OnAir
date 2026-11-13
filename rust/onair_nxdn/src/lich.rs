// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NXDN frame scrambler and LICH (NXDN TS 1-A section 4.3): the PN9 sequence applied to the 182 symbols after the frame sync word, and the
//! 8-bit link information channel (RF channel type, functional channel type, option, direction, even parity).

/// The PN9 scrambler bits (x^9 + x^5 + 1, seed 0xE4), one per byte, filling `out`.
pub fn pn9(out: &mut [u8]) {
    let mut r: u32 = 0xE4;
    for o in out.iter_mut() {
        *o = (r & 1) as u8;
        let b = ((r >> 4) ^ r) & 1;
        r = (r >> 1) | (b << 8);
    }
}

/// The decoded LICH fields.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Lich {
    /// RF channel type: 0 RCCH, 1 RTCH, 2 RDCH, 3 composite.
    pub rfct: i32,
    /// Functional channel type.
    pub fct: i32,
    /// RTCH option: which half carries what.
    pub opt: i32,
    /// Direction, 1 outbound.
    pub dir: i32,
}

/// Even parity over the four channel type bits (bits 6..3 of the 7-bit value).
fn parity(v: i32) -> i32 {
    ((v >> 6) ^ (v >> 5) ^ (v >> 4) ^ (v >> 3)) & 1
}

/// The 8 LICH bits as a byte, MSB first, parity last.
pub fn encode(l: &Lich) -> u8 {
    let v = ((l.rfct & 3) << 5) | ((l.fct & 3) << 3) | ((l.opt & 3) << 1) | (l.dir & 1);
    ((v << 1) | parity(v)) as u8
}

/// Decodes a LICH byte; `None` when the parity is wrong.
pub fn decode(x: u8) -> Option<Lich> {
    let v = (x >> 1) as i32;
    if parity(v) != (x & 1) as i32 {
        return None;
    }
    Some(Lich { rfct: (v >> 5) & 3, fct: (v >> 3) & 3, opt: (v >> 1) & 3, dir: v & 1 })
}
