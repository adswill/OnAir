// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The one static library the C++ code links (`libonair_rs.a`). A single staticlib avoids duplicate Rust runtime/`core` symbols, which
//! several per-module staticlibs would each carry. Every module crate keeps its C ABI in its own `ffi.rs`; the `extern "C"` items are
//! `#[no_mangle]` and therefore end up in this archive, and the `use` lines below make sure each crate is linked in even when no Rust
//! code calls it. This crate also owns the panic handler (members are `no_std` rlibs and define none).
//!
//! Feature `bare` (what CMake builds with) makes the crate `no_std` and installs the panic handler, so the archive carries no Rust `std`.
//! Without it (plain `cargo test`, clippy) the crate is ordinary std code, because test builds unwind and `no_std` needs panic=abort.
#![cfg_attr(feature = "bare", no_std)]

pub use onair_acars as acars;
pub use onair_adsb as adsb;
pub use onair_ax25 as ax25;
pub use onair_common as common;
pub use onair_dab as dab;
pub use onair_dmr as dmr;
pub use onair_drm as drm;
pub use onair_dstar as dstar;
pub use onair_dvbsi as dvbsi;
pub use onair_engine as engine;
pub use onair_fec as fec;
pub use onair_files as files;
pub use onair_hfdl as hfdl;
pub use onair_kaws as kaws;
pub use onair_kris as kris;
pub use onair_mesh as mesh;
pub use onair_net as net;
pub use onair_nxdn as nxdn;
pub use onair_p25 as p25;
pub use onair_pager as pager;
pub use onair_proto as proto;
pub use onair_ring as ring;
pub use onair_same as same;
pub use onair_tetra as tetra;
pub use onair_ts as ts;
pub use onair_uat as uat;
pub use onair_usb as usb;
pub use onair_vdl2 as vdl2;
pub use onair_wisun as wisun;

/// Panics must never unwind into C++: abort.
#[cfg(feature = "bare")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}

// The prebuilt `core` still names the unwinding personality routine; with panic=abort nothing ever calls it.
#[cfg(feature = "bare")]
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}
