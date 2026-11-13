// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Geometry and the small physical models around the messages: great circle, magnetic declination, wind triangle, static air temperature.
use crate::ffi::math::{atan2, cos, hypot, sin, sqrt};

const DEG: f64 = core::f64::consts::PI / 180.0;

/// Great circle distance in nautical miles (haversine; mean earth radius 6371.0088 km = 3440.065 NM).
pub fn distance_nm(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let dl = (lat2 - lat1) * DEG;
    let dn = (lon2 - lon1) * DEG;
    let a = sin(dl / 2.0) * sin(dl / 2.0) + cos(lat1 * DEG) * cos(lat2 * DEG) * sin(dn / 2.0) * sin(dn / 2.0);
    let s = sqrt(a);
    2.0 * 3440.065 * crate::ffi::math::asin(if s < 1.0 { s } else { 1.0 })
}

/// Initial bearing in degrees from north (0 - 360).
pub fn bearing_deg(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let dn = (lon2 - lon1) * DEG;
    let y = sin(dn) * cos(lat2 * DEG);
    let x = cos(lat1 * DEG) * sin(lat2 * DEG) - sin(lat1 * DEG) * cos(lat2 * DEG) * cos(dn);
    let d = atan2(y, x) * 180.0 / core::f64::consts::PI;
    let r = crate::ffi::math::fmod(d, 360.0);
    if r < 0.0 {
        r + 360.0
    } else {
        r
    }
}

// Schmidt semi-normalised coefficients (nT), IGRF-13 epoch 2020.0, n = 1..3.
const G: [[f64; 4]; 4] =
    [[0.0, 0.0, 0.0, 0.0], [-29404.8, -1450.9, 0.0, 0.0], [-2499.6, 2982.0, 1677.0, 0.0], [1363.2, -2381.2, 1236.2, 525.7]];
const H: [[f64; 4]; 4] = [[0.0, 0.0, 0.0, 0.0], [0.0, 4652.5, 0.0, 0.0], [0.0, -2991.6, -734.6, 0.0], [0.0, -82.1, 241.9, -543.4]];

/// The scalar potential V / a at r = a for the colatitude `th` and longitude `ph` (radians), degree 3 (IGRF-13 main field).
fn potential(th: f64, ph: f64) -> f64 {
    let x = cos(th);
    let y = sin(th);
    let mut p = [[0.0f64; 4]; 4];
    p[1][0] = x;
    p[1][1] = y;
    p[2][0] = 0.5 * (3.0 * x * x - 1.0);
    p[2][1] = sqrt(3.0) * x * y;
    p[2][2] = sqrt(3.0) / 2.0 * y * y;
    p[3][0] = 0.5 * (5.0 * x * x * x - 3.0 * x);
    p[3][1] = sqrt(3.0 / 8.0) * y * (5.0 * x * x - 1.0);
    p[3][2] = sqrt(15.0) / 2.0 * x * y * y;
    p[3][3] = sqrt(5.0 / 8.0) * y * y * y;
    let mut v = 0.0;
    for n in 1..=3usize {
        for m in 0..=n {
            v += (G[n][m] * cos(m as f64 * ph) + H[n][m] * sin(m as f64 * ph)) * p[n][m];
        }
    }
    v
}

/// Magnetic declination (degrees, east positive) from a degree 3 spherical harmonic field (IGRF-13 main field for 2020, the higher degrees left
/// out). Good to about 2 - 5 degrees over Europe and North America, worse near the magnetic poles and in the South Atlantic anomaly; enough to turn a
/// BDS 6,0 magnetic heading into a true one for a wind estimate.
pub fn mag_declination_deg(lat: f64, lon: f64) -> f64 {
    let d = 1e-4;
    let clamped = lat.clamp(-89.0, 89.0); // (a NaN stays a NaN, like std::clamp)
    let th = (90.0 - clamped) * DEG;
    let ph = lon * DEG;
    let x = (potential(th + d, ph) - potential(th - d, ph)) / (2.0 * d); // north: (1/r) dV/dtheta
    let y = -(potential(th, ph + d) - potential(th, ph - d)) / (2.0 * d) / sin(th); // east: -(1/(r sin theta)) dV/dphi
    atan2(y, x) / DEG
}

/// Wind from a ground vector (track, ground speed) and an air vector (true heading, true airspeed): (direction the wind blows from in degrees
/// true, speed), both in kt.
pub fn wind_triangle(trk: f64, gs: f64, hdg: f64, tas: f64) -> (f64, f64) {
    let we = gs * sin(trk * DEG) - tas * sin(hdg * DEG);
    let wn = gs * cos(trk * DEG) - tas * cos(hdg * DEG);
    let kt = hypot(we, wn);
    let d = atan2(-we, -wn) / DEG;
    let r = crate::ffi::math::fmod(d, 360.0);
    (if r < 0.0 { r + 360.0 } else { r }, kt)
}

/// Static air temperature (C) from true airspeed and Mach: a = TAS / M, T = 288.15 K (a / 661.47 kt)^2.
pub fn sat_from_tas_mach(tas: f64, mach: f64) -> f64 {
    if mach <= 0.0 {
        return -273.15;
    }
    let a = tas / mach / 661.47;
    288.15 * a * a - 273.15
}

/// ISA temperature (C) at a pressure altitude.
pub fn isa_temp_c(alt_ft: f64) -> f64 {
    if alt_ft < 36089.0 {
        15.0 - 1.98120 * alt_ft / 1000.0
    } else {
        -56.5
    }
}

/// ISA static pressure (hPa) at a pressure altitude.
pub fn isa_pressure_hpa(alt_ft: f64) -> f64 {
    let h = alt_ft * 0.3048;
    if h < 11000.0 {
        1013.25 * crate::ffi::math::pow(1.0 - 2.25577e-5 * h, 5.25588)
    } else {
        226.32 * crate::ffi::math::exp(-(h - 11000.0) / 6341.62)
    }
}
