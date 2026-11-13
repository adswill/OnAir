// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_net` (core/include/dect2/rs/net.h). Every export is `onair_net_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Text writers return the length the text has (also when the buffer was too short: nothing is written past it), `usize::MAX` marks a failure. Null pointers are empty
//! input / failure, never a crash.
#![allow(clippy::missing_safety_doc)]

use crate::flatjson::{self, Entry};
use crate::http::{self, Field, Head};
use crate::radios::{self, Radio};
use crate::rtltcp::{self, Greeting};
use crate::sdrpp::{self, UiCounts, Widget};
use crate::{hash, spy, url, ws, Span};
use core::ffi::c_char;

const FAIL: usize = usize::MAX;

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

unsafe fn wr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// Copies `s` to the buffer as far as it fits; returns the full length.
unsafe fn put_str(s: &str, out: *mut u8, cap: usize) -> usize {
    let o = wr(out, cap);
    let n = s.len().min(o.len());
    o[..n].copy_from_slice(&s.as_bytes()[..n]);
    s.len()
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_net_version() -> u32 {
    1
}

/// A client WebSocket frame as `onair_net_ws_parse_client` reports it.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct WsClientOut {
    /// 0 need more bytes, 1 close the connection with `code`, 2 a whole frame.
    pub kind: u32,
    /// Close status code (kind 1).
    pub code: u32,
    /// Opcode (kind 2).
    pub op: u32,
    /// FIN bit (kind 2).
    pub fin: u32,
    /// Bytes taken from the input (kind 2).
    pub used: usize,
    /// Payload bytes written (kind 2).
    pub len: usize,
}

/// sizeof of the record structs in the order http Head, http Field, rtl_tcp Greeting, sdrpp Widget, sdrpp UiCounts, Radio, flat JSON Entry, WsClientOut.
#[no_mangle]
pub unsafe extern "C" fn onair_net_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let v = [
        core::mem::size_of::<Head>(),
        core::mem::size_of::<Field>(),
        core::mem::size_of::<Greeting>(),
        core::mem::size_of::<Widget>(),
        core::mem::size_of::<UiCounts>(),
        core::mem::size_of::<Radio>(),
        core::mem::size_of::<Entry>(),
        core::mem::size_of::<WsClientOut>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------------- SHA-1, base64

/// SHA-1 of the data into `out` (20 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_net_sha1(data: *const u8, n: usize, out: *mut u8) {
    if out.is_null() {
        return;
    }
    wr(out, 20).copy_from_slice(&hash::sha1(rd(data, n)));
}

/// Base64 (with padding) of the data; returns its length (`4 * ceil(n / 3)`), `usize::MAX` when the buffer is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_net_base64(data: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    hash::base64(rd(data, n), wr(out, cap))
}

/// The `Sec-WebSocket-Accept` value (28 bytes) for a client key.
#[no_mangle]
pub unsafe extern "C" fn onair_net_ws_accept(key: *const u8, n: usize, out: *mut u8) {
    if out.is_null() {
        return;
    }
    let mut o = [0u8; 28];
    hash::ws_accept_key(rd(key, n), &mut o);
    wr(out, 28).copy_from_slice(&o);
}

// ------------------------------------------------------------------------------------------------------------------------------------------- WebSocket

/// Length of the server frame for a payload of `n` bytes.
#[no_mangle]
pub extern "C" fn onair_net_ws_frame_len(n: usize) -> usize {
    ws::frame_len(n)
}

/// Writes an unmasked server frame; returns its length or `usize::MAX` when the buffer is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_net_ws_frame(opcode: u32, payload: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    ws::frame(opcode as u8, rd(payload, n), wr(out, cap))
}

/// Parses the first client frame of the input (see `ws::parse_client`); the unmasked payload goes to `payload` (4096 bytes). Returns the kind (also in `out`).
#[no_mangle]
pub unsafe extern "C" fn onair_net_ws_parse_client(input: *const u8, n: usize, payload: *mut u8, cap: usize, out: *mut WsClientOut) -> u32 {
    let r = match ws::parse_client(rd(input, n), wr(payload, cap)) {
        ws::Client::Need => WsClientOut::default(),
        ws::Client::Close(c) => WsClientOut { kind: 1, code: c as u32, ..WsClientOut::default() },
        ws::Client::Frame { used, op, fin, len } => WsClientOut { kind: 2, code: 0, op: op as u32, fin: fin as u32, used, len },
    };
    if !out.is_null() {
        *out = r;
    }
    r.kind
}

// ------------------------------------------------------------------------------------------------------------------------------------------- URL text

/// Percent-decodes (and turns '+' into a space); returns the decoded length also when the buffer was too short.
#[no_mangle]
pub unsafe extern "C" fn onair_net_url_decode(s: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    url::url_decode(rd(s, n), wr(out, cap))
}

/// The decoded value of query parameter `key`; `*found` tells whether there is one; returns the value's length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_query_param(
    q: *const u8,
    qn: usize,
    key: *const u8,
    kn: usize,
    out: *mut u8,
    cap: usize,
    found: *mut bool,
) -> usize {
    let (f, n) = url::query_param(rd(q, qn), rd(key, kn), wr(out, cap));
    if !found.is_null() {
        *found = f;
    }
    n
}

/// Length of the host part of a Host header value ("127.0.0.1:8765" gives 9, "[::1]:80" gives 5).
#[no_mangle]
pub unsafe extern "C" fn onair_net_host_only_len(h: *const u8, n: usize) -> usize {
    url::host_only_len(rd(h, n))
}

/// `localhost`, `[::1]` or a dotted `127.x.y.z`.
#[no_mangle]
pub unsafe extern "C" fn onair_net_loopback_name(h: *const u8, n: usize) -> bool {
    url::loopback_name(rd(h, n))
}

/// Constant time equality.
#[no_mangle]
pub unsafe extern "C" fn onair_net_ct_equal(a: *const u8, an: usize, b: *const u8, bn: usize) -> bool {
    url::ct_equal(rd(a, an), rd(b, bn))
}

/// The reason phrase of an HTTP status code; returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_status_text(code: i32, out: *mut u8, cap: usize) -> usize {
    put_str(url::status_text(code), out, cap)
}

// ------------------------------------------------------------------------------------------------------------------------------------------- flat JSON

/// Parses a flat JSON object: up to 32 entries and an arena of at least `n` bytes; returns the number of entries or -1 for anything malformed.
#[no_mangle]
pub unsafe extern "C" fn onair_net_flatjson_parse(
    t: *const u8,
    n: usize,
    entries: *mut Entry,
    ecap: usize,
    arena: *mut u8,
    acap: usize,
) -> i32 {
    match flatjson::parse(rd(t, n), wr(entries, ecap), wr(arena, acap)) {
        Some(k) => k as i32,
        None => -1,
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------------- HTTP request head

/// Looks for the end of the request head in the bytes received: returns 0 (need more), 1 (found: `*end` is the offset of the CR LF CR LF) or 2 (over 8192 bytes: 431).
#[no_mangle]
pub unsafe extern "C" fn onair_net_http_scan(buf: *const u8, n: usize, end: *mut usize) -> u32 {
    let (s, e) = http::scan(rd(buf, n));
    if !end.is_null() {
        *end = e;
    }
    s
}

/// Parses the request head (without the empty line); up to 48 header fields go to `fields` (`cap` of them). Fills `out`.
#[no_mangle]
pub unsafe extern "C" fn onair_net_http_parse(head: *const u8, n: usize, fields: *mut Field, cap: usize, out: *mut Head) {
    let h = http::parse(rd(head, n), wr(fields, cap));
    if !out.is_null() {
        *out = h;
    }
}

/// The status code (`*status`) and message of a head error id; returns the message length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_http_error(id: u32, status: *mut u32, out: *mut u8, cap: usize) -> usize {
    let (c, m) = http::error(id);
    if !status.is_null() {
        *status = c;
    }
    put_str(m, out, cap)
}

// ------------------------------------------------------------------------------------------------------------------------------------------- rtl_tcp

/// Reads the 12 byte greeting; false when it is not an rtl_tcp greeting.
#[no_mangle]
pub unsafe extern "C" fn onair_net_rtltcp_greeting(h: *const u8, n: usize, out: *mut Greeting) -> bool {
    match rtltcp::greeting(rd(h, n)) {
        Some(g) => {
            if !out.is_null() {
                *out = g;
            }
            true
        }
        None => false,
    }
}

/// The gains (tenths of a dB) of a tuner type; returns their number (up to `cap` are written).
#[no_mangle]
pub unsafe extern "C" fn onair_net_rtltcp_gains(tuner: i32, out: *mut i32, cap: usize) -> usize {
    let g = rtltcp::tuner_gains(tuner);
    let o = wr(out, cap);
    for (d, s) in o.iter_mut().zip(g) {
        *d = *s as i32;
    }
    g.len()
}

/// The tuner's name; returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_rtltcp_tuner_name(t: i32, out: *mut u8, cap: usize) -> usize {
    put_str(rtltcp::tuner_name(t), out, cap)
}

/// The 5 byte command.
#[no_mangle]
pub unsafe extern "C" fn onair_net_rtltcp_command(cmd: u32, v: u32, out: *mut u8) {
    if !out.is_null() {
        wr(out, 5).copy_from_slice(&rtltcp::command(cmd as u8, v));
    }
}

/// Splits "host:port"; false when there is no host or the port is not 1 to 65535. `host` is the range of the host in the input.
#[no_mangle]
pub unsafe extern "C" fn onair_net_split_host_port(a: *const u8, n: usize, host: *mut Span, port: *mut i32) -> bool {
    match rtltcp::split_host_port(rd(a, n)) {
        Some((h, p)) => {
            if !host.is_null() {
                *host = h;
            }
            if !port.is_null() {
                *port = p;
            }
            true
        }
        None => false,
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------------- SpyServer

/// The 12 words of DEVICE_INFO.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_device_info(b: *const u8, n: usize, out: *mut u32) {
    if !out.is_null() {
        wr(out, 12).copy_from_slice(&spy::device_info(rd(b, n)));
    }
}

/// The 9 words of CLIENT_SYNC.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_client_sync(b: *const u8, n: usize, out: *mut u32) {
    if !out.is_null() {
        wr(out, 9).copy_from_slice(&spy::client_sync(rd(b, n)));
    }
}

/// A command (type, body size, body); returns its length or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_command(ty: u32, body: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    spy::command(ty, rd(body, n), wr(out, cap))
}

/// The HELLO command.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_hello(out: *mut u8, cap: usize) -> usize {
    spy::hello(wr(out, cap))
}

/// SET_SETTING.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_set_setting(id: u32, v: u32, out: *mut u8, cap: usize) -> usize {
    spy::set_setting(id, v, wr(out, cap))
}

/// Reads a 20 byte message header; false when it is short or announces a body over 1 MiB.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_header(h: *const u8, n: usize, ty: *mut u32, flags: *mut u32, size: *mut u32) -> bool {
    match spy::header(rd(h, n)) {
        Some((t, f, s)) => {
            if !ty.is_null() && !flags.is_null() && !size.is_null() {
                *ty = t;
                *flags = f;
                *size = s;
            }
            true
        }
        None => false,
    }
}

/// The rates of a server (fastest first); returns how many there are (up to `cap` are written).
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_rates(max_rate: u32, dec_stages: u32, min_dec: u32, out: *mut f64, cap: usize) -> usize {
    spy::rates(max_rate, dec_stages, min_dec, wr(out, cap))
}

/// The device type's name; returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_spy_dev_name(t: u32, out: *mut u8, cap: usize) -> usize {
    put_str(spy::dev_name(t), out, cap)
}

// ------------------------------------------------------------------------------------------------------------------------------------------- SDR++ server

mod libc {
    use core::ffi::c_char;
    extern "C" {
        pub fn strtod(s: *const c_char, end: *mut *mut c_char) -> f64;
        pub fn strlen(s: *const c_char) -> usize;
    }
}

/// Reads the menu draw list. Fills the widget, item and label arrays (sizes that always suffice: `n / 3 + 1`, `n + 1`, `9 * n + 64`); `out->ok` is 0 for a damaged list
/// or a too small array.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_parse_ui(
    p: *const u8,
    n: usize,
    widgets: *mut Widget,
    wcap: usize,
    items: *mut Span,
    icap: usize,
    arena: *mut u8,
    acap: usize,
    out: *mut UiCounts,
) {
    let c = sdrpp::parse_ui(rd(p, n), wr(widgets, wcap), wr(items, icap), wr(arena, acap));
    if !out.is_null() {
        *out = c;
    }
}

/// Which of the words sr, bw, lna, vga, mix, mixer, if, lin, sens an id holds (bits 0 to 8 in that order).
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_id_flags(id: *const u8, n: usize) -> u32 {
    sdrpp::id_flags(rd(id, n))
}

/// The readable name made from a control's id; returns its length or `usize::MAX` when the buffer is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_label_from_id(id: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    sdrpp::label_from_id(rd(id, n), wr(out, cap))
}

/// "10.0MHz", "2.5MHz", "250KHz", "2.048 MSPS", "48000" (a NUL terminated string) in Hz; 0 when it is not a rate. The number is read by the C library's `strtod`.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_rate_text(t: *const c_char) -> f64 {
    if t.is_null() {
        return 0.0;
    }
    let mut s = t;
    while *s == b' ' as c_char {
        s = s.add(1);
    }
    let mut end: *mut c_char = core::ptr::null_mut();
    let v = libc::strtod(s, &mut end);
    if core::ptr::eq(end as *const c_char, s) || v.is_nan() || v <= 0.0 {
        return 0.0;
    }
    let unit = rd(end as *const u8, libc::strlen(end));
    let m = sdrpp::rate_unit(unit);
    if m == 0.0 {
        0.0
    } else {
        v * m
    }
}

/// A command packet (header, command id, argument); returns its length or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_command(cmd: u32, arg: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    sdrpp::command(cmd, rd(arg, n), wr(out, cap))
}

/// The argument of a UI_ACTION command (value type 1 bool, 2 int, else float).
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_ui_action(
    id: *const u8,
    n: usize,
    ty: u32,
    i: i32,
    f: f32,
    b: u32,
    sync: u32,
    out: *mut u8,
    cap: usize,
) -> usize {
    let b = b != 0;
    let sync = sync != 0;
    sdrpp::ui_action_arg(rd(id, n), ty as u8, i, f, b, sync, wr(out, cap))
}

/// Reads an 8 byte packet header: the type and the size of the body that follows; false when it is invalid.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_packet_header(h: *const u8, n: usize, ty: *mut u32, body: *mut u32) -> bool {
    match sdrpp::packet_header(rd(h, n)) {
        Some((t, b)) => {
            if !ty.is_null() && !body.is_null() {
                *ty = t;
                *body = b;
            }
            true
        }
        None => false,
    }
}

/// The text of an error packet's code (0 to 3); returns its length, `usize::MAX` for another code.
#[no_mangle]
pub unsafe extern "C" fn onair_net_sdrpp_error_text(c: u32, out: *mut u8, cap: usize) -> usize {
    match u8::try_from(c).ok().and_then(sdrpp::error_text) {
        Some(t) => put_str(t, out, cap),
        None => FAIL,
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------------- network radios

/// Parses the radio list text; returns the number of radios or `usize::MAX` when an array is too small (`;` count + 1 radios and `n` arena bytes suffice).
#[no_mangle]
pub unsafe extern "C" fn onair_net_radios_parse(
    s: *const u8,
    n: usize,
    radios: *mut Radio,
    rcap: usize,
    arena: *mut u8,
    acap: usize,
) -> usize {
    radios::parse(rd(s, n), wr(radios, rcap), wr(arena, acap))
}

/// Formats a list (the ranges of `host` and `remote` index `pool`); returns the full length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_radios_format(r: *const Radio, n: usize, pool: *const u8, pn: usize, out: *mut u8, cap: usize) -> usize {
    let rs: &[Radio] = if r.is_null() || n == 0 { &[] } else { core::slice::from_raw_parts(r, n) };
    radios::format(rs, rd(pool, pn), wr(out, cap))
}

/// Cleans a typed host name; returns the length or `usize::MAX` when the buffer is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_net_radio_clean_host(h: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    radios::clean_host(rd(h, n), wr(out, cap))
}

/// The name of a radio type in the radio list; returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_radio_type_name(t: u32, out: *mut u8, cap: usize) -> usize {
    put_str(radios::type_name(t), out, cap)
}

/// The default port of a radio type.
#[no_mangle]
pub extern "C" fn onair_net_radio_default_port(t: u32) -> i32 {
    radios::default_port(t)
}

/// The default host of a radio type; returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_net_radio_default_host(t: u32, out: *mut u8, cap: usize) -> usize {
    put_str(radios::default_host(t), out, cap)
}

/// The port connected to for a radio.
#[no_mangle]
pub extern "C" fn onair_net_radio_port(t: u32, port: i32) -> i32 {
    radios::port_of(t, port)
}
