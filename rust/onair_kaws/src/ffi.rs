// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_kaws` (core/include/dect2/rs/kaws.h). Every export is `onair_kaws_*`; scalars are i32 / u32 / f32 / f64, buffers are caller owned.
//! Bits are one per byte; complex samples are interleaved f32 (re, im) at 2400/s. A null pointer or a bad size gives -1.
#![allow(clippy::missing_safety_doc)]

use crate::cplx::C;
use crate::{audio, chirp, crc, lattice, link, profile, rng, watterson};
use alloc::vec::Vec;

#[no_mangle]
pub extern "C" fn onair_kaws_version() -> u32 { 1 }

/// Information bits k of profile `id` (0..7), or -1.
#[no_mangle]
pub extern "C" fn onair_kaws_profile_k(id: i32) -> i32 { if id < 0 { -1 } else { profile::get(id as usize).map(|p| p.k as i32).unwrap_or(-1) } }
/// Coded block length n of profile `id`, or -1.
#[no_mangle]
pub extern "C" fn onair_kaws_profile_n(id: i32) -> i32 { if id < 0 { -1 } else { profile::get(id as usize).map(|p| p.n as i32).unwrap_or(-1) } }

/// CRC-16-CCITT (init 0xFFFF) of `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_crc16(data: *const u8, len: i32) -> u32 {
    if data.is_null() || len < 0 { return 0; }
    crc::crc16(core::slice::from_raw_parts(data, len as usize)) as u32
}
/// CRC-24 (OpenPGP polynomial) of `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_crc24(data: *const u8, len: i32) -> u32 {
    if data.is_null() || len < 0 { return 0; }
    crc::crc24(core::slice::from_raw_parts(data, len as usize))
}

/// The weave directions of the first `count` symbols (1 rising, -1 falling) into `out`.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_weave(count: i32, out: *mut i32) -> i32 {
    if out.is_null() || count < 0 { return -1; }
    let mut w = chirp::Weave::new();
    for i in 0..count as usize { *out.add(i) = w.next_dir(); }
    count
}

/// Number of baseband samples of a burst with `nblocks` blocks of profile `id`, or -1.
#[no_mangle]
pub extern "C" fn onair_kaws_burst_len(id: i32, nblocks: i32) -> i32 {
    if id < 0 || nblocks < 1 { return -1; }
    match profile::get(id as usize) { Some(p) => link::layout(p, nblocks as usize).total as i32, None => -1 }
}

/// Build a burst: `info` = nblocks x k bits; `iq` receives 2 x burst_len floats. Returns the number of samples, or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_build_burst(id: i32, nblocks: i32, info: *const u8, iq: *mut f32, cap_samples: i32) -> i32 {
    if info.is_null() || iq.is_null() || id < 0 || nblocks < 1 { return -1; }
    let p = match profile::get(id as usize) { Some(p) => p, None => return -1 };
    let infos: Vec<Vec<u8>> = (0..nblocks as usize).map(|b| core::slice::from_raw_parts(info.add(b * p.k), p.k).to_vec()).collect();
    let tx = match link::build_burst(p, &infos) { Some(t) => t, None => return -1 };
    if tx.len() > cap_samples as usize { return -1; }
    for (i, v) in tx.iter().enumerate() { *iq.add(2 * i) = v.re; *iq.add(2 * i + 1) = v.im; }
    tx.len() as i32
}

unsafe fn read_iq(iq: *const f32, n: i32) -> Vec<C> { (0..n.max(0) as usize).map(|i| C::new(*iq.add(2 * i), *iq.add(2 * i + 1))).collect() }

/// Receive a burst. `rx` = n samples; the first lattice unit starts in [search_lo, search_hi]. `out_info` gets nblocks x k bits (0 where undecoded),
/// `out_iters` nblocks values (LDPC iterations, -1 = not decoded), `out_stats` 4 doubles (detection statistic, timing in samples, offset in Hz, noise power).
/// Returns 1 when the preamble was detected, 0 when not, -1 on a bad argument.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_rx_burst(id: i32, nblocks: i32, iq: *const f32, n: i32, search_lo: i32, search_hi: i32, out_info: *mut u8, out_iters: *mut i32, out_stats: *mut f64) -> i32 {
    if iq.is_null() || out_info.is_null() || out_iters.is_null() || out_stats.is_null() || id < 0 || nblocks < 1 || search_lo < 0 || search_hi < search_lo || n <= 0 { return -1; }
    let p = match profile::get(id as usize) { Some(p) => p, None => return -1 };
    let rx = read_iq(iq, n);
    // the search range is clamped so that a whole lattice unit still fits in the stream; nothing to search = no burst
    let hi = core::cmp::min(search_hi as usize, rx.len().saturating_sub(8401));
    if rx.len() < 12000 { for b in 0..nblocks as usize { *out_iters.add(b) = -1; for i in 0..p.k { *out_info.add(b * p.k + i) = 0; } } for i in 0..4 { *out_stats.add(i) = 0.0; } return 0; }
    if hi < search_lo as usize {
        for b in 0..nblocks as usize { *out_iters.add(b) = -1; for i in 0..p.k { *out_info.add(b * p.k + i) = 0; } }
        for i in 0..4 { *out_stats.add(i) = 0.0; }
        return 0;
    }
    let r = link::rx_burst(p, nblocks as usize, &rx, search_lo as usize, hi, p.est_awgn_db);
    for b in 0..nblocks as usize {
        let dst = out_info.add(b * p.k);
        if r.sync && b < r.blocks.len() && r.blocks[b].1 >= 0 {
            for i in 0..p.k { *dst.add(i) = r.blocks[b].0[i]; }
            *out_iters.add(b) = r.blocks[b].1;
        } else {
            for i in 0..p.k { *dst.add(i) = 0; }
            *out_iters.add(b) = -1;
        }
    }
    *out_stats = r.stat;
    *out_stats.add(1) = r.tau;
    *out_stats.add(2) = r.delta_hz;
    *out_stats.add(3) = r.snr_db;
    if r.sync { 1 } else { 0 }
}

/// Pass a burst through the Watterson channel. channel: 0 KW-AWGN, 1 KW-MOD, 2 KW-POOR. `iq_out` needs n + start_delay + tail + 64 samples. Returns the output samples or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_channel(iq: *const f32, n: i32, channel: i32, snr_db: f64, freq_offset_hz: f64, drift_hz_s: f64, start_delay: f64, tail: i32, seed_lo: u32, seed_hi: u32, iq_out: *mut f32, cap_samples: i32) -> i32 {
    if iq.is_null() || iq_out.is_null() || n <= 0 || tail < 0 || start_delay < 0.0 { return -1; }
    let spec = match channel { 0 => watterson::AWGN, 1 => watterson::MOD, 2 => watterson::POOR, _ => return -1 };
    let x = read_iq(iq, n);
    let mut o = watterson::Opts::new(spec, snr_db);
    o.freq_offset_hz = freq_offset_hz;
    o.drift_hz_s = drift_hz_s;
    o.start_delay = start_delay;
    o.tail = tail as usize;
    let mut r = rng::Rng::new(((seed_hi as u64) << 32) | seed_lo as u64);
    let y = watterson::apply(&x, &o, &mut r);
    if y.len() > cap_samples as usize { return -1; }
    for (i, v) in y.iter().enumerate() { *iq_out.add(2 * i) = v.re; *iq_out.add(2 * i + 1) = v.im; }
    y.len() as i32
}

/// Baseband to 12 kHz real audio (5 x samples), largest sample = peak. Returns the audio samples or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_to_audio(iq: *const f32, n: i32, peak: f32, out: *mut f32, cap: i32) -> i32 {
    if iq.is_null() || out.is_null() || n <= 0 { return -1; }
    let a = audio::to_audio(&read_iq(iq, n), peak);
    if a.len() > cap as usize { return -1; }
    for (i, v) in a.iter().enumerate() { *out.add(i) = *v; }
    a.len() as i32
}
/// 12 kHz real audio to baseband (n / 5 samples). Returns the sample count or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_from_audio(a: *const f32, n: i32, iq_out: *mut f32, cap_samples: i32) -> i32 {
    if a.is_null() || iq_out.is_null() || n <= 0 { return -1; }
    let v: Vec<f32> = (0..n as usize).map(|i| *a.add(i)).collect();
    let y = audio::from_audio(&v);
    if y.len() > cap_samples as usize { return -1; }
    for (i, s) in y.iter().enumerate() { *iq_out.add(2 * i) = s.re; *iq_out.add(2 * i + 1) = s.im; }
    y.len() as i32
}

/// One lattice unit (8400 samples) into `iq` (2 x 8400 floats). Returns 8400 or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kaws_lattice_unit(iq: *mut f32, cap_samples: i32) -> i32 {
    let u = lattice::unit();
    if iq.is_null() || (cap_samples as usize) < u.len() { return -1; }
    for (i, v) in u.iter().enumerate() { *iq.add(2 * i) = v.re; *iq.add(2 * i + 1) = v.im; }
    u.len() as i32
}
