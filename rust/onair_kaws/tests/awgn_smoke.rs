// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::cplx::C;
use onair_kaws::rng::Rng;
use onair_kaws::watterson::{self, Opts};

fn trial(id: usize, snr: f64, spec: watterson::Spec, rng: &mut Rng) -> bool {
    let p = profile::get(id).unwrap();
    let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
    let coded = link::encode_block(p, &info).unwrap();
    let vals = link::chirp_values(p, &coded);
    let tx = link::modulate_chirp(p, &vals);
    let mut o = Opts::new(spec, snr);
    o.start_delay = 0.0;
    let mut rx = watterson::apply(&tx, &o, rng);
    rx.truncate(tx.len() + 40);
    let llr = link::demod_chirp(p, &rx, &link::plain_segments(p), p.est_awgn_db);
    let (dec, it) = link::decode_block(p, &llr).unwrap();
    it >= 0 && dec == info
}

#[test]
fn smoke() {
    let _ = C::ZERO;
    let mut rng = Rng::new(5);
    for id in [1usize, 3] {
        let p = profile::get(id).unwrap();
        for d in [-3.0, -1.0, 0.0, 1.0, 2.0, 4.0] {
            let snr = p.est_awgn_db + d;
            let n = 60;
            let ok = (0..n).filter(|_| trial(id, snr, watterson::AWGN, &mut rng)).count();
            println!("P{id} snr {snr:.1}: {ok}/{n}");
        }
    }
}
