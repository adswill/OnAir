// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The SpyServer protocol 2.0.1700 (Airspy's spyserver, as SDR++'s spyserver_source speaks it): little endian; commands are a 32-bit type, a 32-bit body size and the
//! body; messages have a 20 byte header (protocol id, message type with the gain in dB in its top 16 bits, stream type, sequence number, body size).

/// Protocol version OnAir announces in HELLO: 2.0.1700.
pub const PROTOCOL_VERSION: u32 = (2 << 24) | 1700;
/// HELLO.
pub const CMD_HELLO: u32 = 0;
/// SET_SETTING.
pub const CMD_SET_SETTING: u32 = 2;
/// Largest message body the client accepts.
pub const MAX_BODY: u32 = 1 << 20;
/// Size of a message header.
pub const HEADER_LEN: usize = 20;

fn le32(b: &[u8], k: usize) -> u32 {
    u32::from_le_bytes([b[4 * k], b[4 * k + 1], b[4 * k + 2], b[4 * k + 3]])
}

/// The 12 words of DEVICE_INFO (device type, serial, maximum sample rate, maximum bandwidth, decimation stages, gain stages, maximum gain index, minimum and maximum
/// frequency, resolution, minimum decimation, forced IQ format); words past the end of the body read as 0.
pub fn device_info(b: &[u8]) -> [u32; 12] {
    let mut v = [0u32; 12];
    for (k, x) in v.iter_mut().enumerate() {
        if 4 * k + 4 <= b.len() {
            *x = le32(b, k);
        }
    }
    v
}

/// The 9 words of CLIENT_SYNC (can control, gain, device centre, IQ centre, ..., minimum and maximum IQ centre); words past the end read as 0.
pub fn client_sync(b: &[u8]) -> [u32; 9] {
    let mut v = [0u32; 9];
    for (k, x) in v.iter_mut().enumerate() {
        if 4 * k + 4 <= b.len() {
            *x = le32(b, k);
        }
    }
    v
}

/// Length of a command with a body of `n` bytes.
pub const fn command_len(n: usize) -> usize {
    8 + n
}

/// Writes a command (type, body size, body) to `out`; returns its length or `usize::MAX` when `out` is too short.
pub fn command(ty: u32, body: &[u8], out: &mut [u8]) -> usize {
    let t = command_len(body.len());
    if out.len() < t || body.len() > u32::MAX as usize {
        return usize::MAX;
    }
    out[..4].copy_from_slice(&ty.to_le_bytes());
    out[4..8].copy_from_slice(&(body.len() as u32).to_le_bytes());
    out[8..t].copy_from_slice(body);
    t
}

/// The HELLO command (protocol version and the client name "OnAir").
pub fn hello(out: &mut [u8]) -> usize {
    let mut body = [0u8; 9];
    body[..4].copy_from_slice(&PROTOCOL_VERSION.to_le_bytes());
    body[4..].copy_from_slice(b"OnAir");
    command(CMD_HELLO, &body, out)
}

/// SET_SETTING with a setting id and value.
pub fn set_setting(id: u32, v: u32, out: &mut [u8]) -> usize {
    let mut body = [0u8; 8];
    body[..4].copy_from_slice(&id.to_le_bytes());
    body[4..].copy_from_slice(&v.to_le_bytes());
    command(CMD_SET_SETTING, &body, out)
}

/// A message header: (type, flags, body size). None when the header is short or announces a body over `MAX_BODY`. The type is the low 16 bits of the message type word,
/// the flags (the gain in dB) its top 16 bits.
pub fn header(h: &[u8]) -> Option<(u32, u32, u32)> {
    if h.len() < HEADER_LEN {
        return None;
    }
    let mt = le32(h, 1);
    let size = le32(h, 4);
    if size > MAX_BODY {
        onair_common::diag_debug!(820056, "A SpyServer message announces a body of {} bytes, above the limit of {}, so the connection data is dropped.", size, MAX_BODY);
        return None;
    }
    Some((mt & 0xffff, mt >> 16, size))
}

/// The rates a server offers: its radio's full rate / 2^n for its decimation stages, the fastest first. Writes up to `out.len()` of them, returns how many there are
/// (at most 20).
pub fn rates(max_rate: u32, dec_stages: u32, min_dec: u32, out: &mut [f64]) -> usize {
    let stages = dec_stages.max(min_dec.wrapping_add(1));
    let mut n = 0;
    let mut d = min_dec;
    while d < stages && d < 20 {
        if n < out.len() {
            out[n] = max_rate as f64 / (1u32 << d) as f64;
        }
        n += 1;
        d += 1;
    }
    n
}

/// The device type's name.
pub fn dev_name(t: u32) -> &'static str {
    match t {
        1 => "Airspy",
        2 => "Airspy HF+",
        3 => "RTL-SDR",
        _ => "radio",
    }
}
