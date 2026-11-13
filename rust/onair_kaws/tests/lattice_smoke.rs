// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts};

#[test]
fn lattice_detect() {
    let mut rng = Rng::new(11);
    let u = lattice::unit();
    for snr in [10.0, -10.0, -15.0, -20.0] {
        for off in [0.0, 60.0, -90.0] {
            let mut o = Opts::new(watterson::AWGN, snr);
            let start = 3000.5 + rng.range(0.0, 100.0);
            o.start_delay = start;
            o.freq_offset_hz = off;
            o.tail = 6000;
            let rx = watterson::apply(&u, &o, &mut rng);
            let d = lattice::detect(&rx, &[0], 0, 9000);
            println!("snr {snr} off {off}: stat {:.1} tau err {:.2} delta err {:.2} Hz", d.stat, d.tau - start, d.delta_hz[0] - off);
        }
    }
    // noise only
    let mut mx = vec![];
    for _ in 0..30 {
        let rx = watterson::noise(20000, 1.0, &mut rng);
        mx.push(lattice::detect(&rx, &[0], 0, 9000).stat);
    }
    mx.sort_by(|a, b| a.partial_cmp(b).unwrap());
    println!("noise-only max stat: min {:.2} median {:.2} max {:.2}", mx[0], mx[15], mx[29]);
}
