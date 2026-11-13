// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Reed-Solomon style erasure code over GF(256) (polynomial 0x11D) with a Cauchy parity matrix, for the KAVB FEC groups: `g` data cells and `p` parity cells of `cell_len`
//! bytes each; any `g` of the `g + p` cells restore the group. Parity cell j = sum over i of C[j][i] * data cell i with C[j][i] = 1 / (j xor (p + i)) (x_j = j and
//! y_i = p + i are all different, so every square submatrix of the generator is invertible). `g + p` is at most 256.
use alloc::vec;
use alloc::vec::Vec;

struct Gf {
    exp: [u8; 512],
    log: [u8; 256],
}

impl Gf {
    fn new() -> Gf {
        let mut exp = [0u8; 512];
        let mut log = [0u8; 256];
        let mut x: u32 = 1;
        for i in 0..255 {
            exp[i] = x as u8;
            log[x as usize] = i as u8;
            x <<= 1;
            if x & 0x100 != 0 {
                x ^= 0x11D;
            }
        }
        for i in 255..512 {
            exp[i] = exp[i - 255];
        }
        Gf { exp, log }
    }
    fn mul(&self, a: u8, b: u8) -> u8 {
        if a == 0 || b == 0 {
            0
        } else {
            self.exp[self.log[a as usize] as usize + self.log[b as usize] as usize]
        }
    }
    fn inv(&self, a: u8) -> u8 {
        self.exp[255 - self.log[a as usize] as usize]
    }
}

fn valid(g: usize, p: usize, cell_len: usize) -> bool {
    g >= 1 && g + p <= 256 && cell_len >= 1
}

/// Parity cells of `data` (g cells back to back) into `parity` (p cells back to back). False for bad sizes.
pub fn encode(g: usize, p: usize, cell_len: usize, data: &[u8], parity: &mut [u8]) -> bool {
    if !valid(g, p, cell_len) || data.len() != g * cell_len || parity.len() != p * cell_len {
        return false;
    }
    let gf = Gf::new();
    for v in parity.iter_mut() {
        *v = 0;
    }
    for j in 0..p {
        for i in 0..g {
            let c = gf.inv((j ^ (p + i)) as u8);
            let src = &data[i * cell_len..(i + 1) * cell_len];
            let dst = &mut parity[j * cell_len..(j + 1) * cell_len];
            for b in 0..cell_len {
                dst[b] ^= gf.mul(c, src[b]);
            }
        }
    }
    true
}

/// `cells` holds the g + p cells back to back, `present[i]` is non-zero when cell i was received. The missing data cells are rebuilt in place (the parity cells are not
/// touched). True when every data cell is available afterwards, false when fewer than g cells are present or the sizes are bad.
pub fn recover(g: usize, p: usize, cell_len: usize, cells: &mut [u8], present: &[u8]) -> bool {
    let n = g + p;
    if !valid(g, p, cell_len) || cells.len() != n * cell_len || present.len() != n {
        return false;
    }
    let missing: Vec<usize> = (0..g).filter(|&i| present[i] == 0).collect();
    if missing.is_empty() {
        return true;
    }
    let use_idx: Vec<usize> = (0..n).filter(|&i| present[i] != 0).take(g).collect();
    if use_idx.len() < g {
        return false;
    }
    let gf = Gf::new();
    let mut a = vec![0u8; g * g];
    let mut v = vec![0u8; g * cell_len];
    for (r, &idx) in use_idx.iter().enumerate() {
        if idx < g {
            a[r * g + idx] = 1;
        } else {
            for i in 0..g {
                a[r * g + i] = gf.inv(((idx - g) ^ (p + i)) as u8);
            }
        }
        v[r * cell_len..(r + 1) * cell_len].copy_from_slice(&cells[idx * cell_len..(idx + 1) * cell_len]);
    }
    for c in 0..g {
        let piv = match (c..g).find(|&r| a[r * g + c] != 0) {
            Some(r) => r,
            None => return false,
        };
        if piv != c {
            for k in 0..g {
                a.swap(c * g + k, piv * g + k);
            }
            for k in 0..cell_len {
                v.swap(c * cell_len + k, piv * cell_len + k);
            }
        }
        let iv = gf.inv(a[c * g + c]);
        for k in 0..g {
            a[c * g + k] = gf.mul(a[c * g + k], iv);
        }
        for k in 0..cell_len {
            v[c * cell_len + k] = gf.mul(v[c * cell_len + k], iv);
        }
        for r in 0..g {
            let m = a[r * g + c];
            if r == c || m == 0 {
                continue;
            }
            for k in 0..g {
                let t = gf.mul(m, a[c * g + k]);
                a[r * g + k] ^= t;
            }
            for k in 0..cell_len {
                let t = gf.mul(m, v[c * cell_len + k]);
                v[r * cell_len + k] ^= t;
            }
        }
    }
    for &i in &missing {
        cells[i * cell_len..(i + 1) * cell_len].copy_from_slice(&v[i * cell_len..(i + 1) * cell_len]);
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xorshift::XorShift64;

    #[test]
    fn recovers_up_to_p_erasures() {
        let (g, p, len) = (12usize, 4usize, 37usize);
        let mut rng = XorShift64::new(7);
        let data: Vec<u8> = (0..g * len).map(|_| (rng.next() >> 24) as u8).collect();
        let mut parity = vec![0u8; p * len];
        assert!(encode(g, p, len, &data, &mut parity));
        for trial in 0..200 {
            let mut cells = data.clone();
            cells.extend_from_slice(&parity);
            let mut present = vec![1u8; g + p];
            let lose = trial % (p + 1);
            let mut gone = 0;
            while gone < lose {
                let i = (rng.next() % (g + p) as u64) as usize;
                if present[i] != 0 {
                    present[i] = 0;
                    for b in &mut cells[i * len..(i + 1) * len] {
                        *b = 0xEE;
                    }
                    gone += 1;
                }
            }
            assert!(recover(g, p, len, &mut cells, &present));
            assert_eq!(&cells[..g * len], &data[..]);
        }
    }

    #[test]
    fn fails_with_too_many_erasures() {
        let (g, p, len) = (6usize, 2usize, 5usize);
        let data = vec![9u8; g * len];
        let mut parity = vec![0u8; p * len];
        assert!(encode(g, p, len, &data, &mut parity));
        let mut cells = data.clone();
        cells.extend_from_slice(&parity);
        let mut present = vec![1u8; g + p];
        present[0] = 0;
        present[1] = 0;
        present[2] = 0;
        assert!(!recover(g, p, len, &mut cells, &present));
    }

    #[test]
    fn rejects_bad_sizes() {
        let mut parity = vec![0u8; 4];
        assert!(!encode(0, 2, 2, &[], &mut parity));
        assert!(!encode(2, 2, 2, &[0; 3], &mut parity));
        assert!(!encode(200, 100, 1, &[0; 200], &mut [0; 100]));
    }
}
