// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Block level link: LDPC coding, the whole-block bit interleaver, chirp symbol mapping (Gray), modulation of a data section and the soft demodulator
//! for the chirp profiles P0-P4. The OFDM profiles are in `ofdm`; the burst assembly with the lattice units is at the end of this file.
use crate::chirp::{bin_llrs, gray, Modem, Weave};
use crate::cplx::C;
use crate::ldpc;
use crate::mathf;
use crate::profile::{Kind, Profile};
use crate::rng::Rng;
use alloc::vec;
use alloc::vec::Vec;

/// The bit interleaver permutation of a block of n bits: Fisher-Yates driven by the xorshift64 generator seeded with 0x4B415753_0000 + n.
pub fn permutation(n: usize) -> Vec<u32> {
    let mut p: Vec<u32> = (0..n as u32).collect();
    let mut r = Rng::new(0x4B41_5753_0000 + n as u64);
    for i in (1..n).rev() {
        let j = r.below(i as u32 + 1) as usize;
        p.swap(i, j);
    }
    p
}

/// Encode k information bits (one per byte) with the profile's LDPC code and interleave: returns the n transmitted bits.
pub fn encode_block(p: &Profile, info: &[u8]) -> Option<Vec<u8>> {
    let code = ldpc::profile_code(p.id)?;
    if info.len() != p.k { onair_common::diag_debug!(227956, "A KAWS block was given {} information bits but the profile needs {}, so it was not encoded.", info.len(), p.k); return None; }
    let mut cw = vec![0u8; p.n];
    if !code.encode(info, &mut cw) { return None; }
    let perm = permutation(p.n);
    Some((0..p.n).map(|i| cw[perm[i] as usize]).collect())
}

/// Deinterleave LLRs (received order -> codeword order) and decode. Returns (info bits, iterations or -1).
pub fn decode_block(p: &Profile, llr: &[f32]) -> Option<(Vec<u8>, i32)> {
    let code = ldpc::profile_code(p.id)?;
    if llr.len() != p.n { return None; }
    let perm = permutation(p.n);
    let mut cw = vec![0f32; p.n];
    for i in 0..p.n { cw[perm[i] as usize] = llr[i]; }
    let mut info = vec![0u8; p.k];
    let it = code.decode(&cw, 60, &mut info);
    Some((info, it))
}

/// Chirp symbol values of a coded block: sf bits per symbol (first bit = most significant), Gray mapped; the last symbol is padded with zero bits.
pub fn chirp_values(p: &Profile, coded: &[u8]) -> Vec<usize> {
    let sf = match p.kind { Kind::Chirp { sf, .. } => sf, _ => return Vec::new() };
    let nsym = p.n.div_ceil(sf);
    (0..nsym)
        .map(|s| {
            let mut b = 0u32;
            for j in 0..sf {
                let bit = if s * sf + j < coded.len() { coded[s * sf + j] & 1 } else { 0 };
                b = (b << 1) | bit as u32;
            }
            gray(b) as usize
        })
        .collect()
}

/// Modulate symbol values (rep times, the weave continuing across the repetitions) into one contiguous data section.
pub fn modulate_chirp(p: &Profile, vals: &[usize]) -> Vec<C> {
    let (sf, guard) = match p.kind { Kind::Chirp { sf, guard } => (sf, guard), _ => return Vec::new() };
    let md = Modem::new(sf, guard);
    let mut w = Weave::new();
    let mut out = Vec::with_capacity(vals.len() * p.reps * (md.m + guard));
    for _ in 0..p.reps {
        for &v in vals {
            md.symbol(v, w.next_dir(), &mut out);
        }
    }
    out
}

/// A run of data symbols that is contiguous in the received stream: first sample of its first symbol (guard included) and the symbol count.
#[derive(Clone, Copy, Debug)]
pub struct Segment {
    pub start: usize,
    pub nsym: usize,
}

/// Soft demodulator for P0-P4. `rx` is aligned (frequency already removed); the segments list where the transmitted symbols (reps x nsym, in order) are.
/// `design_snr_db` shapes the log-likelihood (Rician); returns n LLRs in transmitted (interleaved) order. The noise level is taken from the median bin energy
/// of every symbol (averaged over the block), so nothing else is needed.
pub fn demod_chirp(p: &Profile, rx: &[C], segs: &[Segment], design_snr_db: f64) -> Vec<f32> {
    let (sf, guard) = match p.kind { Kind::Chirp { sf, guard } => (sf, guard), _ => return Vec::new() };
    let md = Modem::new(sf, guard);
    let m = md.m;
    let nsym = p.block_symbols();
    let total = nsym * p.reps;
    // bin energies of every symbol
    let mut energies: Vec<Vec<f32>> = Vec::with_capacity(total);
    let mut w = Weave::new();
    let mut bins = Vec::new();
    let mut zero = vec![C::ZERO; m];
    let mut med_sum = 0f64;
    let mut med_n = 0usize;
    for seg in segs {
        for s in 0..seg.nsym {
            let a = seg.start + s * (m + guard) + guard;
            let dir = w.next_dir();
            let chunk: &[C] = if a + m <= rx.len() { &rx[a..a + m] } else {
                zero.iter_mut().for_each(|c| *c = C::ZERO);
                let avail = rx.len().saturating_sub(a);
                zero[..avail.min(m)].copy_from_slice(&rx[a.min(rx.len())..a.min(rx.len()) + avail.min(m)]);
                &zero
            };
            md.dechirp(chunk, dir, &mut bins);
            let e: Vec<f32> = bins.iter().map(|b| b.norm_sqr()).collect();
            let mut sorted = e.clone();
            sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
            med_sum += sorted[m / 2] as f64 / core::f64::consts::LN_2;
            med_n += 1;
            energies.push(e);
        }
    }
    let noise_bin = (med_sum / med_n.max(1) as f64).max(1e-30);
    let gamma = m as f64 * mathf::pow10(design_snr_db / 10.0);
    let k = 2.0 * mathf::sqrt(gamma);
    let mut llr = vec![0f32; nsym * sf];
    let mut wv = vec![0f64; m];
    let mut tmp = vec![0f32; sf];
    for s in 0..nsym {
        wv.iter_mut().for_each(|v| *v = 0.0);
        for r in 0..p.reps {
            let idx = r * nsym + s;
            if idx >= energies.len() { continue; }
            for b in 0..m {
                let z = mathf::sqrt(energies[idx][b] as f64 / noise_bin);
                wv[b] += mathf::ln_i0(k * z);
            }
        }
        bin_llrs(&wv, sf, &mut tmp);
        llr[s * sf..(s + 1) * sf].copy_from_slice(&tmp);
    }
    llr.truncate(p.n);
    llr
}

/// Contiguous layout of a data-only section (no midambles): one segment.
pub fn plain_segments(p: &Profile) -> Vec<Segment> {
    vec![Segment { start: 0, nsym: p.block_symbols() * p.reps }]
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Burst assembly and reception (chirp profiles): [lattice units at the front][block: segments with lattice midambles between][more blocks][closing weave]

use crate::lattice;

/// Where everything sits in a burst (sample indices from the burst start, 2400/s).
#[derive(Clone, Debug)]
pub struct Layout {
    pub unit_offsets: Vec<usize>,
    /// Per block: its segments (absolute start of the first symbol of a segment, symbol count).
    pub blocks: Vec<Vec<Segment>>,
    pub closing: usize,
    pub total: usize,
}

/// Number of lattice units at the front of the burst for a profile (P0 2, others 1).
pub fn front_units(p: &Profile) -> usize { if p.id == 0 { 2 } else { 1 } }

/// The burst layout of a profile for `nblocks` data blocks (chirp profiles only; midambles apply to the first block, which for P0 and P1 is the only one).
pub fn layout(p: &Profile, nblocks: usize) -> Layout {
    if let Kind::Ofdm { .. } = p.kind {
        let mut blocks = Vec::new();
        let mut t = lattice::UNIT_LEN;
        for _ in 0..nblocks {
            blocks.push(vec![Segment { start: t, nsym: p.block_symbols() }]);
            t += p.block_symbols() * crate::ofdm::SYM;
        }
        return Layout { unit_offsets: vec![0], blocks, closing: t, total: t + lattice::CLOSING_LEN };
    }
    let sym_len = p.symbol_samples();
    let total_syms = p.block_symbols() * p.reps;
    let front = front_units(p);
    let mids = p.units.saturating_sub(front);
    let nseg = mids + 1;
    let mut unit_offsets = Vec::new();
    let mut t = 0usize;
    for _ in 0..front { unit_offsets.push(t); t += lattice::UNIT_LEN; }
    let mut blocks = Vec::new();
    for b in 0..nblocks {
        let mut segs = Vec::new();
        let ns = if b == 0 { nseg } else { 1 };
        let mut done = 0usize;
        for s in 0..ns {
            if s > 0 { unit_offsets.push(t); t += lattice::UNIT_LEN; }
            let cnt = (total_syms * (s + 1)) / ns - done;
            segs.push(Segment { start: t, nsym: cnt });
            t += cnt * sym_len;
            done += cnt;
        }
        blocks.push(segs);
    }
    Layout { unit_offsets, blocks, closing: t, total: t + lattice::CLOSING_LEN }
}

/// Build the burst for `infos.len()` blocks (each k information bits). The burst has mean power ~1.
pub fn build_burst(p: &Profile, infos: &[Vec<u8>]) -> Option<Vec<C>> {
    let lay = layout(p, infos.len());
    let unit = lattice::unit();
    let mut out: Vec<C> = Vec::with_capacity(lay.total);
    let mut ui = 0usize;
    let mut sections: Vec<(usize, Vec<C>)> = Vec::new(); // (start, samples)
    for (b, info) in infos.iter().enumerate() {
        let coded = encode_block(p, info)?;
        let data = match p.kind {
            Kind::Chirp { .. } => modulate_chirp(p, &chirp_values(p, &coded)),
            Kind::Ofdm { .. } => crate::ofdm::modulate_block(p, &coded),
        };
        let sym_len = p.symbol_samples();
        let mut at = 0usize;
        for seg in &lay.blocks[b] {
            let n = seg.nsym * sym_len;
            sections.push((seg.start, data[at..at + n].to_vec()));
            at += n;
        }
    }
    let _ = &mut ui;
    for &o in &lay.unit_offsets { sections.push((o, unit.clone())); }
    sections.push((lay.closing, lattice::closing()));
    sections.sort_by_key(|s| s.0);
    for (start, v) in sections {
        debug_assert_eq!(out.len(), start);
        out.extend_from_slice(&v);
    }
    Some(out)
}

/// Shift a stream so that its sample `a` (real, >= 0) becomes sample 0.
pub fn advance(x: &[C], a: f64) -> Vec<C> {
    let i0 = mathf::floor(a) as usize;
    let f = a - i0 as f64;
    if i0 >= x.len() { return Vec::new(); }
    let xs = &x[i0..];
    if f < 1e-6 { return xs.to_vec(); }
    crate::watterson::delay(xs, -f, xs.len())
}

/// Remove a time varying frequency offset: `pts` = (sample index, offset Hz); linear in between, constant outside.
pub fn derotate(x: &mut [C], pts: &[(f64, f64)]) {
    // linear between the points, constant before the first and after the last
    let mut phase = 0f64;
    let mut j = 0usize;
    for i in 0..x.len() {
        let t = i as f64;
        while j + 1 < pts.len() && pts[j + 1].0 < t { j += 1; }
        let d = if pts.is_empty() { 0.0 } else if pts.len() == 1 || t <= pts[0].0 { pts[0].1 } else if j + 1 >= pts.len() { pts[pts.len() - 1].1 }
            else { let (t0, d0, t1, d1) = (pts[j].0, pts[j].1, pts[j + 1].0, pts[j + 1].1); d0 + (d1 - d0) * ((t - t0) / (t1 - t0)).clamp(0.0, 1.0) };
        phase += 2.0 * mathf::PI * d / lattice::FS;
        x[i] = x[i] * C::polar(1.0, -phase);
    }
}

#[derive(Clone, Debug, Default)]
pub struct RxResult {
    pub sync: bool,
    pub stat: f64,
    pub tau: f64,
    pub delta_hz: f64,
    /// SNR in 2.4 kHz estimated from the burst power against the noise outside it (dB; reliable down to about -25 dB).
    pub snr_db: f64,
    /// Frequency drift in Hz/s from the first lattice unit and the closing weave (0 when the closing weave was not found).
    pub drift_hz_s: f64,
    /// Per block: decoded information bits and the LDPC iterations (-1 = not decoded).
    pub blocks: Vec<(Vec<u8>, i32)>,
}

/// Detection threshold on `Detection::stat` (set from the noise-only measurement in KAWS-MEASURED-v0.1.md).
pub fn detect_threshold(units: usize) -> f64 {
    if units >= 3 { 9.5 } else if units == 2 { 10.5 } else { 16.0 }
}
/// Noise power per sample from the samples outside the burst when there are enough of them, else the detector's median estimate.
fn noise_estimate(rx: &[C], tau: f64, total: usize, fallback: f32) -> f32 {
    let a = (tau - 60.0).max(0.0) as usize;
    let b = ((tau + total as f64 + 60.0) as usize).min(rx.len());
    let mut s = 0f64;
    let mut n = 0usize;
    for v in rx[..a.min(rx.len())].iter().chain(rx[b..].iter()) { s += v.norm_sqr() as f64; n += 1; }
    if n >= 1500 { (s / n as f64) as f32 } else { fallback }
}

/// Receive a chirp burst: detect the lattice in `rx[search_lo..=search_hi]`, align, remove the offset, demodulate and decode every block.
pub fn rx_burst(p: &Profile, nblocks: usize, rx: &[C], search_lo: usize, search_hi: usize, design_snr_db: f64) -> RxResult {
    let lay = layout(p, nblocks);
    let det = lattice::detect(rx, &lay.unit_offsets, search_lo, search_hi);
    let mut res = RxResult { stat: det.stat, tau: det.tau, ..Default::default() };
    if det.stat < detect_threshold(lay.unit_offsets.len()) { return res; }
    res.sync = true;
    let noise = noise_estimate(rx, det.tau, lay.total, det.noise_power);
    let mut al = advance(rx, det.tau);
    {
        let from = (det.tau.max(0.0) as usize).min(rx.len());
        let end = (from + lay.total).min(rx.len());
        let pw = crate::cplx::mean_power(&rx[from..end]) as f64;
        res.snr_db = 10.0 * mathf::log10(((pw / noise as f64) - 1.0).max(1e-4));
    }
    // frequency estimate: a line through the per-unit offsets (single unit: constant), weighted equally
    let pts: Vec<(f64, f64)> = lay.unit_offsets.iter().zip(det.delta_hz.iter()).map(|(&o, &d)| (o as f64 + lattice::UNIT_LEN as f64 / 2.0, d)).collect();
    let mean_d = det.delta_hz.iter().sum::<f64>() / det.delta_hz.len().max(1) as f64;
    res.delta_hz = mean_d;
    let mut pts2: Vec<(f64, f64)> = pts;
    // the closing weave gives the offset at the end of the burst: with it the drift is a line through all points
    if let Some((_, dc, _)) = lattice::closing_estimate(rx, det.tau + lay.closing as f64, noise) {
        if (dc - mean_d).abs() < 100.0 { pts2.push((lay.closing as f64 + lattice::CLOSING_LEN as f64 / 2.0, dc)); res.drift_hz_s = (dc - det.delta_hz[0]) / ((lay.closing as f64) / lattice::FS); }
    }
    if pts2.len() < 2 { pts2 = vec![(0.0, mean_d)]; }
    derotate(&mut al, &pts2);
    for b in 0..nblocks {
        if let Kind::Ofdm { .. } = p.kind {
            let seg = lay.blocks[b][0];
            let from = seg.start.min(al.len());
            match decode_ofdm_iter(p, &al[from..], noise, OFDM_ROUNDS) {
                Some((info, it)) => res.blocks.push((info, it)),
                None => res.blocks.push((Vec::new(), -1)),
            }
            continue;
        }
        let ce = chirp_energies(p, &al, &lay.blocks[b]);
        match ce.and_then(|c| decode_chirp_em(p, &c, &det.pdp, design_snr_db, OUTER_ITER, INNER_ITER, true)) {
            Some((info, it)) => res.blocks.push((info, it)),
            None => res.blocks.push((Vec::new(), -1)),
        }
    }
    res
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Iterative demapping and decoding (bit-interleaved coded modulation with iterative decoding): the decoder's extrinsic information is fed back as a priori
// information of the symbol bits. Orthogonal signalling loses a lot when the symbol is reduced to independent bit LLRs; the feedback recovers most of it.

/// Per-symbol log-likelihoods ln I0(2 sqrt(gamma) z) of every bin (summed over the repetitions): nsym vectors of M values.
pub fn chirp_loglik(p: &Profile, rx: &[C], segs: &[Segment], design_snr_db: f64, pdp: &[f32]) -> Vec<Vec<f32>> {
    let (sf, guard) = match p.kind { Kind::Chirp { sf, guard } => (sf, guard), _ => return Vec::new() };
    let md = Modem::new(sf, guard);
    let m = md.m;
    let nsym = p.block_symbols();
    let mut dirs: Vec<i32> = Vec::new();
    let mut energies: Vec<Vec<f32>> = Vec::with_capacity(nsym * p.reps);
    let mut w = Weave::new();
    let mut bins = Vec::new();
    let mut zero = vec![C::ZERO; m];
    let mut med_sum = 0f64;
    let mut med_n = 0usize;
    for seg in segs {
        for s in 0..seg.nsym {
            let a = seg.start + s * (m + guard) + guard;
            let dir = w.next_dir();
            dirs.push(dir);
            let chunk: &[C] = if a + m <= rx.len() { &rx[a..a + m] } else {
                zero.iter_mut().for_each(|c| *c = C::ZERO);
                let lo = a.min(rx.len());
                let avail = rx.len() - lo;
                zero[..avail.min(m)].copy_from_slice(&rx[lo..lo + avail.min(m)]);
                &zero
            };
            md.dechirp(chunk, dir, &mut bins);
            let e: Vec<f32> = bins.iter().map(|b| b.norm_sqr()).collect();
            let mut sorted = e.clone();
            sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
            med_sum += sorted[m / 2] as f64 / core::f64::consts::LN_2;
            med_n += 1;
            energies.push(e);
        }
    }
    let noise_bin = (med_sum / med_n.max(1) as f64).max(1e-30);
    let gamma = m as f64 * mathf::pow10(design_snr_db / 10.0);
    let k = 2.0 * mathf::sqrt(gamma);
    let mut out = vec![vec![0f32; m]; nsym];
    for s in 0..nsym {
        for r in 0..p.reps {
            let idx = r * nsym + s;
            if idx >= energies.len() { continue; }
            let dir = dirs[idx];
            let wsum: f32 = if pdp.is_empty() { 1.0 } else { pdp.iter().sum() };
            for b in 0..m {
                let mut e = 0f64;
                if pdp.is_empty() { e = energies[idx][b] as f64; } else {
                    for (i, w) in pdp.iter().enumerate() {
                        if *w == 0.0 { continue; }
                        let d = i as i64 - crate::lattice::PDP_EL as i64;
                        let src = (b as i64 - dir as i64 * d).rem_euclid(m as i64) as usize;
                        e += (*w * energies[idx][src]) as f64;
                    }
                    e /= wsum as f64;
                }
                let z = mathf::sqrt(e / noise_bin);
                out[s][b] += mathf::ln_i0(k * z) as f32;
            }
        }
    }
    out
}

/// Iterative demapper and LDPC decoder. `outer` rounds of (demap with priors, decode `inner` iterations). Returns (info bits, iterations or -1).
pub fn decode_chirp_iter(p: &Profile, ll: &[Vec<f32>], outer: usize, inner: i32) -> Option<(Vec<u8>, i32)> {
    let sf = match p.kind { Kind::Chirp { sf, .. } => sf, _ => return None };
    let code = ldpc::profile_code(p.id)?;
    let nsym = p.block_symbols();
    let m = 1usize << sf;
    let gi: Vec<u32> = (0..m as u32).map(crate::chirp::gray_inv).collect();
    let perm = permutation(p.n);
    let tot = nsym * sf;
    let mut la = vec![0f32; tot]; // a priori, transmitted order (padded positions are known zeros)
    for i in p.n..tot { la[i] = 25.0; }
    let mut ext = vec![0f32; tot];
    let mut scratch = Vec::new();
    let mut info = vec![0u8; p.k];
    let mut cw_in = vec![0f32; p.n];
    let mut post = vec![0f32; p.n];
    let mut last_it = -1;
    for round in 0..outer.max(1) {
        for s in 0..nsym {
            let mut o = [0f32; 16];
            crate::chirp::bin_llrs_prior(&ll[s], sf, &gi, &la[s * sf..(s + 1) * sf], &mut o[..sf], &mut scratch);
            ext[s * sf..(s + 1) * sf].copy_from_slice(&o[..sf]);
        }
        for i in 0..p.n { cw_in[perm[i] as usize] = ext[i].clamp(-30.0, 30.0); }
        let it = code.decode_post(&cw_in, inner, &mut info, Some(&mut post));
        last_it = it;
        if it >= 0 { return Some((info, it)); }
        if round + 1 == outer.max(1) { break; }
        // extrinsic of the decoder -> a priori of the demapper
        for i in 0..p.n {
            let e = post[perm[i] as usize] - cw_in[perm[i] as usize];
            la[i] = e.clamp(-25.0, 25.0);
        }
    }
    Some((info, last_it))
}

/// Outer (demap/decode) rounds and inner LDPC iterations per round of the chirp receiver.
pub const OUTER_ITER: usize = 6;
pub const INNER_ITER: i32 = 12;

/// Decode with the posterior LLRs returned in transmitted order.
pub fn decode_block_post(p: &Profile, llr: &[f32], inner: i32) -> Option<(Vec<u8>, i32, Vec<f32>)> {
    let code = ldpc::profile_code(p.id)?;
    if llr.len() != p.n { return None; }
    let perm = permutation(p.n);
    let mut cw = vec![0f32; p.n];
    for i in 0..p.n { cw[perm[i] as usize] = llr[i]; }
    let mut info = vec![0u8; p.k];
    let mut post = vec![0f32; p.n];
    let it = code.decode_post(&cw, inner, &mut info, Some(&mut post));
    let tx: Vec<f32> = (0..p.n).map(|i| post[perm[i] as usize]).collect();
    Some((info, it, tx))
}

/// OFDM receiver for one block with decision-directed channel estimation rounds. `rx` starts at the block's first sample.
pub fn decode_ofdm_iter(p: &Profile, rx: &[C], noise_power: f32, rounds: usize) -> Option<(Vec<u8>, i32)> {
    let eq = crate::ofdm::Eq::new(p, rx, noise_power);
    let mut h = eq.pilot_h();
    let mut last = None;
    for r in 0..rounds.max(1) {
        let mut llr = eq.llrs(&h);
        llr.truncate(p.n);
        let (info, it, post) = decode_block_post(p, &llr, 30)?;
        if it >= 0 { return Some((info, it)); }
        last = Some((info, it));
        if r + 1 < rounds { 
            let mut pp = post;
            // pad to the number of data bits per block (n may be shorter than the carriers carry)
            pp.resize(eq.nsym * crate::ofdm::NDATA * eq.bps, 0.0);
            h = eq.refine(&pp, &h);
        }
    }
    last
}

pub const OFDM_ROUNDS: usize = 8;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Multipath: a path delayed by e chips puts its energy e bins away from the symbol's bin (rising: m - e, falling: m + e). The receiver therefore sums the
// bin energies over the path lags with weights c_e = rho_e / (1 + rho_e) (rho_e = mean path SNR per bin, the Rayleigh-fading square-law weights). The lags are
// first taken from the lattice correlation (a snapshot of a few seconds) and then re-estimated from the decoder's symbol posteriors (EM) in every round.

/// Bin energies of all transmitted symbols (repetitions included, in transmission order), normalised so that noise-only bins have mean 1.
pub struct ChirpRx {
    pub sf: usize,
    pub m: usize,
    pub nsym: usize,
    pub reps: usize,
    pub e: Vec<Vec<f32>>,
    pub dirs: Vec<i32>,
}

pub fn chirp_energies(p: &Profile, rx: &[C], segs: &[Segment]) -> Option<ChirpRx> {
    let (sf, guard) = match p.kind { Kind::Chirp { sf, guard } => (sf, guard), _ => return None };
    let md = Modem::new(sf, guard);
    let m = md.m;
    let mut e: Vec<Vec<f32>> = Vec::new();
    let mut dirs = Vec::new();
    let mut w = Weave::new();
    let mut bins = Vec::new();
    let mut zero = vec![C::ZERO; m];
    let mut med_sum = 0f64;
    for seg in segs {
        for s in 0..seg.nsym {
            let a = seg.start + s * (m + guard) + guard;
            let dir = w.next_dir();
            dirs.push(dir);
            let chunk: &[C] = if a + m <= rx.len() { &rx[a..a + m] } else {
                zero.iter_mut().for_each(|c| *c = C::ZERO);
                let lo = a.min(rx.len());
                let avail = rx.len() - lo;
                zero[..avail.min(m)].copy_from_slice(&rx[lo..lo + avail.min(m)]);
                &zero
            };
            md.dechirp(chunk, dir, &mut bins);
            let v: Vec<f32> = bins.iter().map(|b| b.norm_sqr()).collect();
            let mut sorted = v.clone();
            sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(core::cmp::Ordering::Equal));
            med_sum += sorted[m / 2] as f64 / core::f64::consts::LN_2;
            e.push(v);
        }
    }
    let noise_bin = (med_sum / e.len().max(1) as f64).max(1e-30) as f32;
    for v in e.iter_mut() { for x in v.iter_mut() { *x /= noise_bin; } }
    let nsym = p.block_symbols();
    if e.len() != nsym * p.reps { return None; }
    Some(ChirpRx { sf, m, nsym, reps: p.reps, e, dirs })
}

fn tap_lag(i: usize) -> i64 { i as i64 - crate::lattice::PDP_EL as i64 }

/// Log-likelihoods of every bin of every block symbol with the lag weights `c` (index e + PDP_EL; empty = a single path at lag 0).
pub fn combine_ll(c: &ChirpRx, taps: &[f32], design_snr_db: f64) -> Vec<Vec<f32>> {
    let m = c.m;
    let gamma = m as f64 * mathf::pow10(design_snr_db / 10.0);
    let k = 2.0 * mathf::sqrt(gamma);
    let lim = (m as i64 / 8).max(2);   // a lag window wider than an eighth of the alphabet would blur neighbouring symbol values
    let active: Vec<(i64, f32)> = if taps.is_empty() { vec![(0, 1.0)] } else { taps.iter().enumerate().filter(|(i, w)| **w > 0.0 && tap_lag(*i).abs() <= lim).map(|(i, w)| (tap_lag(i), *w)).collect() };
    let wsum: f32 = active.iter().map(|x| x.1).sum::<f32>().max(1e-6);
    let linear = active.len() > 1 && MODE.load(core::sync::atomic::Ordering::Relaxed) & 1 != 0;   // several paths: the square-law (Rayleigh) metric sum c_e E; one path: the Rician metric ln I0(2 sqrt(gamma) |y|)
    let mut out = vec![vec![0f32; m]; c.nsym];
    for s in 0..c.nsym {
        for r in 0..c.reps {
            let idx = r * c.nsym + s;
            let dir = c.dirs[idx] as i64;
            let ev = &c.e[idx];
            for b in 0..m {
                let mut acc = 0f32;
                for &(lag, w) in &active {
                    let src = (b as i64 - dir * lag).rem_euclid(m as i64) as usize;
                    acc += w * ev[src];
                }
                if linear { out[s][b] += acc; } else { out[s][b] += mathf::ln_i0(k * mathf::sqrt((acc / wsum) as f64)) as f32; }
            }
        }
    }
    out
}

/// Re-estimate the lag weights from the symbol posteriors (ll + a priori bit LLRs `la`, transmitted order).
fn em_taps(c: &ChirpRx, ll: &[Vec<f32>], la: &[f32], gi: &[u32], old: &[f32]) -> Vec<f32> {
    let m = c.m;
    let sf = c.sf;
    let nt = c.nsym * c.reps;
    let ntaps = crate::lattice::PDP_EL + crate::lattice::PDP_EH + 1;
    let mut q = vec![0f64; ntaps];
    let mut nconf = 0usize;
    let _ = nt;
    let mut pm = vec![0f64; m];
    for s in 0..c.nsym {
        let mut mx = f64::MIN;
        for b in 0..m {
            let mut v = ll[s][b] as f64;
            let bits = gi[b];
            for j in 0..sf {
                let h = 0.5 * la[s * sf + j] as f64;
                if (bits >> (sf - 1 - j)) & 1 == 0 { v += h; } else { v -= h; }
            }
            pm[b] = v;
            if v > mx { mx = v; }
        }
        let mut tot = 0f64;
        let mut best = 0usize;
        for b in 0..m { pm[b] = mathf::exp(pm[b] - mx); tot += pm[b]; if pm[b] > pm[best] { best = b; } }
        let soft = MODE.load(core::sync::atomic::Ordering::Relaxed) & 2 != 0;
        if !soft {
            // only symbols whose posterior is concentrated on one value take part (a diffuse posterior would smear the lag estimate over its own spread)
            if pm[best] / tot < 0.7 { continue; }
            nconf += c.reps;
            for r in 0..c.reps {
                let idx = r * c.nsym + s;
                let dir = c.dirs[idx] as i64;
                for (ti, qv) in q.iter_mut().enumerate() {
                    let lag = tap_lag(ti);
                    let src = (best as i64 - dir * lag).rem_euclid(m as i64) as usize;
                    *qv += c.e[idx][src] as f64 - 1.0;
                }
            }
        } else {
            nconf += c.reps;
            for r in 0..c.reps {
                let idx = r * c.nsym + s;
                let dir = c.dirs[idx] as i64;
                for (ti, qv) in q.iter_mut().enumerate() {
                    let lag = tap_lag(ti);
                    let mut acc = 0f64;
                    for b in 0..m {
                        if pm[b] < 1e-4 * tot { continue; }
                        let src = (b as i64 - dir * lag).rem_euclid(m as i64) as usize;
                        acc += pm[b] / tot * (c.e[idx][src] as f64 - 1.0);
                    }
                    *qv += acc;
                }
            }
        }
    }
    if nconf < 8 { return old.to_vec(); }
    let thr = 3.0 / mathf::sqrt(nconf as f64);
    let rho: Vec<f64> = q.iter().map(|&v| v / nconf as f64).collect();
    let rmax = rho.iter().cloned().fold(0.0, f64::max);
    let sat = MODE.load(core::sync::atomic::Ordering::Relaxed) & 4 != 0;
    rho.iter().map(|&r| if r > thr { (if sat { r / (1.0 + r) } else { r / (1.0 + rmax) }) as f32 } else { 0.0 }).collect()
}

/// Iterative demapper / LDPC decoder with multipath lag weights re-estimated each round (`em`). `w0` = initial lag weights (lattice), empty = single path.
/// Experiment switches: bit 0 linear (square-law) metric with several paths, bit 1 soft EM posteriors, bit 2 saturating tap weights.
pub static MODE: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(6);
pub fn decode_chirp_em(p: &Profile, c: &ChirpRx, w0: &[f32], design_snr_db: f64, outer: usize, inner: i32, em: bool) -> Option<(Vec<u8>, i32)> {
    let sf = c.sf;
    let code = ldpc::profile_code(p.id)?;
    let nsym = c.nsym;
    let m = c.m;
    let gi: Vec<u32> = (0..m as u32).map(crate::chirp::gray_inv).collect();
    let perm = permutation(p.n);
    let tot = nsym * sf;
    let mut la = vec![0f32; tot];
    for i in p.n..tot { la[i] = 25.0; }
    let mut ext = vec![0f32; tot];
    let mut scratch = Vec::new();
    let mut info = vec![0u8; p.k];
    let mut cw_in = vec![0f32; p.n];
    let mut post = vec![0f32; p.n];
    // lattice relative powers -> square-law weights rho / (1 + rho) with rho = relative power x the design bin SNR
    let rho_main = m as f64 * mathf::pow10(design_snr_db / 10.0);
    let wmax = w0.iter().cloned().fold(0.0f32, f32::max) as f64;
    let sat = MODE.load(core::sync::atomic::Ordering::Relaxed) & 4 != 0;
    let mut taps: Vec<f32> = w0.iter().map(|&w| { let r = w as f64 * rho_main; (if sat { r / (1.0 + r) } else { r / (1.0 + wmax * rho_main) }) as f32 }).collect();
    let mut ll = combine_ll(c, &taps, design_snr_db);
    let mut last_it = -1;
    for round in 0..outer.max(1) {
        for s in 0..nsym {
            let mut o = [0f32; 16];
            crate::chirp::bin_llrs_prior(&ll[s], sf, &gi, &la[s * sf..(s + 1) * sf], &mut o[..sf], &mut scratch);
            ext[s * sf..(s + 1) * sf].copy_from_slice(&o[..sf]);
        }
        for i in 0..p.n { cw_in[perm[i] as usize] = ext[i].clamp(-30.0, 30.0); }
        let it = code.decode_post(&cw_in, inner, &mut info, Some(&mut post));
        last_it = it;
        if it >= 0 { return Some((info, it)); }
        if round + 1 == outer.max(1) { break; }
        for i in 0..p.n {
            let e = post[perm[i] as usize] - cw_in[perm[i] as usize];
            la[i] = e.clamp(-25.0, 25.0);
        }
        if em && round >= 1 {
            taps = em_taps(c, &ll, &la, &gi, &taps);
            if taps.iter().all(|w| *w == 0.0) { taps.clear(); }
            ll = combine_ll(c, &taps, design_snr_db);
        }
    }
    Some((info, last_it))
}
