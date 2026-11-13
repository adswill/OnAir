// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Shared building blocks for the OnAir protocol crates. `no_std`, no allocation, no `unsafe`, no dependencies.
#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_code)]

pub mod bits;
pub mod crc;
pub mod endian;
pub mod gray;
pub mod hex;
pub mod manchester;
pub mod nrzi;
pub mod parity;
pub mod ring;
pub mod text;
