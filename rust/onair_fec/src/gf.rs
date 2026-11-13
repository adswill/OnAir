// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! GF(2^8) with the primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D), the field of every byte-wise Reed-Solomon code of OnAir
//! (DVB-T RS(204,188) EN 300 744 section 4.4, ATSC RS(207,187) A/53 part 2 section 6.4.1, DAB RS(120,110) EN 300 401 section 5.3.3).
//! The tables are computed at compile time (`const fn`), so nothing has to be initialised at run time and no allocation is needed.

/// Antilog and log tables of GF(256). `exp` has 512 entries (the cycle twice) so a product of two logs needs no reduction.
pub struct Gf {
    /// `exp[i]` = alpha^i for i in 0..510 (period 255).
    pub exp: [u8; 512],
    /// `log[v]` = i with alpha^i = v; `log[0]` is 0 and never used for a non-zero product.
    pub log: [u8; 256],
}

impl Gf {
    /// Builds the tables by repeated multiplication with alpha.
    pub const fn new() -> Gf {
        let mut exp = [0u8; 512];
        let mut log = [0u8; 256];
        let mut x: u32 = 1;
        let mut i = 0;
        while i < 255 {
            exp[i] = x as u8;
            log[x as usize] = i as u8;
            x <<= 1;
            if x & 0x100 != 0 {
                x ^= 0x11D;
            }
            i += 1;
        }
        while i < 512 {
            exp[i] = exp[i - 255];
            i += 1;
        }
        Gf { exp, log }
    }

    /// Field product (0 when either factor is 0).
    #[inline]
    pub const fn mul(&self, a: u8, b: u8) -> u8 {
        if a != 0 && b != 0 {
            self.exp[self.log[a as usize] as usize + self.log[b as usize] as usize]
        } else {
            0
        }
    }

    /// Multiplicative inverse (the inverse of 0 is reported as 0 to keep the function total; callers never pass 0).
    #[inline]
    pub const fn inv(&self, a: u8) -> u8 {
        if a == 0 {
            0
        } else {
            self.exp[255 - self.log[a as usize] as usize]
        }
    }
}

impl Default for Gf {
    fn default() -> Gf {
        Gf::new()
    }
}

/// The one field instance.
pub static GF: Gf = Gf::new();
