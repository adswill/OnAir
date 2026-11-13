// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Watterson HF channel simulator (spec section 4.1) on complex baseband at 2400 samples/s: two independent Rayleigh paths of equal average power with a
//! Gaussian Doppler spectrum on each (Doppler spread = 2 sigma), the second path delayed (fractional delay by a windowed-sinc interpolator), plus a
//! frequency offset with an optional linear drift, a start delay (clock error), an optional in-band interferer and complex white Gaussian noise.
//! SNR convention (spec): signal power / (N0 x 2400 Hz); with one complex sample per 1/2400 s that is the mean signal power per sample over the noise
//! power per sample, the signal power being the mean power of the transmitted burst (the average channel power gain is 1).
use crate::cplx::C;
use crate::mathf;
use crate::rng::Rng;
use alloc::vec;
use alloc::vec::Vec;

pub const FS: f64 = 2400.0;

#[derive(Clone, Copy, Debug)]
pub struct Spec {
    pub name: &'static str,
    pub paths: usize,
    pub delay_s: f64,
    pub doppler_2sigma_hz: f64,
}
pub const AWGN: Spec = Spec { name: "KW-AWGN", paths: 1, delay_s: 0.0, doppler_2sigma_hz: 0.0 };
pub const MOD: Spec = Spec { name: "KW-MOD", paths: 2, delay_s: 0.002, doppler_2sigma_hz: 1.0 };
pub const POOR: Spec = Spec { name: "KW-POOR", paths: 2, delay_s: 0.005, doppler_2sigma_hz: 2.0 };

#[derive(Clone, Copy, Debug)]
pub struct Opts {
    pub spec: Spec,
    /// SNR in 2.4 kHz, dB.
    pub snr_db: f64,
    pub freq_offset_hz: f64,
    /// Linear frequency drift in Hz/s.
    pub drift_hz_s: f64,
    /// Start delay in samples (may be fractional); the output has this many zero samples (noise only) before the burst.
    pub start_delay: f64,
    /// Extra noise-only samples after the burst.
    pub tail: usize,
    /// In-band interferer power relative to the signal (dB), None = off.
    pub interferer_db: Option<f64>,
}
impl Opts {
    pub fn new(spec: Spec, snr_db: f64) -> Opts {
        Opts { spec, snr_db, freq_offset_hz: 0.0, drift_hz_s: 0.0, start_delay: 0.0, tail: 0, interferer_db: None }
    }
}

const TAPS: usize = 16;
/// Windowed-sinc fractional delay: returns x(n - d) for every n of the output of length `out_len` (x is zero outside its range). d may be any real >= 0.
pub fn delay(x: &[C], d: f64, out_len: usize) -> Vec<C> {
    let di = mathf::floor(d) as i64;
    let f = d - di as f64;
    let mut h = [0f32; TAPS];
    // output[n] = sum_j x[n - di - j + TAPS/2 - 1 ... ] : taps cover offsets k = -(TAPS/2 - 1) .. TAPS/2 relative to n - di
    let half = TAPS as i64 / 2;
    for t in 0..TAPS {
        let k = t as i64 - (half - 1); // sample x[n - di - k]
        let arg = k as f64 - f; // x(n - d) = sum_k x[n - di - k] sinc(k - f)
        let s = if arg.abs() < 1e-12 { 1.0 } else { mathf::sin(mathf::PI * arg) / (mathf::PI * arg) };
        let wpos = (arg / half as f64).abs().min(1.0);
        let w = 0.5 * (1.0 + mathf::cos(mathf::PI * wpos));
        h[t] = (s * w) as f32;
    }
    let mut out = vec![C::ZERO; out_len];
    for n in 0..out_len as i64 {
        let mut acc = C::ZERO;
        for t in 0..TAPS {
            let k = t as i64 - (half - 1);
            let idx = n - di - k;
            if idx >= 0 && (idx as usize) < x.len() {
                let v = x[idx as usize];
                acc.re += v.re * h[t];
                acc.im += v.im * h[t];
            }
        }
        out[n as usize] = acc;
    }
    out
}

const NSIN: usize = 32;
/// A Rayleigh tap with Gaussian Doppler spectrum (sum of NSIN sinusoids with Gaussian-distributed frequencies), evaluated every STEP samples and
/// interpolated linearly (the Doppler is a few Hz against a 2400 Hz sample rate).
struct Tap {
    f: [f64; NSIN],
    ph: [f64; NSIN],
}
impl Tap {
    fn new(sigma_hz: f64, rng: &mut Rng) -> Tap {
        let mut f = [0f64; NSIN];
        let mut ph = [0f64; NSIN];
        for i in 0..NSIN {
            f[i] = sigma_hz * rng.gauss();
            ph[i] = 2.0 * mathf::PI * rng.uniform();
        }
        Tap { f, ph }
    }
    fn at(&self, t: f64) -> C {
        let (mut re, mut im) = (0f64, 0f64);
        for i in 0..NSIN {
            let a = 2.0 * mathf::PI * self.f[i] * t + self.ph[i];
            re += mathf::cos(a);
            im += mathf::sin(a);
        }
        let s = 1.0 / mathf::sqrt(NSIN as f64);
        C::new((re * s) as f32, (im * s) as f32)
    }
}
const STEP: usize = 24;

/// One path gain per sample for `n` samples.
fn gains(n: usize, sigma_hz: f64, rng: &mut Rng) -> Vec<C> {
    let tap = Tap::new(sigma_hz, rng);
    let nk = n / STEP + 2;
    let ks: Vec<C> = (0..nk).map(|k| tap.at(k as f64 * STEP as f64 / FS)).collect();
    let mut g = Vec::with_capacity(n);
    for i in 0..n {
        let k = i / STEP;
        let u = (i % STEP) as f32 / STEP as f32;
        let a = ks[k];
        let b = ks[k + 1];
        g.push(C::new(a.re + (b.re - a.re) * u, a.im + (b.im - a.im) * u));
    }
    g
}

/// Pass the burst `x` through the channel. The burst power is its own mean power (the simulator scales nothing); noise is set from `snr_db`.
/// Returns start_delay + len + tail (+ the interpolator margin) samples.
pub fn apply(x: &[C], o: &Opts, rng: &mut Rng) -> Vec<C> {
    let sig_pow = crate::cplx::mean_power(x).max(1e-12) as f64;
    let extra = (o.start_delay + o.spec.delay_s * FS) as usize + TAPS + 2;
    let n_out = x.len() + extra + o.tail;
    // main path
    let mut y = delay(x, o.start_delay, n_out);
    if o.spec.doppler_2sigma_hz > 0.0 || o.spec.paths > 1 {
        let sigma = o.spec.doppler_2sigma_hz / 2.0;
        let amp = if o.spec.paths > 1 { 1.0 / mathf::sqrt(2.0) } else { 1.0 } as f32;
        let g0 = if sigma > 0.0 { gains(n_out, sigma, rng) } else { vec![C::new(1.0, 0.0); n_out] };
        let mut z = vec![C::ZERO; n_out];
        for i in 0..n_out { z[i] = (y[i] * g0[i]).scale(amp); }
        if o.spec.paths > 1 {
            let y2 = delay(x, o.start_delay + o.spec.delay_s * FS, n_out);
            let g1 = if sigma > 0.0 { gains(n_out, sigma, rng) } else { vec![C::new(1.0, 0.0); n_out] };
            for i in 0..n_out { z[i] += (y2[i] * g1[i]).scale(amp); }
        }
        y = z;
    }
    if o.freq_offset_hz != 0.0 || o.drift_hz_s != 0.0 {
        for i in 0..n_out {
            let t = i as f64 / FS;
            let ph = 2.0 * mathf::PI * (o.freq_offset_hz * t + 0.5 * o.drift_hz_s * t * t);
            y[i] = y[i] * C::polar(1.0, ph);
        }
    }
    let nvar = sig_pow / mathf::pow10(o.snr_db / 10.0);
    let sd = mathf::sqrt(nvar / 2.0);
    if let Some(db) = o.interferer_db {
        let amp = mathf::sqrt(sig_pow * mathf::pow10(db / 10.0)) as f32;
        let f = rng.range(-1000.0, 1000.0);
        let p0 = 2.0 * mathf::PI * rng.uniform();
        for i in 0..n_out {
            y[i] += C::polar(amp, p0 + 2.0 * mathf::PI * f * i as f64 / FS);
        }
    }
    for v in y.iter_mut() {
        v.re += (sd * rng.gauss()) as f32;
        v.im += (sd * rng.gauss()) as f32;
    }
    y
}

/// Noise only, `n` samples, with the given per-sample power.
pub fn noise(n: usize, power: f64, rng: &mut Rng) -> Vec<C> {
    let sd = mathf::sqrt(power / 2.0);
    (0..n).map(|_| C::new((sd * rng.gauss()) as f32, (sd * rng.gauss()) as f32)).collect()
}
