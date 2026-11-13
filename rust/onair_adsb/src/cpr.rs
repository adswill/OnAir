// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Compact position reporting, 17 bit, NZ = 15 (ICAO Annex 10 Vol. IV 3.1.2.8.6, DO-260B appendix A.1.7; worked examples in the Riddle).
#![allow(clippy::manual_range_contains)] // plain comparisons on purpose: a NaN must pass them like it does in the C++ original
use crate::ffi::math::{self, floor, fmod};

/// Euclidean-style modulo: the result has the sign of `y` (the standards' MOD).
fn modulo(x: f64, y: f64) -> f64 {
    let r = fmod(x, y);
    if r < 0.0 {
        r + y
    } else {
        r
    }
}

/// Transition latitudes: NL >= n up to |lat| = T[n] for n = 2 ..= 59 (Annex 10 / DO-260B table A-1; the formula acos(sqrt((1 - cos(pi/30)) /
/// (1 - cos(2 pi / n)))) in degrees gives the table's 10.47047130, 14.82817437, ... 87.0). Stored as literals (the same table is in the C++ implementation) so the zone changes cannot depend on the
/// libm; the first one is the table's exact 87.0 (NL = 2 up to and including 87 degrees). A test checks them against the formula.
#[allow(clippy::excessive_precision)]
pub static NL_THRESHOLDS: [f64; 58] = [
    8.70000000000000000e+01,
    8.65353699751210002e+01,
    8.57554162094441779e+01,
    8.48916619070208469e+01,
    8.39917356298056319e+01,
    8.30719944471981364e+01,
    8.21395698051060492e+01,
    8.11980134927194825e+01,
    8.02492321328051190e+01,
    7.92942822545692536e+01,
    7.83337408292274660e+01,
    7.73678946132818766e+01,
    7.63968439079446568e+01,
    7.54205625665335759e+01,
    7.44389341572513672e+01,
    7.34517744166786315e+01,
    7.24588454472894341e+01,
    7.14598647302898229e+01,
    7.04545107498759933e+01,
    6.94424263114402436e+01,
    6.84232202208333149e+01,
    6.73964677408466315e+01,
    6.63617100838261678e+01,
    6.53184530968208890e+01,
    6.42661652256743992e+01,
    6.32042747938192733e+01,
    6.21321665921033031e+01,
    6.10491777424635131e+01,
    5.99545927669402943e+01,
    5.88476377614845703e+01,
    5.77274735386611368e+01,
    5.65931875620591782e+01,
    5.54437844449504311e+01,
    5.42781747227290055e+01,
    5.30951615279600233e+01,
    5.18934246916876845e+01,
    5.06715016555383500e+01,
    4.94277643925568739e+01,
    4.81603912809662233e+01,
    4.68673325249874537e+01,
    4.55462672266023461e+01,
    4.41945495141927438e+01,
    4.28091401224355508e+01,
    4.13865183226023845e+01,
    3.99225668433386147e+01,
    3.84124189241225622e+01,
    3.68502510759352617e+01,
    3.52289959779638480e+01,
    3.35399343629848445e+01,
    3.17720970768107698e+01,
    2.99113568573180899e+01,
    2.79389871012190447e+01,
    2.58292470705877548e+01,
    2.35450448655707056e+01,
    2.10293949260284663e+01,
    1.81862635707133542e+01,
    1.48281743686867955e+01,
    1.04704712999684784e+01,
];

/// Number of longitude zones at this latitude (1 - 59).
pub fn nl(lat: f64) -> i32 {
    let a = math::fabs(lat);
    if a > 90.0 {
        return 1;
    }
    let mut nl = 59;
    for n in (2..=59).rev() {
        if a > NL_THRESHOLDS[n - 2] {
            nl = n as i32 - 1;
        } else {
            break;
        }
    }
    nl
}

const SCALE: f64 = 131072.0; // 2^17

/// A decoded position.
pub struct Pos {
    pub lat: f64,
    pub lon: f64,
}

fn wrap_lon(l: f64) -> f64 {
    modulo(l + 180.0, 360.0) - 180.0
}

/// Global decoding of an even and an odd airborne frame (17 bit values); `odd_latest` says which came last. `None` when a latitude falls outside
/// -90 .. 90. When the two latitudes sit in different NL zones the later frame's latitude is used (the spec drops such a pair; that would keep a whole
/// track over Lithuania / southern Sweden from ever getting a global fix, and the longitude is still unique).
pub fn global_airborne(lat_e: i32, lon_e: i32, lat_o: i32, lon_o: i32, odd_latest: bool) -> Option<Pos> {
    let le = lat_e as f64 / SCALE;
    let lo = lat_o as f64 / SCALE;
    let j = floor(59.0 * le - 60.0 * lo + 0.5);
    let mut ve = (360.0 / 60.0) * (modulo(j, 60.0) + le);
    let mut vo = (360.0 / 59.0) * (modulo(j, 59.0) + lo);
    if ve >= 270.0 {
        ve -= 360.0;
    }
    if vo >= 270.0 {
        vo -= 360.0;
    }
    // (written with plain comparisons, not a range: a NaN passes them like it does in the C++ original)
    if ve < -90.0 || ve > 90.0 || vo < -90.0 || vo > 90.0 {
        return None;
    }
    let rlat = if odd_latest { vo } else { ve };
    let nl = nl(rlat);
    let ni = (nl - odd_latest as i32).max(1);
    let m = floor((lon_e as f64 / SCALE) * (nl - 1) as f64 - (lon_o as f64 / SCALE) * nl as f64 + 0.5);
    let mut rlon = (360.0 / ni as f64) * (modulo(m, ni as f64) + (if odd_latest { lon_o } else { lon_e }) as f64 / SCALE);
    if rlon >= 180.0 {
        rlon -= 360.0;
    }
    Some(Pos { lat: rlat, lon: rlon })
}

/// Local decoding: the position nearest to the reference (valid within 180 NM for airborne, 45 NM for surface).
pub fn local_airborne(lat_cpr: i32, lon_cpr: i32, odd: bool, ref_lat: f64, ref_lon: f64) -> Option<Pos> {
    let i = if odd { 1.0 } else { 0.0 };
    let d_lat = 360.0 / (60.0 - i);
    let lc = lat_cpr as f64 / SCALE;
    let xc = lon_cpr as f64 / SCALE;
    let j = floor(ref_lat / d_lat) + floor(0.5 + modulo(ref_lat, d_lat) / d_lat - lc);
    let rlat = d_lat * (j + lc);
    if rlat < -90.0 || rlat > 90.0 {
        return None;
    }
    let d_lon = 360.0 / max_f(nl(rlat) as f64 - i, 1.0);
    let m = floor(ref_lon / d_lon) + floor(0.5 + modulo(ref_lon, d_lon) / d_lon - xc);
    let mut rlon = d_lon * (m + xc);
    if rlon >= 180.0 {
        rlon -= 360.0;
    }
    if rlon < -180.0 {
        rlon += 360.0;
    }
    Some(Pos { lat: rlat, lon: rlon })
}

/// std::max for doubles (the first argument unless the second is greater).
fn max_f(a: f64, b: f64) -> f64 {
    if a < b {
        b
    } else {
        a
    }
}

/// Surface positions repeat every 90 degrees: the reference picks the solution. `None` when the two latitudes are in different NL zones.
pub fn global_surface(lat_e: i32, lon_e: i32, lat_o: i32, lon_o: i32, odd_latest: bool, ref_lat: f64, ref_lon: f64) -> Option<Pos> {
    let le = lat_e as f64 / SCALE;
    let lo = lat_o as f64 / SCALE;
    let j = floor(59.0 * le - 60.0 * lo + 0.5);
    // the zones repeat every 90 degrees: the northern solution is in [0, 90), the southern one 90 lower; the reference picks
    let hemisphere = |v: f64| if math::fabs(v - ref_lat) <= math::fabs(v - 90.0 - ref_lat) { v } else { v - 90.0 };
    let ve = hemisphere((90.0 / 60.0) * (modulo(j, 60.0) + le));
    let vo = hemisphere((90.0 / 59.0) * (modulo(j, 59.0) + lo));
    let nl_e = nl(ve);
    if nl_e != nl(vo) {
        return None;
    }
    let rlat = if odd_latest { vo } else { ve };
    let ni = (nl_e - odd_latest as i32).max(1);
    let m = floor((lon_e as f64 / SCALE) * (nl_e - 1) as f64 - (lon_o as f64 / SCALE) * nl_e as f64 + 0.5);
    let base = (90.0 / ni as f64) * (modulo(m, ni as f64) + (if odd_latest { lon_o } else { lon_e }) as f64 / SCALE);
    let (mut best, mut best_d) = (0.0, 1e9);
    for k in 0..4 {
        let c = wrap_lon(base + 90.0 * k as f64);
        let d = math::fabs(wrap_lon(c - ref_lon));
        if d < best_d {
            best_d = d;
            best = c;
        }
    }
    Some(Pos { lat: rlat, lon: best })
}

/// Local decoding of a surface frame against a reference.
pub fn local_surface(lat_cpr: i32, lon_cpr: i32, odd: bool, ref_lat: f64, ref_lon: f64) -> Option<Pos> {
    let i = if odd { 1.0 } else { 0.0 };
    let d_lat = 90.0 / (60.0 - i);
    let lc = lat_cpr as f64 / SCALE;
    let xc = lon_cpr as f64 / SCALE;
    let j = floor(ref_lat / d_lat) + floor(0.5 + modulo(ref_lat, d_lat) / d_lat - lc);
    let rlat = d_lat * (j + lc);
    if rlat < -90.0 || rlat > 90.0 {
        return None;
    }
    let d_lon = 90.0 / max_f(nl(rlat) as f64 - i, 1.0);
    let m = floor(ref_lon / d_lon) + floor(0.5 + modulo(ref_lon, d_lon) / d_lon - xc);
    Some(Pos { lat: rlat, lon: wrap_lon(d_lon * (m + xc)) })
}

fn encode(lat: f64, lon: f64, odd: bool, span: f64) -> (i32, i32) {
    let d_lat = span / (if odd { 59.0 } else { 60.0 });
    let yz = floor(SCALE * modulo(lat, d_lat) / d_lat + 0.5);
    let rlat = d_lat * (yz / SCALE + floor(lat / d_lat));
    let d_lon = span / ((nl(rlat) - odd as i32).max(1)) as f64;
    let xz = floor(SCALE * modulo(lon, d_lon) / d_lon + 0.5);
    (modulo(yz, SCALE) as i32, modulo(xz, SCALE) as i32)
}

/// Encoding of an airborne position (the generator and the tests use it): the 17 bit latitude and longitude.
pub fn encode_airborne(lat: f64, lon: f64, odd: bool) -> (i32, i32) {
    encode(lat, lon, odd, 360.0)
}

/// Encoding of a surface position.
pub fn encode_surface(lat: f64, lon: f64, odd: bool) -> (i32, i32) {
    encode(lat, lon, odd, 90.0)
}
