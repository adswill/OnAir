// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Conversion between the 2400 samples/s complex baseband and 12 kHz real audio (the sound card / WAV rate): audio frequency = 1500 Hz + baseband
//! frequency, so the 2.4 kHz signal sits at 300 .. 2700 Hz. Interpolation / decimation by 5 with a 127-tap windowed-sinc low-pass (cut-off 1250 Hz).
use crate::cplx::C;
use crate::mathf;
use alloc::vec;
use alloc::vec::Vec;

pub const AUDIO_RATE: usize = 12000;
const UP: usize = 5;
const NT: usize = 127;

fn lowpass() -> Vec<f32> {
    let fc = 1250.0 / AUDIO_RATE as f64; // cycles per sample
    let mid = (NT / 2) as f64;
    (0..NT)
        .map(|i| {
            let x = i as f64 - mid;
            let s = if x == 0.0 { 2.0 * fc } else { mathf::sin(2.0 * mathf::PI * fc * x) / (mathf::PI * x) };
            let w = 0.42 - 0.5 * mathf::cos(2.0 * mathf::PI * i as f64 / (NT - 1) as f64) + 0.08 * mathf::cos(4.0 * mathf::PI * i as f64 / (NT - 1) as f64);
            (s * w) as f32
        })
        .collect()
}

/// Baseband (2400/s) to real audio (12 kHz), scaled so the largest sample is `peak` (0 < peak <= 1). Output length 5 x input.
pub fn to_audio(x: &[C], peak: f32) -> Vec<f32> {
    let h = lowpass();
    let n_out = x.len() * UP;
    let mut up = vec![C::ZERO; n_out];
    for (i, v) in x.iter().enumerate() { up[i * UP] = v.scale(UP as f32); }
    let mid = NT / 2;
    let mut out = vec![0f32; n_out];
    for n in 0..n_out {
        let mut acc = C::ZERO;
        // filtered[n] = sum_j h[j] up[n + mid - j]
        let lo = (n + mid).saturating_sub(NT - 1);
        let hi = core::cmp::min(n + mid, n_out - 1);
        for m in lo..=hi {
            let v = up[m];
            if v.re == 0.0 && v.im == 0.0 { continue; }
            acc += v.scale(h[n + mid - m]);
        }
        let ph = 2.0 * mathf::PI * 1500.0 * n as f64 / AUDIO_RATE as f64;
        out[n] = acc.re * mathf::cos(ph) as f32 - acc.im * mathf::sin(ph) as f32;
    }
    let mut mx = 0f32;
    for v in &out { mx = mx.max(v.abs()); }
    if mx > 0.0 {
        let g = peak / mx;
        for v in out.iter_mut() { *v *= g; }
    }
    out
}

/// Real audio (12 kHz) to baseband (2400/s). The scale is arbitrary (the receiver estimates the noise itself); the output has len/5 samples.
pub fn from_audio(a: &[f32]) -> Vec<C> {
    let h = lowpass();
    let mid = NT / 2;
    let n_out = a.len() / UP;
    // mix down
    let mixed: Vec<C> = (0..a.len())
        .map(|n| {
            let ph = 2.0 * mathf::PI * 1500.0 * n as f64 / AUDIO_RATE as f64;
            C::new(a[n] * mathf::cos(ph) as f32, -a[n] * mathf::sin(ph) as f32).scale(2.0)
        })
        .collect();
    let mut out = Vec::with_capacity(n_out);
    for k in 0..n_out {
        let n = k * UP;
        let mut acc = C::ZERO;
        let lo = (n + mid).saturating_sub(NT - 1);
        let hi = core::cmp::min(n + mid, mixed.len() - 1);
        for m in lo..=hi { acc += mixed[m].scale(h[n + mid - m]); }
        out.push(acc);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tone_roundtrip() {
        // baseband tone at +300 Hz -> audio 1800 Hz -> back
        let x: Vec<C> = (0..4800).map(|n| C::polar(1.0, 2.0 * mathf::PI * 300.0 * n as f64 / 2400.0)).collect();
        let a = to_audio(&x, 0.9);
        let y = from_audio(&a);
        let mut num = C::ZERO;
        for i in 600..4200 { num += y[i] * x[i].conj(); }
        let mut den = 0f32;
        for i in 600..4200 { den += y[i].norm_sqr(); }
        let c = num.norm_sqr() / (den * 3600.0);
        assert!(c > 0.99, "{c}");
    }
}
