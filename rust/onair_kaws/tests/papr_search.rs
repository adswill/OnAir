// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Random search of the 15 element start phases for the lowest peak-to-mean ratio of the lattice unit (the result is baked into lattice::PHASES).
use onair_kaws::*;
use onair_kaws::rng::Rng;

#[test]
#[ignore]
fn phase_search() {
    let mut rng = Rng::new(424242);
    let mut best = [0f64; 15];
    let mut best_papr = lattice::papr_db(&lattice::unit_with(&best));
    println!("zero phases: {best_papr:.2} dB");
    for it in 0..4000 {
        let mut ph = best;
        if it < 1500 { for p in ph.iter_mut() { *p = rng.range(0.0, 6.2831853); } } else {
            for _ in 0..3 { let k = rng.below(15) as usize; ph[k] = rng.range(0.0, 6.2831853); }
        }
        let p = lattice::papr_db(&lattice::unit_with(&ph));
        if p < best_papr { best_papr = p; best = ph; }
    }
    println!("best {best_papr:.2} dB phases {:?}", best.iter().map(|v| (v * 1000.0).round() / 1000.0).collect::<Vec<_>>());
}
