// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir KAWS: the owner's HF weak-signal digital mode (2.4 kHz, UTC slotted; chat, beacon, very low rate voice). Spec: KAWS-SPEC-v0.1.
//!
//! This crate holds the signal processing: the cyclic-shift chirp modem (profiles P0-P4), the coherent OFDM modem (P5-P7), the woven-lattice preamble and
//! its detector, the closing weave, the LDPC codes, the bit interleaver, CRC-16/24, the Watterson HF channel simulator and the 12 kHz audio conversion.
//! Framing, the UTC slot scheduler and the ARQ state machine live in C++ (`core/src/kaws/`). All signals are complex baseband at 2400 samples/s
//! (one sample per chip); audio frequency = 1500 Hz + baseband frequency.
//! `no_std` with `alloc`; the C library math functions are declared in `mathf`. Only `ffi` and `mathf` use `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)]

extern crate alloc;

pub mod audio;
pub mod chirp;
pub mod cplx;
pub mod crc;
pub mod fft;
pub mod lattice;
pub mod ldpc;
pub mod link;
pub mod mathf;
pub mod ofdm;
pub mod profile;
pub mod rng;
pub mod watterson;

#[allow(unsafe_code)]
pub mod ffi;
