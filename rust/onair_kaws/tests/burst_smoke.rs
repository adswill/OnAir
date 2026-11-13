// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts};

#[test]
fn burst_roundtrip() {
    let mut rng = Rng::new(21);
    for id in [0usize, 1, 3, 4] {
        let p = profile::get(id).unwrap();
        let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
        let tx = link::build_burst(p, &[info.clone()]).unwrap();
        let lay = link::layout(p, 1);
        assert_eq!(tx.len(), lay.total);
        for (spec, d) in [(watterson::AWGN, 8.0), (watterson::AWGN, 4.0)] {
            let snr = p.est_awgn_db + d;
            let mut o = Opts::new(spec, snr);
            o.start_delay = 2400.0 * 1.5 + 1234.37;
            o.freq_offset_hz = 73.0;
            o.tail = 4000;
            let rx = watterson::apply(&tx, &o, &mut rng);
            let t0 = std::time::Instant::now();
            let r = link::rx_burst(p, 1, &rx, 0, 14000, p.est_awgn_db);
            let ok = r.sync && r.blocks[0].1 >= 0 && r.blocks[0].0 == info;
            println!("P{id} snr {snr:.1} tau err {:.2} delta {:.2} stat {:.1} ok {ok} ({:.2}s)", r.tau - o.start_delay, r.delta_hz, r.stat, t0.elapsed().as_secs_f64());
        }
    }
}
