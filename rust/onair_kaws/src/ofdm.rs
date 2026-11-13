// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Coherent OFDM for the profiles P5-P7 (spec section 2): 48 carriers at 50 Hz (2400 Hz), 20 ms symbol (48 samples at 2400/s) plus a 5 ms cyclic prefix
//! (12 samples) = 25 ms, 40 symbols/s; every 4th carrier is a pilot (12 pilots, 36 data carriers, 1440 data symbols/s). Data carriers: BPSK (P5) or
//! QPSK (P6, P7), Gray mapped, unit average power. Pilots: known BPSK from a fixed xorshift sequence, same power as data.
//! Receiver: pilot LS channel estimate per symbol, smoothed over three symbols in time, linear interpolation over the carriers, one-tap equaliser as
//! soft demapping (LLR = scaled Re(H* y)). Carrier i has baseband frequency (i - 24) x 50 Hz.
use crate::cplx::C;
use crate::mathf;
use crate::profile::{Kind, Profile};
use crate::rng::Rng;
use alloc::vec;
use alloc::vec::Vec;

pub const NC: usize = 48;
pub const CP: usize = 12;
pub const SYM: usize = NC + CP; // 60 samples = 25 ms
pub const NDATA: usize = 36;
/// FFT window start inside the 60-sample symbol (the nominal start is the cyclic prefix end, 12; the window starts a few samples early to tolerate timing error).
pub const WINDOW_START: usize = 9;
/// Smallest delay (samples) of the 12 channel taps estimated from the pilots (taps cover TAP_LO .. TAP_LO + 11, relative to the FFT window start).
pub const TAP_LO: i32 = -1;

fn is_pilot(i: usize) -> bool { i % 4 == 0 }

fn twiddles() -> Vec<C> { (0..NC).map(|k| C::polar(1.0, 2.0 * mathf::PI * k as f64 / NC as f64)).collect() }

/// Pilot symbol (+1 or -1) for OFDM symbol `s` and carrier `i`.
fn pilot(s: usize, i: usize) -> f32 {
    let mut r = Rng::new(0x4F46_444D_0000 + (s as u64) * 64 + i as u64);
    if r.next_u64() & 1 == 0 { 1.0 } else { -1.0 }
}

/// Inverse DFT (naive, 48 points) of carrier values (index i = carrier, frequency (i-24) x 50 Hz) to NC time samples, unit mean power for unit carriers.
fn idft(x: &[C; NC], tw: &[C]) -> Vec<C> {
    let sc = 1.0 / mathf::sqrt(NC as f64) as f32;
    let mut out = Vec::with_capacity(NC);
    for n in 0..NC {
        let mut acc = C::ZERO;
        for i in 0..NC {
            // exp(j 2 pi (i-24) n / 48)
            let k = ((i + NC - 24) % NC * n) % NC;
            acc += x[i] * tw[k];
        }
        out.push(acc.scale(sc));
    }
    out
}

fn dft(x: &[C], tw: &[C]) -> [C; NC] {
    let sc = 1.0 / mathf::sqrt(NC as f64) as f32;
    let mut out = [C::ZERO; NC];
    for i in 0..NC {
        let mut acc = C::ZERO;
        for n in 0..NC {
            let k = ((i + NC - 24) % NC * n) % NC;
            acc += x[n] * tw[k].conj();
        }
        out[i] = acc.scale(sc);
    }
    out
}

fn bps_of(p: &Profile) -> usize { match p.kind { Kind::Ofdm { bps, .. } => bps, _ => 1 } }

/// Modulate one block (n coded bits, interleaved order) into nsym x 60 samples.
pub fn modulate_block(p: &Profile, coded: &[u8]) -> Vec<C> {
    let bps = bps_of(p);
    let nsym = p.block_symbols();
    let tw = twiddles();
    let mut out = Vec::with_capacity(nsym * SYM);
    let mut bi = 0usize;
    let q = core::f32::consts::FRAC_1_SQRT_2;
    for s in 0..nsym {
        let mut x = [C::ZERO; NC];
        for i in 0..NC {
            if is_pilot(i) {
                x[i] = C::new(pilot(s, i), 0.0);
            } else {
                let b0 = if bi < coded.len() { coded[bi] & 1 } else { 0 };
                bi += 1;
                if bps == 1 {
                    x[i] = C::new(if b0 == 0 { 1.0 } else { -1.0 }, 0.0);
                } else {
                    let b1 = if bi < coded.len() { coded[bi] & 1 } else { 0 };
                    bi += 1;
                    x[i] = C::new(if b0 == 0 { q } else { -q }, if b1 == 0 { q } else { -q });
                }
            }
        }
        let t = idft(&x, &tw);
        out.extend_from_slice(&t[NC - CP..]);
        out.extend_from_slice(&t);
    }
    out
}

/// Receiver state of one block: the carrier values of every symbol and the channel estimate machinery.
pub struct Eq {
    pub nsym: usize,
    pub bps: usize,
    nv: f32,
    tw: Vec<C>,
    y: Vec<[C; NC]>,
}

fn dft_taps(h: &[C; NC], tw: &[C]) -> [C; NC] {
    // g[t] = (1/48) sum_i h[i] exp(+j 2 pi i t / 48)
    let mut g = [C::ZERO; NC];
    for t in 0..NC {
        let mut acc = C::ZERO;
        for i in 0..NC { acc += h[i] * tw[(i * t) % NC]; }
        g[t] = acc.scale(1.0 / NC as f32);
    }
    g
}
fn taps_to_h(g: &[C; NC], tw: &[C]) -> [C; NC] {
    // h[i] = sum_t g[t] exp(-j 2 pi i t / 48)
    let mut h = [C::ZERO; NC];
    for i in 0..NC {
        let mut acc = C::ZERO;
        for t in 0..NC { acc += g[t] * tw[(i * t) % NC].conj(); }
        h[i] = acc;
    }
    h
}

impl Eq {
    /// `rx[0]` = first sample of the first cyclic prefix; `noise_power` = noise power per sample.
    pub fn new(p: &Profile, rx: &[C], noise_power: f32) -> Eq {
        let tw = twiddles();
        let nsym = p.block_symbols();
        let mut y = Vec::with_capacity(nsym);
        for s in 0..nsym {
            let a = s * SYM + WINDOW_START;
            let mut buf = [C::ZERO; NC];
            for n in 0..NC {
                if a + n < rx.len() { buf[n] = rx[a + n]; }
            }
            y.push(dft(&buf, &tw));
        }
        Eq { nsym, bps: bps_of(p), nv: noise_power.max(1e-12), tw, y }
    }

    /// Channel estimate from the pilots only: LS at the 12 pilots, triangular smoothing over +-3 symbols, interpolation through 12 time-domain taps.
    pub fn pilot_h(&self) -> Vec<[C; NC]> {
        let nsym = self.nsym;
        let mut hp = vec![[C::ZERO; 12]; nsym];
        for s in 0..nsym {
            for q in 0..12 { hp[s][q] = self.y[s][q * 4].scale(pilot(s, q * 4)); }
        }
        let wts = [1f32, 2.0, 3.0, 4.0, 3.0, 2.0, 1.0];
        let mut out = vec![[C::ZERO; NC]; nsym];
        for s in 0..nsym {
            let mut hs = [C::ZERO; 12];
            let mut wsum = 0f32;
            for (j, w) in wts.iter().enumerate() {
                let ss = s as i64 + j as i64 - 3;
                if ss < 0 || ss >= nsym as i64 { continue; }
                wsum += w;
                for q in 0..12 { hs[q] += hp[ss as usize][q].scale(*w); }
            }
            for q in 0..12 { hs[q] = hs[q].scale(1.0 / wsum); }
            let mut g = [C::ZERO; 12];
            for t in 0..12 {
                let mut acc = C::ZERO;
                for q in 0..12 { acc += hs[q] * self.tw[(q * t) % 12 * 4 % NC]; }
                g[t] = acc.scale(1.0 / 12.0);
            }
            for i in 0..NC {
                let mut acc = C::ZERO;
                for t in 0..12 {
                    let delay = TAP_LO + t as i32;
                    let tt = (delay.rem_euclid(12)) as usize;
                    let k = ((i as i64 * delay as i64).rem_euclid(NC as i64)) as usize;
                    acc += g[tt] * self.tw[k].conj();
                }
                out[s][i] = acc;
            }
        }
        out
    }

    /// Soft demapping with channel estimate `h`; n LLRs of the data carriers in transmitted order (BPSK: 1 per carrier, QPSK: 2).
    pub fn llrs(&self, h: &[[C; NC]]) -> Vec<f32> {
        let mut llr = Vec::with_capacity(self.nsym * NDATA * self.bps);
        let nv = self.nv;
        for s in 0..self.nsym {
            for i in 0..NC {
                if is_pilot(i) { continue; }
                let hy = h[s][i].conj() * self.y[s][i];
                if self.bps == 1 {
                    llr.push(4.0 * hy.re / nv);
                } else {
                    let k = 2.0 * core::f32::consts::SQRT_2 / nv;
                    llr.push(k * hy.re);
                    llr.push(k * hy.im);
                }
            }
        }
        llr
    }

    /// Decision-directed refinement: `post` = posterior LLRs of the data bits (transmitted order). Every carrier then acts as a soft pilot, weighted by the
    /// reliability of its symbol (an EM style update that keeps the old estimate where the symbol is unknown); the estimate is denoised in the delay
    /// domain (taps below 3x the median tap power, and their neighbours, are dropped) and smoothed over +-4 symbols.
    pub fn refine(&self, post: &[f32], h_old: &[[C; NC]]) -> Vec<[C; NC]> {
        let nsym = self.nsym;
        let q = core::f32::consts::FRAC_1_SQRT_2;
        let mut gs: Vec<[C; NC]> = Vec::with_capacity(nsym);
        let mut bi = 0usize;
        for s in 0..nsym {
            let mut e = [C::ZERO; NC];
            for i in 0..NC {
                let (x, w) = if is_pilot(i) {
                    (C::new(pilot(s, i), 0.0), 1.0f32)
                } else if self.bps == 1 {
                    let l = if bi < post.len() { post[bi] } else { 0.0 };
                    bi += 1;
                    let t = mathf::exp(-(l.abs() as f64)) as f32;
                    let m = (1.0 - t) / (1.0 + t);
                    (C::new(if l >= 0.0 { m } else { -m }, 0.0), m * m)
                } else {
                    let l0 = if bi < post.len() { post[bi] } else { 0.0 };
                    let l1 = if bi + 1 < post.len() { post[bi + 1] } else { 0.0 };
                    bi += 2;
                    let t0 = mathf::exp(-(l0.abs() as f64)) as f32;
                    let t1 = mathf::exp(-(l1.abs() as f64)) as f32;
                    let m0 = (1.0 - t0) / (1.0 + t0);
                    let m1 = (1.0 - t1) / (1.0 + t1);
                    let xr = (if l0 >= 0.0 { m0 } else { -m0 }) * q;
                    let xi = (if l1 >= 0.0 { m1 } else { -m1 }) * q;
                    (C::new(xr, xi), 0.5 * (m0 * m0 + m1 * m1))
                };
                e[i] = self.y[s][i] * x.conj() + h_old[s][i].scale(1.0 - w);
            }
            gs.push(dft_taps(&e, &self.tw));
        }
        // tap power over the block, noise floor = median
        let mut pdp = [0f32; NC];
        for g in &gs { for t in 0..NC { pdp[t] += g[t].norm_sqr(); } }
        let mut srt = pdp;
        srt.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
        let floor = srt[NC / 2].max(1e-20);
        let mut keep = [false; NC];
        for t in 0..NC {
            if pdp[t] > 3.0 * floor {
                keep[t] = true;
                keep[(t + 1) % NC] = true;
                keep[(t + NC - 1) % NC] = true;
            }
        }
        let wts = [1f32, 2.0, 3.0, 4.0, 5.0, 4.0, 3.0, 2.0, 1.0];
        let mut out = vec![[C::ZERO; NC]; nsym];
        for s in 0..nsym {
            let mut g = [C::ZERO; NC];
            let mut wsum = 0f32;
            for (j, w) in wts.iter().enumerate() {
                let ss = s as i64 + j as i64 - 4;
                if ss < 0 || ss >= nsym as i64 { continue; }
                wsum += w;
                for t in 0..NC { if keep[t] { g[t] += gs[ss as usize][t].scale(*w); } }
            }
            for t in 0..NC { g[t] = g[t].scale(1.0 / wsum); }
            out[s] = taps_to_h(&g, &self.tw);
        }
        out
    }
}

/// One pass (pilots only) for callers that do not iterate.
pub fn demod_block(p: &Profile, rx: &[C], noise_power: f32) -> Vec<f32> {
    let eq = Eq::new(p, rx, noise_power);
    let h = eq.pilot_h();
    let mut l = eq.llrs(&h);
    l.truncate(p.n);
    l
}
