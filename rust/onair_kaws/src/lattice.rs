// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The woven lattice (spec section 7): the preamble unit (4 rising + 4 falling chirps of 1 s over 1600 Hz and a 7-tone zig-zag comb, 3.5 s), the closing
//! weave (1 s) and the detector. Signals are complex baseband at 2400/s (audio frequency = 1500 Hz + baseband frequency), every unit scaled to mean power 1
//! so it sits at the level of the constant-envelope data. Element edges carry a 4 ms raised-cosine ramp (not in the spec text, which has hard edges).
use crate::cplx::C;
use crate::fft::{next_pow2, Fft};
use crate::mathf;
use alloc::vec;
use alloc::vec::Vec;

pub const FS: f64 = 2400.0;
pub const UNIT_LEN: usize = 8400; // 3.5 s
pub const CLOSING_LEN: usize = 2400; // 1.0 s
pub const SLOPE: f64 = 1600.0; // Hz/s of the lattice chirps
const RAMP: usize = 10; // samples (4.2 ms)
const CENTER: f64 = 1500.0;

/// Adds amp * exp(j phase(t)) over [start, start+len) samples; the audio frequency f(t') (Hz) is a linear sweep f0 + slope t' (t' in s).
fn add_sweep(buf: &mut [C], start: f64, dur: f64, f0_audio: f64, slope: f64) { add_sweep_ph(buf, start, dur, f0_audio, slope, 0.0) }

fn add_sweep_ph(buf: &mut [C], start: f64, dur: f64, f0_audio: f64, slope: f64, ph0: f64) {
    let s0 = mathf::floor(start * FS + 0.5) as usize;
    let n = mathf::floor(dur * FS + 0.5) as usize;
    for i in 0..n {
        if s0 + i >= buf.len() { break; }
        let t = i as f64 / FS;
        let ph = ph0 + 2.0 * mathf::PI * ((f0_audio - CENTER) * t + 0.5 * slope * t * t);
        let mut a = 1.0f32;
        if i < RAMP { a = 0.5 * (1.0 - mathf::cos(mathf::PI * i as f64 / RAMP as f64) as f32); }
        if n - 1 - i < RAMP { a = 0.5 * (1.0 - mathf::cos(mathf::PI * (n - 1 - i) as f64 / RAMP as f64) as f32); }
        buf[s0 + i] += C::polar(a, ph);
    }
}

fn normalise(buf: &mut [C]) {
    let p = crate::cplx::mean_power(buf).max(1e-12);
    let g = 1.0 / mathf::sqrt(p as f64) as f32;
    for v in buf.iter_mut() { *v = v.scale(g); }
}

/// One lattice unit: mean power 1.
/// Start phase (radians) of the 15 elements: 4 rising, 4 falling, 7 comb tones. Not given in the spec text (zero gives a 8.91 dB peak to mean ratio); chosen by `phase_search` (tests/papr_search.rs) to lower the peak to mean ratio.
pub static PHASES: [f64; 15] = [1.91, 2.055, 0.02, 2.763, 2.784, 1.683, 2.367, 4.943, 3.946, 1.721, 1.005, 0.368, 1.186, 5.295, 0.29];

pub fn unit() -> Vec<C> { unit_with(&PHASES) }

pub fn unit_with(ph: &[f64; 15]) -> Vec<C> {
    let mut b = vec![C::ZERO; UNIT_LEN];
    for k in 0..4 {
        add_sweep_ph(&mut b, 0.5 * k as f64, 1.0, 300.0 + 266.666_666_7 * k as f64, SLOPE, ph[k]);
    }
    for j in 0..4 {
        add_sweep_ph(&mut b, 0.25 + 0.5 * j as f64, 1.0, 2700.0 - 266.666_666_7 * j as f64, -SLOPE, ph[4 + j]);
    }
    let pi_ = [0, 3, 6, 2, 5, 1, 4];
    for n in 0..7 {
        let f = 450.0 + 350.0 * pi_[n] as f64;
        add_sweep_ph(&mut b, 0.35 + 0.4 * n as f64, 0.4, f, 0.0, ph[8 + n]);
    }
    normalise(&mut b);
    b
}

/// The closing weave, 1 s long, mean power 1 over the whole second (the last 0.125 s is empty).
pub fn closing() -> Vec<C> {
    let mut b = vec![C::ZERO; CLOSING_LEN];
    for i in 0..2 {
        add_sweep(&mut b, 0.25 * i as f64, 0.5, 300.0 + 500.0 * i as f64, 3200.0);
        add_sweep(&mut b, 0.125 + 0.25 * i as f64, 0.5, 2700.0 - 500.0 * i as f64, -3200.0);
    }
    normalise(&mut b);
    b
}

/// Peak to mean power ratio in dB of a block.
pub fn papr_db(x: &[C]) -> f64 {
    let mean = crate::cplx::mean_power(x).max(1e-12) as f64;
    let mut pk = 0f64;
    for v in x { pk = pk.max(v.norm_sqr() as f64); }
    10.0 * mathf::log10(pk / mean)
}

fn element_refs() -> Vec<Vec<C>> {
    // 8 unit-amplitude references of 1 s: rising k = 0..3 then falling j = 0..3
    let mut v = Vec::new();
    for k in 0..4 {
        let mut b = vec![C::ZERO; 2400];
        add_sweep(&mut b, 0.0, 1.0, 300.0 + 266.666_666_7 * k as f64, SLOPE);
        v.push(b);
    }
    for j in 0..4 {
        let mut b = vec![C::ZERO; 2400];
        add_sweep(&mut b, 0.0, 1.0, 2700.0 - 266.666_666_7 * j as f64, -SLOPE);
        v.push(b);
    }
    v
}

#[derive(Clone, Debug, Default)]
pub struct Detection {
    /// Sample index (in the searched stream, fractional) where the first unit starts.
    pub tau: f64,
    /// Normalised detection statistic of the best hypothesis (roughly a deflection in standard deviations).
    pub stat: f64,
    /// Frequency offset in Hz per unit (index = unit), from the rising/falling peak spacing.
    pub delta_hz: Vec<f64>,
    /// Timing per unit (sample index of the unit start, fractional).
    pub unit_tau: Vec<f64>,
    /// Noise power per sample estimated from the stream.
    pub noise_power: f32,
    /// Relative power of the propagation paths at delays -PDP_EL ..= PDP_EH samples around the strongest one (index e + PDP_EL), 0 where the lattice shows no path.
    pub pdp: Vec<f32>,
    /// Diagnostics: median and robust spread of the per-position statistic over the searched window.
    pub t_med: f32,
    pub t_sig: f32,
}

/// Median and spread of the statistic T[x] = max_s A[x-s] + B[x+s] in noise only (noise power known), 8 correlators of unit mean.
pub const T_MED: f32 = 18.43;
pub const T_SIG: f32 = 1.82;
pub const PDP_EL: usize = 20;
pub const PDP_EH: usize = 20;

/// Path weights from the rising and falling correlation sums at the unit peaks (noise mean 8 per unit, standard deviation sqrt(8 per unit)).
fn path_weights(a: &[f32], b: &[f32], peaks: &[(i64, i64)]) -> Vec<f32> {
    let n = PDP_EL + PDP_EH + 1;
    let mut raw = vec![0f32; n];
    let nu = peaks.len().max(1) as f32;
    for (i, r) in raw.iter_mut().enumerate() {
        let e = i as i64 - PDP_EL as i64;
        for &(pa, pb) in peaks {
            let (ia, ib) = (pa + e, pb + e);
            if ia >= 0 && (ia as usize) < a.len() { *r += a[ia as usize]; }
            if ib >= 0 && (ib as usize) < b.len() { *r += b[ib as usize]; }
        }
        *r -= 8.0 * nu;
    }
    let sigma = mathf::sqrt(8.0 * nu as f64) as f32;
    let main = raw[PDP_EL];
    let mut w = vec![0f32; n];
    w[PDP_EL] = 1.0;
    if main <= 3.0 * sigma { return w; }
    for i in 0..n {
        let e = i as i64 - PDP_EL as i64;
        if e == 0 || e.abs() == 1 { continue; }
        if raw[i] > 3.0 * sigma { w[i] = (raw[i] / main).min(1.0); }
    }
    w
}

fn parabolic(y0: f32, y1: f32, y2: f32) -> f64 {
    let d = y0 - 2.0 * y1 + y2;
    if d.abs() < 1e-20 { return 0.0; }
    let p = 0.5 * (y0 - y2) / d;
    (p as f64).clamp(-0.5, 0.5)
}

/// Detect a burst whose lattice units start `offsets[u]` samples after the first one (offsets[0] = 0). The first unit starts somewhere in
/// `rx[search_lo ..= search_hi]` (a frequency offset of +-100 Hz and any timing error inside that range are covered). Returns the best hypothesis
/// and its statistic; the caller compares `stat` with the threshold (see `DETECT_THRESHOLD`).
pub fn detect(rx: &[C], offsets: &[usize], search_lo: usize, search_hi: usize) -> Detection {
    let refs = element_refs();
    let nu = offsets.len().max(1);
    let last = *offsets.last().unwrap_or(&0);
    let need = search_hi + last + UNIT_LEN + 400;
    let len = rx.len().min(need);
    if len < 9000 || search_hi < search_lo { return Detection::default(); }
    let x = &rx[..len];
    let nfft = next_pow2(len + 2400);
    let f = Fft::new(nfft);
    let mut xf = vec![C::ZERO; nfft];
    xf[..len].copy_from_slice(x);
    f.forward(&mut xf);
    // noise estimate: the 15th percentile of |x|^2 (an exponential variable: p15 = -ln(0.85) x mean) on a decimated subset, so that a stream that is mostly signal
    // (strong or short bursts) still gives the noise level as long as 15 percent of it is noise only
    let mut pw: Vec<f32> = x.iter().step_by(7).map(|v| v.norm_sqr()).collect();
    pw.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
    let noise = (pw[pw.len() * 15 / 100] as f64 / 0.162_518_9).max(1e-20) as f32;
    let e_ref: f32 = refs[0].iter().map(|v| v.norm_sqr()).sum();
    let norm = 1.0 / (noise * e_ref);
    let m = len - 2400 + 1;
    let mut corr: Vec<Vec<f32>> = Vec::with_capacity(8);
    for r in refs.iter() {
        let mut rf = vec![C::ZERO; nfft];
        rf[..2400].copy_from_slice(r);
        f.forward(&mut rf);
        for i in 0..nfft { rf[i] = xf[i] * rf[i].conj(); }
        f.inverse(&mut rf);
        let inv = 1.0 / nfft as f32;
        corr.push((0..m).map(|t| rf[t].norm_sqr() * inv * inv * norm).collect());
    }
    // A[p] = sum_k corrR_k[p + 1200 k];  B[q] = sum_j corrF_j[q + 600 + 1200 j]
    let alen = m.saturating_sub(3600 + 600);
    let mut a = vec![0f32; alen];
    let mut b = vec![0f32; alen];
    for p in 0..alen {
        a[p] = corr[0][p] + corr[1][p + 1200] + corr[2][p + 2400] + corr[3][p + 3600];
        b[p] = corr[4][p + 600] + corr[5][p + 1800] + corr[6][p + 3000] + corr[7][p + 4200];
    }
    // T[x] = max_s A[x - s] + B[x + s], s in [-SMAX, SMAX] (|delta| <= 100 Hz -> 150 samples; 160 allows a little margin)
    const SMAX: i64 = 160;
    let tlo = search_lo as i64;
    let thi = (search_hi + last) as i64;
    let mut tmax: Vec<f32> = vec![0.0; (thi - tlo + 1).max(0) as usize];
    let mut targ: Vec<i16> = vec![0; tmax.len()];
    for (ix, xv) in (tlo..=thi).enumerate() {
        let mut best = f32::MIN;
        let mut bs = 0i64;
        for s in -SMAX..=SMAX {
            let pa = xv - s;
            let pb = xv + s;
            if pa < 0 || pb < 0 || pa as usize >= alen || pb as usize >= alen { continue; }
            let v = a[pa as usize] + b[pb as usize];
            if v > best { best = v; bs = s; }
        }
        tmax[ix] = if best == f32::MIN { 0.0 } else { best };
        targ[ix] = bs as i16;
    }
    // robust normalisation of T (mean and spread over the window)
    let mut srt: Vec<f32> = tmax.iter().step_by(3).cloned().collect();
    srt.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
    let _ = &srt;
    // noise-only median and spread of the per-position statistic (measured, tests/thresholds.rs noise_consts): the window itself cannot be used when most of it is signal
    let med = T_MED;
    let sig = T_SIG;
    // total over the units for each candidate tau
    let mut best_stat = f64::MIN;
    let mut best_tau = search_lo;
    for t in search_lo..=search_hi {
        let mut sum = 0f32;
        for &o in offsets.iter().take(nu) {
            let ix = (t + o) as i64 - tlo;
            if ix >= 0 && (ix as usize) < tmax.len() { sum += tmax[ix as usize]; }
        }
        let stat = (sum - med * nu as f32) as f64 / (sig as f64 * mathf::sqrt(nu as f64));
        if stat > best_stat { best_stat = stat; best_tau = t; }
    }
    // refinement per unit
    let mut peaks: Vec<(i64, i64)> = Vec::new();
    let mut det = Detection { tau: best_tau as f64, stat: best_stat, noise_power: noise, t_med: med, t_sig: sig, ..Default::default() };
    for &o in offsets.iter().take(nu) {
        let ix = (best_tau + o) as i64 - tlo;
        let s0 = if ix >= 0 && (ix as usize) < targ.len() { targ[ix as usize] as i64 } else { 0 };
        let xc = (best_tau + o) as i64;
        // local peaks of A near xc - s0 and B near xc + s0 (search +-4 samples)
        let find = |arr: &[f32], c: i64| -> (f64, i64) {
            let mut bp = c;
            let mut bv = f32::MIN;
            for p in (c - 4)..=(c + 4) {
                if p >= 1 && (p as usize) + 1 < arr.len() && arr[p as usize] > bv { bv = arr[p as usize]; bp = p; }
            }
            if bp >= 1 && (bp as usize) + 1 < arr.len() {
                let (y0, y1, y2) = (arr[bp as usize - 1].max(0.0), arr[bp as usize], arr[bp as usize + 1].max(0.0));
                (bp as f64 + parabolic(mathf::sqrt(y0 as f64) as f32, mathf::sqrt(y1.max(0.0) as f64) as f32, mathf::sqrt(y2 as f64) as f32), bp)
            } else {
                (c as f64, c)
            }
        };
        let (p, pi) = find(&a, xc - s0);
        let (q, qi) = find(&b, xc + s0);
        peaks.push((pi, qi));
        let ut = 0.5 * (p + q) - o as f64;
        let s = 0.5 * (q - p);
        det.unit_tau.push(ut);
        det.delta_hz.push(s / FS * SLOPE);
    }
    det.pdp = path_weights(&a, &b, &peaks);
    // With more than one propagation path the rising and the falling sums may peak on different paths (a pairing error of half the delay spread gives a timing
    // and frequency error); the power centroids of the two sums see the same channel, so they are used instead of the peaks.
    let multipath = det.pdp.iter().enumerate().any(|(i, w)| *w > 0.0 && i != PDP_EL);
    if multipath {
        let centroid = |arr: &[f32], c: i64| -> f64 {
            let (mut sw, mut swi) = (0f64, 0f64);
            for i in (c - 6)..=(c + PDP_EH as i64) {
                if i < 0 || i as usize >= arr.len() { continue; }
                let v = arr[i as usize] as f64 - 4.0;
                if v > 5.0 { sw += v; swi += v * i as f64; }
            }
            if sw > 0.0 { swi / sw } else { c as f64 }
        };
        for (u, &o) in offsets.iter().take(nu).enumerate() {
            let (pi, qi) = peaks[u];
            let (pc, qc) = (centroid(&a, pi), centroid(&b, qi));
            det.unit_tau[u] = 0.5 * (pc + qc) - o as f64;
            det.delta_hz[u] = 0.5 * (qc - pc) / FS * SLOPE;
            peaks[u] = (mathf::floor(pc + 0.5) as i64, mathf::floor(qc + 0.5) as i64);
        }
        det.pdp = path_weights(&a, &b, &peaks);
    }
    // the first unit's timing, averaged with the rest (they sit at known spacing)
    let mut tau_mean = 0f64;
    for t in &det.unit_tau { tau_mean += *t; }
    det.tau = tau_mean / det.unit_tau.len().max(1) as f64;
    det
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unit_properties() {
        let u = unit();
        assert_eq!(u.len(), UNIT_LEN);
        let p = crate::cplx::mean_power(&u);
        assert!((p - 1.0).abs() < 1e-3);
        std::println!("unit PAPR {:.2} dB, closing PAPR {:.2} dB", papr_db(&u), papr_db(&closing()));
    }
}

/// Estimate the timing and frequency offset from the closing weave (four 0.5 s chirps at slope 3200 Hz/s, spec 7.5): `expected` = sample index of its nominal start
/// in `rx`, `noise` = noise power per sample. Searches +-40 samples and +-133 Hz. Returns (start sample, offset Hz, statistic) when the peak is clearly above noise.
pub fn closing_estimate(rx: &[C], expected: f64, noise: f32) -> Option<(f64, f64, f32)> {
    const SM: i64 = 100;
    const TM: i64 = 40;
    let mk = |f0: f64, start: f64, slope: f64| {
        let mut b = vec![C::ZERO; 1200];
        add_sweep(&mut b, start, 0.5, f0, slope);
        let _ = start;
        b
    };
    // references start at sample 0 of their own buffer; the element start offsets are applied when summing
    let refs = [mk(300.0, 0.0, 3200.0), mk(800.0, 0.0, 3200.0), mk(2700.0, 0.0, -3200.0), mk(2200.0, 0.0, -3200.0)];
    let lo = expected - (TM + SM + 8) as f64;
    if lo < 0.0 { return None; }
    let lo = lo as usize;
    let hi = (expected + (TM + SM + 8) as f64) as usize + 1200 + 900;
    if hi + 1200 > rx.len() { return None; }
    let x = &rx[lo..hi];
    let e_ref: f32 = refs[0].iter().map(|v| v.norm_sqr()).sum();
    let norm = 1.0 / (noise.max(1e-20) * e_ref);
    let cs: Vec<Vec<f32>> = refs.iter().map(|r| crate::fft::correlate(x, r).iter().map(|v| v.norm_sqr() * norm).collect()).collect();
    let n = cs[0].len();
    // closing start positions: rising 0 and 600, falling 300 and 900 (samples from the closing start)
    let a = |p: i64| -> f32 { if p >= 0 && (p as usize) + 600 < n { cs[0][p as usize] + cs[1][p as usize + 600] } else { 0.0 } };
    let b = |q: i64| -> f32 { if q >= 0 && (q as usize) + 900 < n { cs[2][q as usize + 300] + cs[3][q as usize + 900] } else { 0.0 } };
    let base = (expected as i64) - lo as i64;
    let (mut best, mut bx, mut bs) = (f32::MIN, 0i64, 0i64);
    for xv in (base - TM)..=(base + TM) {
        for s in -SM..=SM {
            let v = a(xv - s) + b(xv + s);
            if v > best { best = v; bx = xv; bs = s; }
        }
    }
    // noise-only reference: 4 exponential variables (mean 4) maximised over ~16000 hypotheses stays below about 25
    if best < 30.0 { return None; }
    let refine = |f: &dyn Fn(i64) -> f32, c: i64| -> f64 {
        let (y0, y1, y2) = (f(c - 1).max(0.0), f(c).max(0.0), f(c + 1).max(0.0));
        c as f64 + parabolic(mathf::sqrt(y0 as f64) as f32, mathf::sqrt(y1 as f64) as f32, mathf::sqrt(y2 as f64) as f32)
    };
    let p = refine(&a, bx - bs);
    let q = refine(&b, bx + bs);
    let start = lo as f64 + 0.5 * (p + q);
    let s = 0.5 * (q - p);
    Some((start, s / FS * 3200.0, best))
}
