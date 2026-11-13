// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use onair_kaws::*;
use onair_kaws::rng::Rng;

#[test]
fn burst_through_12k_audio() {
    let mut rng = Rng::new(3);
    for id in [1usize, 3, 5] {
        let p = profile::get(id).unwrap();
        let info: Vec<u8> = (0..p.k).map(|_| (rng.next_u64() & 1) as u8).collect();
        let tx = link::build_burst(p, &[info.clone()]).unwrap();
        let mut padded = vec![cplx::C::ZERO; 3600];
        padded.extend_from_slice(&tx);
        padded.extend(std::iter::repeat(cplx::C::ZERO).take(2400));
        let a = audio::to_audio(&padded, 0.9);
        // add real noise at 12 kHz: audio noise power chosen for a high SNR
        let rx = audio::from_audio(&a);
        let r = link::rx_burst(p, 1, &rx, 0, 12000, p.est_awgn_db);
        println!("P{id}: sync {} stat {:.1} tau {:.2} iters {:?}", r.sync, r.stat, r.tau, r.blocks.iter().map(|b| b.1).collect::<Vec<_>>());
        assert!(r.sync && r.blocks[0].0 == info);
    }
}
