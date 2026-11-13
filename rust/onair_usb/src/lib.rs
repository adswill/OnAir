// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir USB drivers: the RTL-SDR protocol layer (RTL2832U registers, R820T / R820T2 / R828D and E4000 tuners) as pure logic over a [`UsbTransfer`]
//! trait, so one implementation runs over libusb on a desktop and over an Android `UsbDeviceConnection` file descriptor from JNI.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

mod e4k;
pub mod fake;
pub mod fuzz;
mod r82xx;
pub mod rtl;
pub mod transfer;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;

pub use rtl::{RtlSdr, Tuner};
pub use transfer::{UsbString, UsbTransfer};
