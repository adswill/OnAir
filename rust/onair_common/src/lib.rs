// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Shared building blocks for the OnAir protocol crates. `no_std`, no allocation, no dependencies, and no `unsafe` except one `transmute` in `diag` (see there).
#![cfg_attr(not(any(test, feature = "std")), no_std)]
#![deny(unsafe_code)]

pub mod bits;
pub mod crc;
pub mod diag;
pub mod endian;
pub mod gray;
pub mod hex;
pub mod manchester;
pub mod nrzi;
pub mod parity;
pub mod ring;
pub mod text;
