// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! POCSAG code word classification (ITU-R M.584-2): which kind of word came out of the BCH check. The batch / message assembly (state with a growing
//! bit list) stays in C++ (`PocsagParser` in core/src/pager/pager_proto.cpp); this is its per-word decision.
use crate::bch::{correct_word, encode_word};

/// POCSAG idle code word.
pub const IDLE: u32 = 0x7A89_C197;

/// What a received code word is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Word {
    /// Too damaged to repair (more than two bad bits).
    Failed,
    /// The idle code word: ends the message in progress.
    Idle,
    /// An address word: starts a message. `ric18` are the top 18 address bits, `func` the two function bits.
    Address { ric18: u32, func: u32, bits: i32 },
    /// A message word: 20 data bits (first transmitted bit in bit 19).
    Data { data20: u32, bits: i32 },
}

/// Classifies one received 32-bit word. `bits` is the number of bits the BCH code corrected (the parity bit counts).
pub fn classify(raw: u32) -> (Word, bool) {
    let c = correct_word(raw);
    if !c.ok {
        return (Word::Failed, false);
    }
    let fixed = c.bits != 0;
    if c.data == (IDLE >> 11) && encode_word(c.data) == IDLE {
        return (Word::Idle, fixed);
    }
    if (c.data >> 20) == 0 {
        (Word::Address { ric18: (c.data >> 2) & 0x3FFFF, func: c.data & 3, bits: c.bits }, fixed)
    } else {
        (Word::Data { data20: c.data & 0xFFFFF, bits: c.bits }, fixed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn kinds() {
        assert_eq!(classify(IDLE).0, Word::Idle);
        let a = encode_word((0x1234 << 2) | 1);
        assert_eq!(classify(a).0, Word::Address { ric18: 0x1234, func: 1, bits: 0 });
        let d = encode_word((1 << 20) | 0xABCDE);
        assert_eq!(classify(d ^ 4).0, Word::Data { data20: 0xABCDE, bits: 1 });
        assert_eq!(classify(a ^ 0b1110_0000).0, Word::Failed);
    }
}
