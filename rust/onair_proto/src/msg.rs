// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! AIS message decoding (ITU-R M.1371-5 annex 8, field offsets as in the gpsd AIVDM document).

use crate::bits::{get_s, get_text, get_u};
use crate::mmsi::class_of_mmsi;

pub const NAME_CAP: usize = 48;
pub const CALL_CAP: usize = 8;
pub const DEST_CAP: usize = 24;
pub const VENDOR_CAP: usize = 4;

/// Mirror of the C++ `AisMsg`, flat and `repr(C)` (see core/include/dect2/rs/proto.h). Strings are fixed buffers with a length; the text
/// of type 14 is fetched by the caller (`text_bits` bits from bit 40) because its length is not bounded.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Msg {
    pub valid: u8,
    pub has_pos: u8,
    pub has_rot: u8,
    pub off_position: u8,
    pub virtual_aid: u8,
    pub name_len: u8,
    pub callsign_len: u8,
    pub destination_len: u8,
    pub vendor_len: u8,
    pub typ: i32,
    pub repeat: i32,
    pub mmsi: u32,
    pub cls: i32,
    pub lat: f64,
    pub lon: f64,
    pub sog: f32,
    pub cog: f32,
    pub heading: i32,
    pub rot_deg_min: f32,
    pub nav_status: i32,
    pub second: i32,
    pub imo: u32,
    pub ship_type: i32,
    pub dim_a: i32,
    pub dim_b: i32,
    pub dim_c: i32,
    pub dim_d: i32,
    pub eta_month: i32,
    pub eta_day: i32,
    pub eta_hour: i32,
    pub eta_min: i32,
    pub draught_m: f32,
    pub epfd: i32,
    pub part_no: i32,
    pub aid_type: i32,
    pub altitude_m: i32,
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub minute: i32,
    pub sec: i32,
    pub dac: i32,
    pub fi: i32,
    pub dest_mmsi: u32,
    pub text_bits: usize,
    pub bits: usize,
    pub name: [u8; NAME_CAP],
    pub callsign: [u8; CALL_CAP],
    pub destination: [u8; DEST_CAP],
    pub vendor: [u8; VENDOR_CAP],
}

impl Msg {
    /// The same defaults as `AisMsg()` in C++.
    pub fn new() -> Msg {
        Msg {
            valid: 0,
            has_pos: 0,
            has_rot: 0,
            off_position: 0,
            virtual_aid: 0,
            name_len: 0,
            callsign_len: 0,
            destination_len: 0,
            vendor_len: 0,
            typ: 0,
            repeat: 0,
            mmsi: 0,
            cls: 5,
            lat: 0.0,
            lon: 0.0,
            sog: -1.0,
            cog: -1.0,
            heading: -1,
            rot_deg_min: 0.0,
            nav_status: -1,
            second: 60,
            imo: 0,
            ship_type: -1,
            dim_a: 0,
            dim_b: 0,
            dim_c: 0,
            dim_d: 0,
            eta_month: 0,
            eta_day: 0,
            eta_hour: 24,
            eta_min: 60,
            draught_m: 0.0,
            epfd: 0,
            part_no: -1,
            aid_type: -1,
            altitude_m: -1,
            year: 0,
            month: 0,
            day: 0,
            hour: 24,
            minute: 60,
            sec: 60,
            dac: -1,
            fi: -1,
            dest_mmsi: 0,
            text_bits: 0,
            bits: 0,
            name: [0; NAME_CAP],
            callsign: [0; CALL_CAP],
            destination: [0; DEST_CAP],
            vendor: [0; VENDOR_CAP],
        }
    }
}

impl Default for Msg {
    fn default() -> Msg {
        Msg::new()
    }
}

const LON_NA: i32 = 0x6791AC0;
const LAT_NA: i32 = 0x3412140;

fn lon_of(raw: i32) -> f64 {
    if raw == LON_NA {
        181.0
    } else {
        raw as f64 / 600000.0
    }
}
fn lat_of(raw: i32) -> f64 {
    if raw == LAT_NA {
        91.0
    } else {
        raw as f64 / 600000.0
    }
}
fn within(x: f64, lim: f64) -> bool {
    x <= lim && x >= -lim
}

pub fn rate_of_turn(raw: i32) -> f64 {
    if raw == -128 {
        return f64::NAN;
    }
    let v = raw as f64 / 4.733;
    let s = if raw < 0 { -1.0 } else { 1.0 };
    s * if raw == 127 || raw == -127 { 720.0 } else { v * v }
}

fn pos(m: &mut Msg, b: &[u8], lo_at: usize, la_at: usize) {
    let lo = get_s(b, lo_at, 28);
    let la = get_s(b, la_at, 27);
    m.has_pos = (lo != LON_NA && la != LAT_NA && within(lon_of(lo), 180.0) && within(lat_of(la), 90.0)) as u8;
    m.lon = lon_of(lo);
    m.lat = lat_of(la);
}

fn sog10(raw: u32) -> f32 {
    if raw == 1023 {
        -1.0
    } else {
        raw as f32 / 10.0
    }
}
fn cog10(raw: u32) -> f32 {
    if raw >= 3600 {
        -1.0
    } else {
        raw as f32 / 10.0
    }
}
fn heading(raw: u32) -> i32 {
    if raw >= 360 {
        -1
    } else {
        raw as i32
    }
}

/// Decodes a payload (without FCS). Always fills the header (as far as 38 bits are there); returns `valid`.
pub fn decode_message(b: &[u8], m: &mut Msg) -> bool {
    *m = Msg::new();
    m.bits = b.len();
    if b.len() < 38 {
        return false;
    }
    m.typ = get_u(b, 0, 6) as i32;
    m.repeat = get_u(b, 6, 2) as i32;
    m.mmsi = get_u(b, 8, 30);
    m.cls = class_of_mmsi(m.mmsi, m.typ);
    let n = b.len();
    let u = |p: usize, l: usize| get_u(b, p, l) as i32;
    match m.typ {
        1..=3 => {
            if n != 168 {
                return false;
            }
            m.nav_status = u(38, 4);
            let rot = get_s(b, 42, 8);
            m.has_rot = (rot != -128) as u8;
            if rot != -128 {
                m.rot_deg_min = rate_of_turn(rot) as f32;
            }
            m.sog = sog10(get_u(b, 50, 10));
            pos(m, b, 61, 89);
            m.cog = cog10(get_u(b, 116, 12));
            m.heading = heading(get_u(b, 128, 9));
            m.second = u(137, 6);
            m.valid = 1;
        }
        4 | 11 => {
            if n != 168 {
                return false;
            }
            m.year = u(38, 14);
            m.month = u(52, 4);
            m.day = u(56, 5);
            m.hour = u(61, 5);
            m.minute = u(66, 6);
            m.sec = u(72, 6);
            pos(m, b, 79, 107);
            m.epfd = u(134, 4);
            m.valid = 1;
        }
        5 => {
            if !(420..=424).contains(&n) {
                return false;
            }
            m.imo = get_u(b, 40, 30);
            m.callsign_len = get_text(b, 70, 42, &mut m.callsign) as u8;
            m.name_len = get_text(b, 112, 120, &mut m.name) as u8;
            m.ship_type = u(232, 8);
            m.dim_a = u(240, 9);
            m.dim_b = u(249, 9);
            m.dim_c = u(258, 6);
            m.dim_d = u(264, 6);
            m.epfd = u(270, 4);
            m.eta_month = u(274, 4);
            m.eta_day = u(278, 5);
            m.eta_hour = u(283, 5);
            m.eta_min = u(288, 6);
            m.draught_m = get_u(b, 294, 8) as f32 / 10.0;
            m.destination_len = get_text(b, 302, 120, &mut m.destination) as u8;
            m.valid = 1;
        }
        6 => {
            if n < 72 {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            if n >= 88 {
                m.dac = u(72, 10);
                m.fi = u(82, 6);
            }
            m.valid = 1;
        }
        8 => {
            if n < 56 {
                return false;
            }
            m.dac = u(40, 10);
            m.fi = u(50, 6);
            m.valid = 1;
        }
        9 => {
            if n != 168 {
                return false;
            }
            let alt = get_u(b, 38, 12);
            m.altitude_m = if alt == 4095 { -1 } else { alt as i32 };
            let sp = get_u(b, 50, 10);
            m.sog = if sp == 1023 { -1.0 } else { sp as f32 };
            pos(m, b, 61, 89);
            m.cog = cog10(get_u(b, 116, 12));
            m.second = u(128, 6);
            m.valid = 1;
        }
        14 => {
            if n < 40 {
                return false;
            }
            m.text_bits = (n - 40) / 6 * 6;
            m.valid = 1;
        }
        18 | 19 => {
            if (m.typ == 18 && n != 168) || (m.typ == 19 && n != 312) {
                return false;
            }
            m.sog = sog10(get_u(b, 46, 10));
            pos(m, b, 57, 85);
            m.cog = cog10(get_u(b, 112, 12));
            m.heading = heading(get_u(b, 124, 9));
            m.second = u(133, 6);
            if m.typ == 19 {
                m.name_len = get_text(b, 143, 120, &mut m.name) as u8;
                m.ship_type = u(263, 8);
                m.dim_a = u(271, 9);
                m.dim_b = u(280, 9);
                m.dim_c = u(289, 6);
                m.dim_d = u(295, 6);
                m.epfd = u(301, 4);
            }
            m.valid = 1;
        }
        21 => {
            if !(272..=360).contains(&n) {
                return false;
            }
            m.aid_type = u(38, 5);
            let mut len = get_text(b, 43, 120, &mut m.name);
            pos(m, b, 164, 192);
            m.dim_a = u(219, 9);
            m.dim_b = u(228, 9);
            m.dim_c = u(237, 6);
            m.dim_d = u(243, 6);
            m.epfd = u(249, 4);
            m.second = u(253, 6);
            m.off_position = (get_u(b, 259, 1) != 0) as u8;
            m.virtual_aid = (get_u(b, 269, 1) != 0) as u8;
            if n > 272 {
                // name extension: up to 14 more characters, appended to the (trimmed) name
                let mut ext = [0u8; 16];
                let el = get_text(b, 272, (n - 272) / 6 * 6, &mut ext);
                for &c in &ext[..el] {
                    if len < NAME_CAP {
                        m.name[len] = c;
                        len += 1;
                    }
                }
            }
            m.name_len = len as u8;
            m.valid = 1;
        }
        24 => {
            if !(160..=168).contains(&n) {
                return false;
            }
            m.part_no = u(38, 2);
            if m.part_no == 0 {
                m.name_len = get_text(b, 40, 120, &mut m.name) as u8;
                m.valid = 1;
            } else if m.part_no == 1 {
                m.ship_type = u(40, 8);
                m.vendor_len = get_text(b, 48, 18, &mut m.vendor) as u8;
                m.callsign_len = get_text(b, 90, 42, &mut m.callsign) as u8;
                if m.mmsi / 10_000_000 == 98 {
                    m.dest_mmsi = get_u(b, 132, 30);
                } else {
                    m.dim_a = u(132, 9);
                    m.dim_b = u(141, 9);
                    m.dim_c = u(150, 6);
                    m.dim_d = u(156, 6);
                }
                m.valid = 1;
            }
        }
        27 => {
            if n != 96 && n != 168 {
                return false;
            }
            m.nav_status = u(40, 4);
            let lo = get_s(b, 44, 18);
            let la = get_s(b, 62, 17);
            m.has_pos = (lo != 181 * 600 && la != 91 * 600 && within(lo as f64 / 600.0, 180.0) && within(la as f64 / 600.0, 90.0)) as u8;
            m.lon = lo as f64 / 600.0;
            m.lat = la as f64 / 600.0;
            let sp = get_u(b, 79, 6);
            m.sog = if sp == 63 { -1.0 } else { sp as f32 };
            let c = get_u(b, 85, 9);
            m.cog = if c >= 360 { -1.0 } else { c as f32 };
            m.valid = 1;
        }
        7 | 13 => {
            if !(72..=168).contains(&n) {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            m.valid = 1;
        }
        10 => {
            if n < 72 {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            m.valid = 1;
        }
        12 => {
            if !(72..=1008).contains(&n) {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            m.text_bits = (n - 72) / 6 * 6;
            m.valid = 1;
        }
        15 => {
            if !(88..=168).contains(&n) {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            m.valid = 1;
        }
        16 => {
            if !(96..=168).contains(&n) {
                return false;
            }
            m.dest_mmsi = get_u(b, 40, 30);
            m.valid = 1;
        }
        17 => {
            if !(80..=816).contains(&n) {
                return false;
            }
            let lo = get_s(b, 40, 18);
            let la = get_s(b, 58, 17);
            m.has_pos = (lo != 181 * 600 && la != 91 * 600 && within(lo as f64 / 600.0, 180.0) && within(la as f64 / 600.0, 90.0)) as u8;
            m.lon = lo as f64 / 600.0;
            m.lat = la as f64 / 600.0;
            m.valid = 1;
        }
        20 => {
            if !(72..=186).contains(&n) {
                return false;
            }
            m.valid = 1;
        }
        22 => {
            if !(160..=176).contains(&n) {
                return false;
            }
            m.valid = 1;
        }
        23 => {
            if !(152..=176).contains(&n) {
                return false;
            }
            m.valid = 1;
        }
        25 => {
            if !(40..=168).contains(&n) {
                return false;
            }
            m.valid = 1;
        }
        26 => {
            if !(60..=1064).contains(&n) {
                return false;
            }
            m.valid = 1;
        }
        _ => {}
    }
    m.valid != 0
}
