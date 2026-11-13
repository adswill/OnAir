// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NEON kernel of the K = 7 Viterbi decoder (aarch64 only). The same butterflies as `viterbi::range_scalar`, eight states per register: the 64 path
//! metrics stay in eight registers for the whole run (a store and reload per step would make the chain of steps latency bound), the 64 decisions of a step are
//! packed to one `u64` by narrowing the comparison masks to bytes, weighting by 1, 2, 4 ... 128 and adding neighbours. This file is the only place outside
//! `ffi.rs` with `unsafe`: the `core::arch` intrinsics are `unsafe fn`s (NEON is part of the aarch64 baseline, so they are always available).
#![allow(unsafe_code)]

use crate::viterbi::{traceback, SIG_X, SIG_Y};
use core::arch::aarch64::*;

/// Decodes `steps` steps (slice lengths are checked by `viterbi::range`). Returns the final path metric.
pub fn range(soft: &[i8], steps: usize, bits: &mut [u8], dec: &mut [u64]) -> i64 {
    // SAFETY: NEON is mandatory on aarch64; all loads read the fixed-size tables below, `soft` is indexed with bounds checks.
    unsafe {
        let mut off: i64 = 0;
        let mut sig_x = [vdupq_n_s16(0); 4];
        let mut sig_y = [vdupq_n_s16(0); 4];
        for q in 0..4 {
            sig_x[q] = vld1q_s16(SIG_X.as_ptr().add(8 * q));
            sig_y[q] = vld1q_s16(SIG_Y.as_ptr().add(8 * q));
        }
        let mut n = [vdupq_n_s16(0); 8];
        const W8: [u8; 16] = [1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128];
        let wt = vld1q_u8(W8.as_ptr());
        // One trellis step: new metrics from `n` (not renormalised here), the 64 decisions as a word.
        #[inline(always)]
        unsafe fn step(
            n: &[int16x8_t; 8],
            sig_x: &[int16x8_t; 4],
            sig_y: &[int16x8_t; 4],
            wt: uint8x16_t,
            a: i16,
            b: i16,
        ) -> ([int16x8_t; 8], u64) {
            let mut nn = [vdupq_n_s16(0); 8];
            let mut lo = [vdupq_n_u16(0); 4];
            let mut hi = [vdupq_n_u16(0); 4];
            for q in 0..4 {
                let o = vuzpq_s16(n[2 * q], n[2 * q + 1]); // even states, odd states
                let m = vmlaq_n_s16(vmulq_n_s16(sig_x[q], a), sig_y[q], b);
                let (ev, od) = (o.0, o.1);
                let p0 = vqaddq_s16(ev, m);
                let p1 = vqsubq_s16(od, m);
                let r0 = vqsubq_s16(ev, m);
                let r1 = vqaddq_s16(od, m);
                nn[q] = vmaxq_s16(p0, p1);
                nn[4 + q] = vmaxq_s16(r0, r1);
                lo[q] = vcgtq_s16(p1, p0);
                hi[q] = vcgtq_s16(r1, r0);
            }
            let c0 = vandq_u8(vcombine_u8(vmovn_u16(lo[0]), vmovn_u16(lo[1])), wt);
            let c1 = vandq_u8(vcombine_u8(vmovn_u16(lo[2]), vmovn_u16(lo[3])), wt);
            let c2 = vandq_u8(vcombine_u8(vmovn_u16(hi[0]), vmovn_u16(hi[1])), wt);
            let c3 = vandq_u8(vcombine_u8(vmovn_u16(hi[2]), vmovn_u16(hi[3])), wt);
            let mut r = vpaddq_u8(vpaddq_u8(c0, c1), vpaddq_u8(c2, c3));
            r = vpaddq_u8(r, r);
            (nn, vgetq_lane_u64::<0>(vreinterpretq_u64_u8(r)))
        }
        // blocks of four steps, then the renormalisation (int16 never saturates); the 0..3 steps left over are not renormalised
        let mut t = 0usize;
        for (sc, dc) in soft[..2 * steps].chunks_exact(8).zip(dec[..steps].chunks_exact_mut(4)) {
            let mut nn = n;
            for k in 0..4 {
                let (x, d) = step(&nn, &sig_x, &sig_y, wt, sc[2 * k] as i16, sc[2 * k + 1] as i16);
                nn = x;
                dc[k] = d;
            }
            let mut mx = nn[0];
            for v in nn.iter().skip(1) {
                mx = vmaxq_s16(mx, *v);
            }
            let top = vmaxvq_s16(mx);
            off += top as i64;
            let m = vdupq_n_s16(top);
            for (x, y) in n.iter_mut().zip(nn.iter()) {
                *x = vsubq_s16(*y, m);
            }
            t += 4;
        }
        while t < steps {
            let (x, d) = step(&n, &sig_x, &sig_y, wt, soft[2 * t] as i16, soft[2 * t + 1] as i16);
            n = x;
            dec[t] = d;
            t += 1;
        }
        let mut pm = [0i16; 64];
        for (q, x) in n.iter().enumerate() {
            vst1q_s16(pm.as_mut_ptr().add(8 * q), *x);
        }
        off + traceback(&pm, &dec[..steps], bits) as i64
    }
}
