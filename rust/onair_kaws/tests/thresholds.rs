// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Threshold measurement harness (spec 4.4). Heavy: run with `cargo test --release --test thresholds -- --ignored --nocapture`.
//! Environment: KAWS_PROFILES (comma list, default all chirp), KAWS_CHANNELS (awgn,mod,poor), KAWS_BLOCKS, KAWS_OUTER, KAWS_THREADS, KAWS_STEP.
use onair_kaws::*;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts, Spec};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

fn env_usize(k: &str, d: usize) -> usize { std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d) }
fn env_list(k: &str, d: &str) -> Vec<String> { std::env::var(k).unwrap_or(d.to_string()).split(',').map(|s| s.to_string()).collect() }

fn spec_of(s: &str) -> Spec { match s { "awgn" => watterson::AWGN, "mod" => watterson::MOD, _ => watterson::POOR } }

/// One genie-synchronised payload block (no preamble, no offsets). Returns (decoded ok, wrong-but-converged).
fn payload_trial(id: usize, snr: f64, spec: Spec, outer: usize, rng: &mut Rng) -> (bool, bool) {
    let p = profile::get(id).unwrap();
    let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
    let coded = link::encode_block(p, &info).unwrap();
    let tx = link::modulate_chirp(p, &link::chirp_values(p, &coded));
    let o = Opts::new(spec, snr);
    let mut rx = watterson::apply(&tx, &o, rng);
    rx.truncate(tx.len() + 40);
    static OFF: std::sync::OnceLock<f64> = std::sync::OnceLock::new();
    let off = *OFF.get_or_init(|| std::env::var("KAWS_DESIGN_DB").ok().and_then(|v| v.parse().ok()).unwrap_or(0.0));
    let ll = link::chirp_loglik(p, &rx, &link::plain_segments(p), p.est_awgn_db + off, &[]);
    let (dec, it) = link::decode_chirp_iter(p, &ll, outer, link::INNER_ITER).unwrap();
    (it >= 0 && dec == info, it >= 0 && dec != info)
}

pub fn fer_point(id: usize, snr: f64, spec: Spec, nblocks: usize, max_fail: usize, outer: usize, threads: usize, seed: u64) -> (usize, usize, usize) {
    let done = Arc::new(AtomicUsize::new(0));
    let fails = Arc::new(AtomicUsize::new(0));
    let undet = Arc::new(AtomicUsize::new(0));
    let mut hs = vec![];
    for t in 0..threads {
        let (done, fails, undet) = (done.clone(), fails.clone(), undet.clone());
        hs.push(std::thread::spawn(move || {
            let mut rng = Rng::new(seed * 1000 + t as u64 * 7919 + 1);
            loop {
                let i = done.fetch_add(1, Ordering::SeqCst);
                if i >= nblocks || fails.load(Ordering::SeqCst) >= max_fail { break; }
                let (ok, bad) = payload_trial(id, snr, spec, outer, &mut rng);
                if !ok { fails.fetch_add(1, Ordering::SeqCst); }
                if bad { undet.fetch_add(1, Ordering::SeqCst); }
            }
        }));
    }
    for h in hs { h.join().unwrap(); }
    let n = done.load(Ordering::SeqCst).min(nblocks);
    // `done` counts started trials; fails may stop early, so the sample count is approximate when stopped early: use started trials
    (fails.load(Ordering::SeqCst), n.max(1), undet.load(Ordering::SeqCst))
}

#[test]
#[ignore]
fn thr_chirp() {
    if let Ok(v) = std::env::var("KAWS_DEG") { let x: Vec<u32> = v.split(',').map(|s| s.parse().unwrap()).collect(); ldpc::TUNE_HEAVY_PCT.store(x[0], Ordering::Relaxed); ldpc::TUNE_W_HEAVY.store(x[1], Ordering::Relaxed); ldpc::TUNE_W_LIGHT.store(x[2], Ordering::Relaxed); }
    let profs = env_list("KAWS_PROFILES", "0,1,2,3,4");
    let chans = env_list("KAWS_CHANNELS", "awgn,mod,poor");
    let outer = env_usize("KAWS_OUTER", link::OUTER_ITER);
    let threads = env_usize("KAWS_THREADS", 4);
    let step = std::env::var("KAWS_STEP").ok().and_then(|v| v.parse().ok()).unwrap_or(0.5f64);
    for ps in &profs {
        let id: usize = ps.parse().unwrap();
        let p = profile::get(id).unwrap();
        let nblocks = env_usize("KAWS_BLOCKS", if id == 0 { 200 } else { 1000 });
        for cs in &chans {
            let spec = spec_of(cs);
            let mut snr = p.est_awgn_db - 3.0;
            let mut pts: Vec<(f64, f64, usize, usize)> = vec![];
            loop {
                let (f, n, u) = fer_point(id, snr, spec, nblocks, nblocks / 10, outer, threads, 17);
                let fer = f as f64 / n as f64;
                println!("P{id} {} snr {snr:.1} fer {fer:.4} ({f}/{n}) undetected {u}", spec.name);
                pts.push((snr, fer, n, u));
                if fer < 0.003 && n >= nblocks { break; }
                if snr > p.est_awgn_db + 14.0 { break; }
                snr += step;
            }
            // interpolate FER 10% and 1% (log-linear between points)
            let find = |target: f64| -> Option<f64> {
                for w in pts.windows(2) {
                    let (s0, f0, s1, f1) = (w[0].0, w[0].1, w[1].0, w[1].1);
                    if f0 >= target && f1 < target {
                        let (l0, l1) = (f0.max(1e-5).ln(), f1.max(1e-5).ln());
                        return Some(s0 + (s1 - s0) * (target.ln() - l0) / (l1 - l0));
                    }
                }
                None
            };
            let undet: usize = pts.iter().map(|x| x.3).sum();
            println!("RESULT P{id} {} fer10 {:?} fer1 {:?} undetected_total {undet} blocks_per_point {nblocks} outer {outer}", spec.name, find(0.1).map(|v| (v * 10.0).round() / 10.0), find(0.01).map(|v| (v * 10.0).round() / 10.0));
        }
    }
}

fn pct(v: &mut Vec<f64>, q: f64) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[((v.len() as f64 - 1.0) * q).round() as usize]
}

/// Noise-only detection statistic: distribution of the best hypothesis over windows that contain no signal.
#[test]
#[ignore]
fn noise_only_detect() {
    let trials = env_usize("KAWS_NOISE_TRIALS", 300);
    for id in std::env::var("KAWS_NOISE_PROFILES").unwrap_or("0,1,3".into()).split(',').map(|s| s.parse::<usize>().unwrap()).collect::<Vec<_>>() {
        let p = profile::get(id).unwrap();
        let lay = link::layout(p, 1);
        let len = 14400 + lay.total;
        let threads = 4;
        let mut hs = vec![];
        for t in 0..threads {
            hs.push(std::thread::spawn(move || {
                let mut rng = Rng::new(99 + t as u64);
                let p = profile::get(id).unwrap();
                let lay = link::layout(p, 1);
                let mut out = vec![];
                for _ in 0..trials / threads {
                    let rx = watterson::noise(len, 1.0, &mut rng);
                    out.push(onair_kaws::lattice::detect(&rx, &lay.unit_offsets, 0, 14400).stat);
                }
                out
            }));
        }
        let mut all: Vec<f64> = hs.into_iter().flat_map(|h| h.join().unwrap()).collect();
        println!("NOISE P{id} units {} trials {}: p50 {:.2} p90 {:.2} p99 {:.2} max {:.2}", lay.unit_offsets.len(), all.len(), pct(&mut all, 0.5), pct(&mut all, 0.9), pct(&mut all, 0.99), pct(&mut all, 1.0));
    }
}

/// One full-chain trial: preamble detection, offsets, clock error. Returns (sync_ok_and_aligned, block_ok, undetected_wrong).
fn full_trial(id: usize, snr: f64, spec: Spec, rng: &mut Rng, drift: f64, interferer: Option<f64>) -> (bool, bool, bool) {
    let p = profile::get(id).unwrap();
    let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
    let tx = link::build_burst(p, &[info.clone()]).unwrap();
    let mut o = Opts::new(spec, snr);
    o.start_delay = rng.range(2400.0, 12000.0);
    o.freq_offset_hz = rng.range(-100.0, 100.0);
    o.drift_hz_s = drift;
    o.interferer_db = interferer;
    o.tail = 2400;
    let rx = watterson::apply(&tx, &o, rng);
    let r = link::rx_burst(p, 1, &rx, 0, 14400, p.est_awgn_db);
    let aligned = r.sync && (r.tau - o.start_delay).abs() < 40.0;
    let ok = aligned && r.blocks[0].1 >= 0 && r.blocks[0].0 == info;
    let bad = r.sync && r.blocks[0].1 >= 0 && r.blocks[0].0 != info;
    (aligned, ok, bad)
}

fn full_point(id: usize, snr: f64, spec: Spec, n: usize, threads: usize, seed: u64, drift: f64, interferer: Option<f64>) -> (usize, usize, usize, usize) {
    let done = Arc::new(AtomicUsize::new(0));
    let (al, ok, bad) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
    let mut hs = vec![];
    for t in 0..threads {
        let (done, al, ok, bad) = (done.clone(), al.clone(), ok.clone(), bad.clone());
        hs.push(std::thread::spawn(move || {
            let mut rng = Rng::new(seed * 977 + t as u64 * 104729 + 3);
            while done.fetch_add(1, Ordering::SeqCst) < n {
                let (a, o, b) = full_trial(id, snr, spec, &mut rng, drift, interferer);
                if a { al.fetch_add(1, Ordering::SeqCst); }
                if o { ok.fetch_add(1, Ordering::SeqCst); }
                if b { bad.fetch_add(1, Ordering::SeqCst); }
            }
        }));
    }
    for h in hs { h.join().unwrap(); }
    (n, al.load(Ordering::SeqCst), ok.load(Ordering::SeqCst), bad.load(Ordering::SeqCst))
}

/// Full chain (sync + +-100 Hz + +-2 s clock error) on the SNR grid given in KAWS_FULL = "id:snr,snr;id:snr..." for each channel in KAWS_CHANNELS.
#[test]
#[ignore]
fn thr_full() {
    let chans = env_list("KAWS_CHANNELS", "awgn,mod,poor");
    let threads = env_usize("KAWS_THREADS", 4);
    let n = env_usize("KAWS_FULL_BLOCKS", 200);
    let drift: f64 = std::env::var("KAWS_DRIFT").ok().and_then(|v| v.parse().ok()).unwrap_or(0.0);
    let interf: Option<f64> = std::env::var("KAWS_INTERF").ok().and_then(|v| v.parse().ok());
    let grid = std::env::var("KAWS_FULL").unwrap();
    for part in grid.split(';') {
        let (ids, snrs) = part.split_once(':').unwrap();
        let id: usize = ids.parse().unwrap();
        for cs in &chans {
            let spec = spec_of(cs);
            for sn in snrs.split(',') {
                let snr: f64 = sn.parse().unwrap();
                let (tot, al, ok, bad) = full_point(id, snr, spec, n, threads, 5, drift, interf);
                println!("FULL P{id} {} snr {snr:.1} drift {drift} interf {:?}: sync_ok {al}/{tot} block_ok {ok}/{tot} undetected_wrong {bad}", spec.name, interf);
            }
        }
    }
}

/// Full-chain SNR sweep per profile and channel: KAWS_PROFILES, KAWS_CHANNELS, KAWS_FULL_BLOCKS, KAWS_START (dB offset of the first point from the spec estimate, default -2),
/// KAWS_STEP. Prints one line per point and a RESULTF line with the interpolated SNR at 10 % and 1 % block error (sync failures count as errors).
#[test]
#[ignore]
fn thr_sweep_full() {
    let profs = env_list("KAWS_PROFILES", "5,6,7");
    let chans = env_list("KAWS_CHANNELS", "awgn,mod,poor");
    let threads = env_usize("KAWS_THREADS", 4);
    let n = env_usize("KAWS_FULL_BLOCKS", 200);
    let step = std::env::var("KAWS_STEP").ok().and_then(|v| v.parse().ok()).unwrap_or(0.5f64);
    let start: f64 = std::env::var("KAWS_START").ok().and_then(|v| v.parse().ok()).unwrap_or(-2.0);
    let drift: f64 = std::env::var("KAWS_DRIFT").ok().and_then(|v| v.parse().ok()).unwrap_or(0.0);
    for ps in &profs {
        let id: usize = ps.parse().unwrap();
        let p = profile::get(id).unwrap();
        for cs in &chans {
            let spec = spec_of(cs);
            let mut snr = p.est_awgn_db + start;
            let mut pts: Vec<(f64, f64)> = vec![];
            let mut undet = 0;
            let mut synclost = 0usize;
            let mut total = 0usize;
            loop {
                let (tot, al, ok, bad) = full_point(id, snr, spec, n, threads, 9, drift, None);
                let fer = 1.0 - ok as f64 / tot as f64;
                println!("FULLPT P{id} {} snr {snr:.1}: block_ok {ok}/{tot} sync_ok {al}/{tot} undetected {bad}", spec.name);
                pts.push((snr, fer));
                undet += bad;
                synclost += tot - al;
                total += tot;
                if fer < 0.01 || snr > p.est_awgn_db + 16.0 { break; }
                snr += step;
            }
            let find = |target: f64| -> Option<f64> {
                for w in pts.windows(2) {
                    let (s0, f0, s1, f1) = (w[0].0, w[0].1, w[1].0, w[1].1);
                    if f0 >= target && f1 < target {
                        let (l0, l1) = (f0.max(1e-3).ln(), f1.max(1e-3).ln());
                        return Some(s0 + (s1 - s0) * (target.ln() - l0) / (l1 - l0));
                    }
                }
                None
            };
            println!("RESULTF P{id} {} fer10 {:?} fer1 {:?} undetected_total {undet} sync_lost_total {synclost}/{total} blocks_per_point {n} drift {drift}", spec.name, find(0.1).map(|v| (v * 10.0).round() / 10.0), find(0.01).map(|v| (v * 10.0).round() / 10.0));
        }
    }
}

/// Quick probe: FER and converged-wrong count of one profile/channel at a list of SNRs. KAWS_PROFILES, KAWS_CHANNELS, KAWS_SNRS, KAWS_BLOCKS, KAWS_OUTER.
#[test]
#[ignore]
fn probe() {
    if let Ok(v) = std::env::var("KAWS_DEG") { let x: Vec<u32> = v.split(',').map(|s| s.parse().unwrap()).collect(); ldpc::TUNE_HEAVY_PCT.store(x[0], Ordering::Relaxed); ldpc::TUNE_W_HEAVY.store(x[1], Ordering::Relaxed); ldpc::TUNE_W_LIGHT.store(x[2], Ordering::Relaxed); }
    let outer = env_usize("KAWS_OUTER", link::OUTER_ITER);
    let n = env_usize("KAWS_BLOCKS", 500);
    for ps in env_list("KAWS_PROFILES", "2") {
        let id: usize = ps.parse().unwrap();
        for cs in env_list("KAWS_CHANNELS", "awgn") {
            for sn in env_list("KAWS_SNRS", "-18") {
                let snr: f64 = sn.parse().unwrap();
                let (f, t, u) = fer_point(id, snr, spec_of(&cs), n, n, outer, 4, 31);
                println!("PROBE P{id} {cs} snr {snr} outer {outer}: fail {f}/{t} converged_wrong {u}");
            }
        }
    }
}

/// Median and spread of the per-position statistic in noise-only windows (diagnostic used to fix the constants of the detector).
#[test]
#[ignore]
fn noise_consts() {
    let mut rng = Rng::new(5);
    for nu in [1usize, 2, 10] {
        let offs: Vec<usize> = (0..nu).map(|i| i * 12000).collect();
        let (mut m, mut s) = (vec![], vec![]);
        for _ in 0..30 {
            let rx = watterson::noise(14400 + offs[nu - 1] + 8400 + 400, 1.0, &mut rng);
            let d = onair_kaws::lattice::detect(&rx, &offs, 0, 14400);
            m.push(d.t_med as f64); s.push(d.t_sig as f64);
        }
        println!("CONSTS units {nu}: med {:.3} sig {:.3} (min/max med {:.3}/{:.3}, sig {:.3}/{:.3})", m.iter().sum::<f64>() / 30.0, s.iter().sum::<f64>() / 30.0, m.iter().cloned().fold(1e9, f64::min), m.iter().cloned().fold(0.0, f64::max), s.iter().cloned().fold(1e9, f64::min), s.iter().cloned().fold(0.0, f64::max));
    }
}
