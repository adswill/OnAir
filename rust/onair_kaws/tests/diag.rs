// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts};

#[test]
#[ignore]
fn mod_failures() {
    let id: usize = std::env::var("KAWS_ID").ok().and_then(|v| v.parse().ok()).unwrap_or(3);
    let snr: f64 = std::env::var("KAWS_SNR").ok().and_then(|v| v.parse().ok()).unwrap_or(0.0);
    let spec = match std::env::var("KAWS_CH").unwrap_or("mod".into()).as_str() { "poor" => watterson::POOR, "awgn" => watterson::AWGN, "static" => watterson::Spec { name: "static2", paths: 2, delay_s: 0.002, doppler_2sigma_hz: 0.0 }, _ => watterson::MOD };
    let p = profile::get(id).unwrap();
    let mut rng = Rng::new(77);
    link::MODE.store(std::env::var("KAWS_MODE").ok().and_then(|v| v.parse().ok()).unwrap_or(6), std::sync::atomic::Ordering::Relaxed);
    let (mut ok, mut bad) = (0, 0);
    for t in 0..120 {
        let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
        let tx = link::build_burst(p, &[info.clone()]).unwrap();
        let mut o = Opts::new(spec, snr);
        o.start_delay = rng.range(2400.0, 12000.0);
        o.freq_offset_hz = rng.range(-100.0, 100.0);
        o.tail = 2400;
        let rx = watterson::apply(&tx, &o, &mut rng);
        let r = link::rx_burst(p, 1, &rx, 0, 14400, p.est_awgn_db);
        let good = r.sync && r.blocks[0].1 >= 0 && r.blocks[0].0 == info;
        if good { ok += 1; } else {
            bad += 1;
            let ber = if r.sync { r.blocks[0].0.iter().zip(info.iter()).filter(|(a, b)| a != b).count() } else { 0 };
            println!("FAIL t{t}: tau err {:.2} delta err {:.2} stat {:.0} iters {} info bit errors {ber}", r.tau - o.start_delay, r.delta_hz - o.freq_offset_hz, r.stat, r.blocks[0].1);
        }
    }
    println!("DIAG P{id} {} snr {snr}: ok {ok} bad {bad}", spec.name);
}
