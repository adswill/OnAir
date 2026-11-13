// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! FLEX frame handling (no free specification exists; the layout follows the public descriptions of the protocol and the way multimon-ng and
//! PDW-style decoders read it): sync code table, frame information word (FIW) checksum, and the decoding of one 11-block phase (88 code words,
//! 8-way bit interleave per block) into address / vector / message words and then pages. The encoder (`flexPhaseBits`) stays in C++.
use crate::bch::{correct_word, rev21};
use crate::text::{alpha_from_bits, numeric_from_bits, NUM_FLEX};

/// Code words in one phase: 11 blocks of 8.
pub const PHASE_WORDS: usize = 88;
/// Transmitted bits of one phase.
pub const PHASE_BITS: usize = PHASE_WORDS * 32;
/// Longest page text a phase can produce (87 message words of three characters, with room to spare).
pub const TEXT_MAX: usize = 320;

/// Message types shared with C++ (`PagerMsgType`).
pub const T_TONE: i32 = 0;
/// Numeric page.
pub const T_NUMERIC: i32 = 1;
/// Alphanumeric page.
pub const T_ALPHA: i32 = 2;
/// Secure page (not shown).
pub const T_SECURE: i32 = 3;
/// Binary data page.
pub const T_BINARY: i32 = 4;

/// One entry of the speed table: sync code A of sync 1, data symbol rate, number of levels and the `PagerSpeed` value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mode {
    /// Sync code A.
    pub code: u16,
    /// Symbol rate of the data.
    pub baud: i32,
    /// 2 or 4 levels.
    pub levels: i32,
    /// `PagerSpeed` enum value (kFlex1600_2 = 3 ... kFlex6400_4 = 6).
    pub speed: i32,
}

/// 0x4C7C is a second code for 6400 bits/s that some transmitters use.
pub const MODES: [Mode; 5] = [
    Mode { code: 0x870C, baud: 1600, levels: 2, speed: 3 },
    Mode { code: 0x7B18, baud: 3200, levels: 2, speed: 4 },
    Mode { code: 0xB068, baud: 1600, levels: 4, speed: 5 },
    Mode { code: 0xDEA0, baud: 3200, levels: 4, speed: 6 },
    Mode { code: 0x4C7C, baud: 3200, levels: 4, speed: 6 },
];

/// Index of the table entry closest to `code` (Hamming distance at most `max_distance`; the first of equals wins), or None.
pub fn mode_for_code(code: u16, max_distance: i32) -> Option<usize> {
    let mut best = None;
    let mut bd = max_distance.saturating_add(1);
    for (i, m) in MODES.iter().enumerate() {
        let d = (m.code ^ code).count_ones() as i32;
        if d < bd {
            bd = d;
            best = Some(i);
        }
    }
    best
}

/// Index of the first table entry with this `PagerSpeed`, or None.
pub fn mode_for_speed(speed: i32) -> Option<usize> {
    MODES.iter().position(|m| m.speed == speed)
}

/// FIW checksum: the five nibbles of the data plus bit 20 (the checksum nibble is the first).
fn fiw_sum(f: u32) -> i32 {
    let mut s = 0;
    for i in (0..20).step_by(4) {
        s += ((f >> i) & 0xF) as i32;
    }
    s + ((f >> 20) & 1) as i32
}

/// The frame information word for a cycle and frame number, with its checksum nibble (field value, first transmitted bit in bit 0).
pub fn fiw_data(cycle: i32, frame: i32) -> u32 {
    let mut f = (((cycle & 15) as u32) << 4) | (((frame & 127) as u32) << 8);
    f |= ((0xF - fiw_sum(f)) & 0xF) as u32;
    f
}

/// The block information word and the first vector word of every page carry the same 4-bit check as the FIW (bits 0-3; the nibbles of the
/// 21 data bits plus bit 20 sum to 0xF). Real recordings (issue #95, #261) confirm it on the BIW and on vector words.
pub fn check_ok(f: u32) -> bool {
    (fiw_sum(f) & 0xF) == 0xF
}

/// Checks a FIW and returns (cycle, frame) when the checksum is right.
pub fn fiw_check(f: u32) -> Option<(i32, i32)> {
    if (fiw_sum(f) & 0xF) != 0xF {
        return None;
    }
    Some((((f >> 4) & 15) as i32, ((f >> 8) & 127) as i32))
}

/// One decoded page (C layout: the C++ side mirrors it as `OnairPagerPage`).
#[derive(Clone, Copy)]
#[repr(C)]
pub struct Page {
    /// Capcode.
    pub address: u32,
    /// Message type (`T_*`).
    pub mtype: i32,
    /// Bits the BCH code corrected in the words of this page.
    pub fixed: i32,
    /// A word of the message could not be repaired.
    pub damaged: bool,
    /// Used bytes of `text`.
    pub len: u32,
    /// Text (numeric digits or characters, or a fixed note for secure / binary pages).
    pub text: [u8; TEXT_MAX],
}

impl Page {
    fn new(address: u32) -> Self {
        Page { address, mtype: T_TONE, fixed: 0, damaged: false, len: 0, text: [0; TEXT_MAX] }
    }
    fn set_text(&mut self, s: &[u8]) {
        let n = s.len().min(TEXT_MAX);
        self.len = n as u32;
        self.text[..n].copy_from_slice(&s[..n]);
    }
}

/// Counters of one phase.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct PhaseStat {
    /// Words that passed unchanged.
    pub ok: i32,
    /// Words repaired by the BCH code.
    pub fixed: i32,
    /// Words that could not be repaired.
    pub failed: i32,
    /// The phase carried a block information word.
    pub biw_ok: bool,
}

/// Appends a 4-bit group (bits `sh..sh+4` of `w`, least significant first) to a bit list.
fn nib(nb: &mut Bits, w: u32, sh: u32) {
    for k in 0..4 {
        nb.push(((w >> (sh + k)) & 1) as u8);
    }
}

/// A bounded bit list (the longest message of a phase is 88 words of 21 bits).
struct Bits {
    b: [u8; PHASE_WORDS * 21 + 8],
    n: usize,
}
impl Bits {
    fn new() -> Self {
        Bits { b: [0; PHASE_WORDS * 21 + 8], n: 0 }
    }
    fn push(&mut self, v: u8) {
        if self.n < self.b.len() {
            self.b[self.n] = v;
            self.n += 1;
        }
    }
    fn slice(&self) -> &[u8] {
        &self.b[..self.n]
    }
}

/// Decodes the 2816 transmitted bits of one phase (`bits`, one per byte; shorter input counts as zeros) into pages appended to `out`
/// (at most `out.len()`); returns the number of pages. Words are de-interleaved 8 ways per block, repaired and bit-reversed; the block
/// information word (first word) gives the address and vector offsets; every address word is paired with the vector word at the same index
/// (two words each for a long address), and the vector type picks the message reader (alphanumeric, numeric, short numeric, tone, binary).
pub fn decode_phase(bits: &[u8], out: &mut [Page], st: &mut PhaseStat) -> usize {
    let bit = |i: usize| -> u32 { u32::from(bits.get(i).copied().unwrap_or(0) & 1) };
    let mut f = [0u32; PHASE_WORDS];
    let mut fx = [0i32; PHASE_WORDS];
    for blk in 0..11 {
        for w in 0..8 {
            let mut word = 0u32;
            for j in 0..32 {
                word = (word << 1) | bit(blk * 256 + j * 8 + w);
            }
            let c = correct_word(word);
            let i = blk * 8 + w;
            if c.ok {
                f[i] = rev21(c.data);
                fx[i] = c.bits;
                if c.bits != 0 {
                    st.fixed += 1;
                } else {
                    st.ok += 1;
                }
            } else {
                f[i] = 0;
                fx[i] = -1;
                st.failed += 1;
            }
        }
    }
    let mut count = 0;
    if fx[0] < 0 || f[0] == 0 || f[0] == 0x1F_FFFF || !check_ok(f[0]) {
        return 0;
    }
    st.biw_ok = true;
    let aoff = ((f[0] >> 8) & 3) as usize + 1;
    let voff = ((f[0] >> 10) & 0x3F) as usize;
    if voff < aoff || voff >= PHASE_WORDS {
        return 0;
    }
    let (mut i, mut j) = (aoff, voff);
    while i < voff && j < PHASE_WORDS {
        let a = f[i];
        if fx[i] < 0 || a == 0 || a == 0x1F_FFFF {
            i += 1;
            j += 1;
            continue;
        }
        let long_addr = !(0x8001..=0x1E_0000).contains(&a);
        let mut used = 1;
        let mut cap = a.wrapping_sub(0x8000); // short address: the word minus 32768 (multimon-ng parse_capcode)
        let mut fixed_bits = fx[i];
        if long_addr {
            // a long address takes two words and two vector words (the formula is what PDW-style decoders use)
            if i + 1 >= voff || fx[i + 1] < 0 {
                i += 2;
                j += 2;
                continue;
            }
            used = 2;
            cap = (u64::from(a) + (u64::from(f[i + 1] ^ 0x1F_FFFF) << 15) + 0x1F_9000) as u32;
            fixed_bits += fx[i + 1];
        }
        if j >= PHASE_WORDS || fx[j] < 0 {
            i += used;
            j += used;
            continue;
        }
        let viw = f[j];
        if !check_ok(viw) {
            // a vector word whose check fails is not a vector (a mis-repaired word, or a field this decoder does not understand)
            i += used;
            j += used;
            continue;
        }
        let has_viw2 = long_addr && j + 1 < PHASE_WORDS && fx[j + 1] >= 0;
        let viw2 = if has_viw2 { f[j + 1] } else { 0 };
        let j1 = j;
        fixed_bits += fx[j];
        i += used;
        j += used;
        // vector word: type bits 4-6, start word bits 7-13, length (words, the header included) bits 14-20
        let vtype = (viw >> 4) & 7;
        let mw1 = ((viw >> 7) & 0x7F) as usize;
        let mut m = Page::new(cap);
        let mut tmp = [0u8; TEXT_MAX];
        if vtype == 2 {
            // tone only; bits 7-8 say 1 = tone, 0 = a short numeric page (3 digits in the vector word, 5 more in the second one of a long address)
            if ((viw >> 7) & 3) == 0 {
                let mut nb = Bits::new();
                nib(&mut nb, viw, 9);
                nib(&mut nb, viw, 13);
                nib(&mut nb, viw, 17);
                if has_viw2 {
                    for sh in (0..=16).step_by(4) {
                        nib(&mut nb, viw2, sh);
                    }
                }
                m.mtype = T_NUMERIC;
                let l = numeric_from_bits(nb.slice(), NUM_FLEX, true, &mut tmp);
                m.set_text(&tmp[..l]);
            } else {
                m.mtype = T_TONE;
            }
        } else if vtype == 5 || vtype == 0 {
            // alphanumeric: a header word (continuation bit 10, fragment bits 11-12), then the text, three 7-bit characters per word (bits 0-6,
            // 7-13, 14-20); in the first text word bits 0-6 are not text when the fragment field is 3 (a complete message); a character 3 is a
            // filler. The length counts the header. Short address: the header is the word at the start index and the text follows it. Long
            // address: the header is the second vector word and the text starts at the start index (seen in real 3200/4 frames: with that
            // reading the messages of a phase tile its message field exactly, with the header at the start index they overlap by one word).
            // Fragments are not joined.
            let len = ((viw >> 14) & 0x7F) as usize;
            if len < 1 || mw1 >= PHASE_WORDS || (long_addr && len < 2) {
                continue;
            }
            let (hdr, hfx, t1, t2) = if long_addr {
                (viw2, if has_viw2 { fx[j1 + 1] } else { -1 }, mw1, (mw1 + len - 2).min(PHASE_WORDS - 1))
            } else {
                (f[mw1], fx[mw1], mw1 + 1, (mw1 + len - 1).min(PHASE_WORDS - 1))
            };
            if vtype == 0 {
                m.mtype = T_SECURE;
                m.set_text(b"secure, not shown");
            } else {
                m.mtype = T_ALPHA;
                if hfx < 0 {
                    m.damaged = true;
                } else {
                    fixed_bits += hfx;
                }
                let frag = if hfx < 0 { 3 } else { (hdr >> 11) & 3 };
                let mut b7 = Bits::new();
                let put = |b7: &mut Bits, c: u32| {
                    for k in 0..7 {
                        b7.push(((c >> k) & 1) as u8);
                    }
                };
                for w in t1..=t2 {
                    if fx[w] < 0 {
                        m.damaged = true;
                        continue;
                    }
                    fixed_bits += fx[w];
                    if w > t1 || frag != 3 {
                        put(&mut b7, f[w] & 0x7F);
                    }
                    put(&mut b7, (f[w] >> 7) & 0x7F);
                    put(&mut b7, (f[w] >> 14) & 0x7F);
                }
                let l = alpha_from_bits(b7.slice(), &mut tmp);
                m.set_text(&tmp[..l]);
            }
        } else if vtype == 3 || vtype == 4 || vtype == 7 {
            // numeric: start word bits 7-13, 3 bits (14-16) = further words, so e + 1 words; digits are read from bit 0 of each word upwards in
            // 4-bit groups running across word boundaries, after skipping 2 bits (10 for numbered numeric); 12 is filler. A long address starts
            // with the second vector word and then takes e words from the start word.
            let cnt = ((viw >> 14) & 7) as usize + 1;
            let mut nb = Bits::new();
            let mut skip = if vtype == 7 { 10 } else { 2 };
            let mut feed = |nb: &mut Bits, w: u32| {
                for k in 0..21 {
                    if skip > 0 {
                        skip -= 1;
                        continue;
                    }
                    nb.push(((w >> k) & 1) as u8);
                }
            };
            if long_addr {
                if has_viw2 {
                    fixed_bits += fx[j1 + 1];
                    feed(&mut nb, viw2);
                } else {
                    m.damaged = true;
                }
                for w in mw1..mw1 + cnt - 1 {
                    if w >= PHASE_WORDS || fx[w] < 0 {
                        m.damaged = true;
                    } else {
                        fixed_bits += fx[w];
                        feed(&mut nb, f[w]);
                    }
                }
            } else {
                for w in mw1..mw1 + cnt {
                    if w >= PHASE_WORDS || fx[w] < 0 {
                        m.damaged = true;
                    } else {
                        fixed_bits += fx[w];
                        feed(&mut nb, f[w]);
                    }
                }
            }
            m.mtype = T_NUMERIC;
            let l = numeric_from_bits(nb.slice(), NUM_FLEX, true, &mut tmp);
            m.set_text(&tmp[..l]);
        } else if vtype == 6 {
            m.mtype = T_BINARY;
            m.set_text(b"binary data");
        } else {
            continue; // short instruction (group messages): nothing to show
        }
        m.fixed = fixed_bits;
        if count < out.len() {
            out[count] = m;
            count += 1;
        }
    }
    count
}
