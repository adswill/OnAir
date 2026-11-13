// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The profile ladder (spec section 2): ids 0..7. Profile ids 8..15 are reserved.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Kind {
    /// Cyclic-shift chirps: spreading factor (bits per symbol) and guard chips in front of each symbol.
    Chirp { sf: usize, guard: usize },
    /// Coherent OFDM: bits per data carrier (1 = BPSK, 2 = QPSK), OFDM symbols per block.
    Ofdm { bps: usize, nsym: usize },
}
#[derive(Clone, Copy, Debug)]
pub struct Profile {
    pub id: usize,
    pub name: &'static str,
    pub kind: Kind,
    /// Coded block length and information bits.
    pub n: usize,
    pub k: usize,
    /// Repetitions of the coded block (time diversity, P0 only).
    pub reps: usize,
    /// Slot length in seconds (0 = none, push to talk).
    pub slot_s: u32,
    /// Lattice units in the preamble (front, midambles); P0 10, P1 2, others 1 (spec section 3).
    pub units: usize,
    /// Spec ESTIMATE of the AWGN threshold in dB (SNR in 2.4 kHz), used only to shape the soft demapper.
    pub est_awgn_db: f64,
}
pub const PROFILES: [Profile; 8] = [
    Profile { id: 0, name: "Abyss", kind: Kind::Chirp { sf: 10, guard: 0 }, n: 512, k: 128, reps: 3, slot_s: 120, units: 10, est_awgn_db: -29.6 },
    Profile { id: 1, name: "Deep", kind: Kind::Chirp { sf: 10, guard: 0 }, n: 1024, k: 256, reps: 1, slot_s: 60, units: 2, est_awgn_db: -25.2 },
    Profile { id: 2, name: "Weak", kind: Kind::Chirp { sf: 9, guard: 0 }, n: 1008, k: 336, reps: 1, slot_s: 60, units: 1, est_awgn_db: -21.4 },
    Profile { id: 3, name: "Chat", kind: Kind::Chirp { sf: 8, guard: 0 }, n: 1024, k: 512, reps: 1, slot_s: 30, units: 1, est_awgn_db: -17.1 },
    Profile { id: 4, name: "Mid", kind: Kind::Chirp { sf: 6, guard: 16 }, n: 1020, k: 510, reps: 1, slot_s: 15, units: 1, est_awgn_db: -13.3 },
    Profile { id: 5, name: "Fast data", kind: Kind::Ofdm { bps: 1, nsym: 40 }, n: 1440, k: 360, reps: 1, slot_s: 15, units: 1, est_awgn_db: -7.4 },
    Profile { id: 6, name: "Voice-700", kind: Kind::Ofdm { bps: 2, nsym: 24 }, n: 1728, k: 600, reps: 1, slot_s: 0, units: 1, est_awgn_db: -2.3 },
    Profile { id: 7, name: "Voice-1200", kind: Kind::Ofdm { bps: 2, nsym: 24 }, n: 1728, k: 900, reps: 1, slot_s: 0, units: 1, est_awgn_db: 0.0 },
];
pub fn get(id: usize) -> Option<&'static Profile> { PROFILES.get(id) }
impl Profile {
    /// Data symbols of one block (chirp: ceil(n / sf); OFDM: OFDM symbols).
    pub fn block_symbols(&self) -> usize {
        match self.kind {
            Kind::Chirp { sf, .. } => self.n.div_ceil(sf),
            Kind::Ofdm { nsym, .. } => nsym,
        }
    }
    /// Samples (at 2400/s) of one data symbol including its guard / cyclic prefix.
    pub fn symbol_samples(&self) -> usize {
        match self.kind {
            Kind::Chirp { sf, guard } => (1usize << sf) + guard,
            Kind::Ofdm { .. } => 60,
        }
    }
}
