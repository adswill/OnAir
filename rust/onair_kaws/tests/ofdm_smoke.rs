// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts};

#[test]
fn ofdm_roundtrip() {
    let mut rng = Rng::new(33);
    for id in [5usize, 6, 7] {
        let p = profile::get(id).unwrap();
        for (spec, d) in [(watterson::AWGN, 6.0), (watterson::AWGN, 3.0), (watterson::AWGN, 1.0), (watterson::MOD, 8.0), (watterson::POOR, 10.0)] {
            let mut ok = 0;
            let n = 20;
            for _ in 0..n {
                let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
                let tx = link::build_burst(p, &[info.clone()]).unwrap();
                let mut o = Opts::new(spec, p.est_awgn_db + d);
                o.start_delay = 2400.0 * 1.5 + rng.range(0.0, 500.0);
                o.freq_offset_hz = rng.range(-100.0, 100.0);
                o.tail = 4000;
                let rx = watterson::apply(&tx, &o, &mut rng);
                let r = link::rx_burst(p, 1, &rx, 0, 14000, p.est_awgn_db);
                if r.sync && r.blocks[0].1 >= 0 && r.blocks[0].0 == info { ok += 1; }
            }
            println!("P{id} {} snr {:.1}: {ok}/{n}", spec.name, p.est_awgn_db + d);
        }
    }
}
