// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Soft-decision Viterbi decoder of the standard K = 7 convolutional code (generators 171 and 133 octal, rate 1/2) of DVB-T (EN 300 744 section 4.3.3), ISDB-T and the
//! other DVB-family receivers, with punctured positions given as 0. 64 states; the soft values are signed bytes (positive = the bit is more likely 0).
//!
//! Path metrics are `i16` with saturating arithmetic and are renormalised every four steps; one metric per butterfly is enough because both generators
//! tap the newest and the oldest register bit (`new[j] = max(old[2j] + M, old[2j+1] - M)`, `new[j+32] = max(old[2j] - M, old[2j+1] + M)`, M = +-x +-y).
//! On aarch64 the 64 metrics sit in eight NEON registers (`viterbi_neon`); everywhere else, and as the reference in tests, `range_scalar` does the same
//! with plain integers. The decision bits of every step go to a caller buffer (one `u64` per step), the trace-back then recovers the input bits.

/// Sign of the first output (generator 0x79) for the new state's register `2j`: +1, or -1 when the parity is odd.
pub(crate) const fn sig_x(j: usize) -> i16 {
    if ((2 * j) as u32 & 0x79).count_ones() & 1 == 1 {
        -1
    } else {
        1
    }
}

/// Sign of the second output (generator 0x5B).
pub(crate) const fn sig_y(j: usize) -> i16 {
    if ((2 * j) as u32 & 0x5B).count_ones() & 1 == 1 {
        -1
    } else {
        1
    }
}

/// Sign tables for the 32 butterflies.
pub(crate) const SIG_X: [i16; 32] = {
    let mut t = [0i16; 32];
    let mut j = 0;
    while j < 32 {
        t[j] = sig_x(j);
        j += 1;
    }
    t
};
/// See `SIG_X`.
pub(crate) const SIG_Y: [i16; 32] = {
    let mut t = [0i16; 32];
    let mut j = 0;
    while j < 32 {
        t[j] = sig_y(j);
        j += 1;
    }
    t
};

/// Trace-back from the best final state. `dec[t]` bit `s` is the decision that led to state `s` at step `t`. Returns nothing, writes `bits[..steps]`.
pub(crate) fn traceback(pm: &[i16; 64], dec: &[u64], bits: &mut [u8]) -> i32 {
    let mut state = 0usize;
    let mut bm = pm[0] as i32;
    for (s, &m) in pm.iter().enumerate().skip(1) {
        if m as i32 > bm {
            bm = m as i32;
            state = s;
        }
    }
    for t in (0..dec.len()).rev() {
        bits[t] = (state >> 5) as u8;
        let b = ((dec[t] >> state) & 1) as usize;
        state = ((state & 31) << 1) | b;
    }
    bm
}

/// Portable version of the whole range decode, same arithmetic as the NEON one: saturating `i16` metrics, renormalised every four steps.
/// `soft` has `2 * steps` values, `dec` and `bits` `steps` entries. Returns the final path metric (the best state's value plus everything renormalised away).
pub fn range_scalar(soft: &[i8], steps: usize, bits: &mut [u8], dec: &mut [u64]) -> i64 {
    let mut off: i64 = 0;
    let mut n = [0i16; 64];
    for t in 0..steps {
        let a = soft[2 * t] as i16;
        let b = soft[2 * t + 1] as i16;
        let mut nn = [0i16; 64];
        let mut d = 0u64;
        for j in 0..32 {
            let m = SIG_X[j].wrapping_mul(a).wrapping_add(SIG_Y[j].wrapping_mul(b));
            let (ev, od) = (n[2 * j], n[2 * j + 1]);
            let p0 = ev.saturating_add(m);
            let p1 = od.saturating_sub(m);
            let r0 = ev.saturating_sub(m);
            let r1 = od.saturating_add(m);
            nn[j] = p0.max(p1);
            nn[j + 32] = r0.max(r1);
            d |= ((p1 > p0) as u64) << j;
            d |= ((r1 > r0) as u64) << (j + 32);
        }
        dec[t] = d;
        if t & 3 == 3 {
            let top = *nn.iter().max().unwrap_or(&0);
            off += top as i64;
            for (x, y) in n.iter_mut().zip(nn.iter()) {
                *x = y.wrapping_sub(top);
            }
        } else {
            n = nn;
        }
    }
    off + traceback(&n, &dec[..steps], bits) as i64
}

/// Decodes `steps` trellis steps. Uses the NEON kernel on aarch64 and `range_scalar` elsewhere. The slices must be long enough (`soft` 2 * steps,
/// `bits` and `dec` steps); returns `None` when they are not.
pub fn range(soft: &[i8], steps: usize, bits: &mut [u8], dec: &mut [u64]) -> Option<i64> {
    if soft.len() < steps.checked_mul(2)? || bits.len() < steps || dec.len() < steps {
        return None;
    }
    #[cfg(target_arch = "aarch64")]
    {
        Some(crate::viterbi_neon::range(soft, steps, bits, dec))
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        Some(range_scalar(soft, steps, bits, dec))
    }
}
