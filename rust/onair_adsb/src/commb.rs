// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Comm-B register inference (DF20 / DF21): which BDS register the 56 bit MB field is, and its content. Follows the checks of the BDS inference
//! chapter of the Riddle (reserved bits zero, status bits consistent with their values, values in a plausible range); a field that passes for two
//! formats is not decoded. Register layouts: ICAO Doc 9871 appendix A (tables A-2-16 data link capability, A-2-23 GICB capability, A-2-48 ACAS
//! resolution advisory, A-2-64 / 65 / 66 intent, track and turn, heading and speed, A-2-68 routine weather, A-2-69 hazard weather).
use crate::altitude::{char_from_6bit, decode_ac13};
use crate::bits::{get, get_signed};
use crate::ffi::math::{fabs, tan};
use crate::geo::{isa_pressure_hpa, isa_temp_c};

/// "No altitude" marker for the reply altitude (the weather registers are only taken with an altitude).
pub const NO_ALTITUDE: i32 = -100000;

/// What the inference found. `repr(C)`: this is the C ABI struct (core/include/dect2/rs/adsb.h, `OnairAdsbCommB`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CommB {
    /// 0x10, 0x17, 0x20, 0x30, 0x40, 0x44, 0x45, 0x50, 0x60, or 0 when the field is not one of them (or fits more than one).
    pub bds: i32,
    /// 2,0: the aircraft identification (trailing spaces removed).
    pub callsign: [u8; 9],
    pub callsign_len: u8,
    // 4,0 selected vertical intention
    pub has_sel_alt: bool,
    pub sel_alt_mcp_ft: i32,
    pub has_sel_alt_fms: bool,
    pub sel_alt_fms_ft: i32,
    pub has_baro: bool,
    pub baro_mb: f64,
    // 5,0 track and turn report
    pub has_roll: bool,
    pub roll_deg: f64,
    pub has_track: bool,
    pub track_deg: f64,
    pub has_gs: bool,
    pub gs_kt: i32,
    pub has_track_rate: bool,
    pub track_rate_dps: f64,
    pub has_tas: bool,
    pub tas_kt: i32,
    // 6,0 heading and speed report
    pub has_heading: bool,
    pub heading_deg: f64,
    pub has_ias: bool,
    pub ias_kt: i32,
    pub has_mach: bool,
    pub mach: f64,
    pub has_baro_rate: bool,
    pub baro_rate_fpm: i32,
    pub has_inertial_rate: bool,
    pub inertial_rate_fpm: i32,
    /// 1,0 data link capability: Mode S subnetwork version, ACAS operating, the continuation flag.
    pub subnet_version: i32,
    pub acas_operating: bool,
    pub continuation: bool,
    /// 1,7: 24 bits, bit 23 (first transmitted) = 0,5 ... bit 0 = 6,0 (Annex 10 IV table A-2-23).
    pub gicb_mask: u32,
    // 3,0 ACAS resolution advisory
    pub ara: u32,
    pub rac: u32,
    pub ra_terminated: bool,
    pub multiple_threats: bool,
    pub tti: i32,
    /// Valid when `tti` is 1.
    pub threat_icao: u32,
    // 4,4 meteorological routine report, 4,5 meteorological hazard report
    /// 4,4 figure of merit / source 0 - 4.
    pub fom: i32,
    pub has_wind: bool,
    pub wind_kt: i32,
    pub wind_dir_deg: f64,
    pub has_sat: bool,
    pub sat_c: f64,
    pub has_pressure: bool,
    pub pressure_hpa: i32,
    pub has_humidity: bool,
    pub humidity_pct: f64,
    /// 4,4 and 4,5: 0 nil, 1 light, 2 moderate, 3 severe (-1 not sent).
    pub turbulence: i32,
    /// 4,5 hazard levels 0 - 3 (-1 not sent).
    pub wind_shear: i32,
    pub microburst: i32,
    pub icing: i32,
    pub wake_vortex: i32,
    pub has_radio_height: bool,
    pub radio_height_ft: i32,
}

impl CommB {
    /// Nothing found (bds 0), with the "not sent" markers at -1 like the C++ defaults.
    pub fn new() -> CommB {
        CommB {
            bds: 0,
            callsign: [0; 9],
            callsign_len: 0,
            has_sel_alt: false,
            sel_alt_mcp_ft: 0,
            has_sel_alt_fms: false,
            sel_alt_fms_ft: 0,
            has_baro: false,
            baro_mb: 0.0,
            has_roll: false,
            roll_deg: 0.0,
            has_track: false,
            track_deg: 0.0,
            has_gs: false,
            gs_kt: 0,
            has_track_rate: false,
            track_rate_dps: 0.0,
            has_tas: false,
            tas_kt: 0,
            has_heading: false,
            heading_deg: 0.0,
            has_ias: false,
            ias_kt: 0,
            has_mach: false,
            mach: 0.0,
            has_baro_rate: false,
            baro_rate_fpm: 0,
            has_inertial_rate: false,
            inertial_rate_fpm: 0,
            subnet_version: -1,
            acas_operating: false,
            continuation: false,
            gicb_mask: 0,
            ara: 0,
            rac: 0,
            ra_terminated: false,
            multiple_threats: false,
            tti: 0,
            threat_icao: 0,
            fom: -1,
            has_wind: false,
            wind_kt: 0,
            wind_dir_deg: 0.0,
            has_sat: false,
            sat_c: 0.0,
            has_pressure: false,
            pressure_hpa: 0,
            has_humidity: false,
            humidity_pct: 0.0,
            turbulence: -1,
            wind_shear: -1,
            microburst: -1,
            icing: -1,
            wake_vortex: -1,
            has_radio_height: false,
            radio_height_ft: 0,
        }
    }
}

impl Default for CommB {
    fn default() -> Self {
        CommB::new()
    }
}

/// Status bit followed by a value of `n` bits: when the status is 0 the value must be 0.
fn status_ok(mb: &[u8], status_bit: i32, first: i32, n: i32) -> bool {
    get(mb, status_bit, 1) != 0 || get(mb, first, n) == 0
}

/// Temperature and pressure of a weather report against the altitude of the reply (ISA +- 35 C; the pressure within 10 hPa + 4 %).
fn weather_fits(alt_ft: i32, sat_c: f64, has_p: bool, hpa: f64) -> bool {
    if alt_ft == NO_ALTITUDE {
        return false;
    }
    if fabs(sat_c - isa_temp_c(alt_ft as f64)) > 35.0 {
        return false;
    }
    if has_p {
        let p = isa_pressure_hpa(alt_ft as f64);
        if fabs(hpa - p) > 10.0 + 0.04 * p {
            return false;
        }
    }
    true
}

fn b(v: bool) -> i32 {
    v as i32
}

/// Infers the register of the 7 bytes `mb` (message bits 33 - 88). `alt_ft`: the altitude of the reply (DF20) or [`NO_ALTITUDE`]: the weather
/// registers 4,4 and 4,5 are only taken with it, their temperature and pressure must fit the altitude (the static pressure is what the altimeter
/// reads, so it is that of the pressure altitude). Fewer than 7 bytes give nothing.
/// An ACAS resolution advisory in the BDS 3,0 layout, from where the format is known: the MV field of DF16 (VDS 0x30) or the ME field of a
/// TC 28 subtype 2 squitter (bytes 2 - 7 have the same layout). The same sanity checks as for Comm-B inference.
pub fn acas_ra(mb: &[u8]) -> CommB {
    let mut out = CommB::new();
    if mb.len() < 7 {
        return out;
    }
    let g = |from: usize, n: usize| -> u32 {
        let mut v = 0u32;
        for i in from..from + n {
            let (byte, bit) = ((i - 1) / 8, 7 - (i - 1) % 8);
            v = (v << 1) | ((mb[byte] >> bit) & 1) as u32;
        }
        v
    };
    let tti = g(29, 2);
    if tti == 3 || (tti == 1 && (g(55, 2) != 0 || g(31, 24) == 0)) || (tti == 0 && g(31, 26) != 0) {
        return out;
    }
    if g(9, 14) == 0 && g(27, 1) == 0 {
        return out;
    }
    out.bds = 0x30;
    out.ara = g(9, 14);
    out.rac = g(23, 4);
    out.ra_terminated = g(27, 1) != 0;
    out.multiple_threats = g(28, 1) != 0;
    out.tti = tti as i32;
    if tti == 1 {
        out.threat_icao = g(31, 24);
    }
    out
}

pub fn infer(mb: &[u8], alt_ft: i32) -> CommB {
    let mut out = CommB::new();
    if mb.len() < 7 {
        return out;
    }
    let mb = &mb[..7];
    let g = |first: i32, n: i32| get(mb, first, n);
    if mb.iter().all(|&x| x == 0) {
        return out;
    }
    // 2,0: the first byte is the code, the rest are eight characters
    let mut is20 = mb[0] == 0x20;
    let mut cs = [b' '; 8];
    if is20 {
        for (i, c) in cs.iter_mut().enumerate() {
            let ch = char_from_6bit(g(9 + 6 * i as i32, 6));
            if ch == b'#' {
                is20 = false;
            }
            *c = ch;
        }
        if is20 && cs.iter().all(|&c| c == b' ') {
            is20 = false;
        }
    }
    // 4,0 selected vertical intention
    let mut is40 = status_ok(mb, 1, 2, 12)
        && status_ok(mb, 14, 15, 12)
        && status_ok(mb, 27, 28, 12)
        && g(40, 8) == 0
        && g(52, 2) == 0
        && status_ok(mb, 48, 49, 3)
        && status_ok(mb, 54, 55, 2);
    if is40 {
        let n40 = g(1, 1) + g(14, 1) + g(27, 1);
        if n40 < 2 {
            is40 = false;
        }
        if is40 && g(27, 1) != 0 {
            let baro = 800.0 + 0.1 * g(28, 12) as f64;
            if !(900.0..=1100.0).contains(&baro) {
                is40 = false;
            }
        }
    }
    // 5,0 track and turn report
    let mut is50 = status_ok(mb, 1, 2, 10)
        && status_ok(mb, 12, 13, 11)
        && status_ok(mb, 24, 25, 10)
        && status_ok(mb, 35, 36, 10)
        && status_ok(mb, 46, 47, 10);
    if is50 {
        let n50 = g(1, 1) + g(12, 1) + g(24, 1) + g(35, 1) + g(46, 1);
        if n50 < 4 {
            is50 = false;
        }
        if is50 && g(1, 1) != 0 && fabs(get_signed(mb, 2, 10) as f64 * 45.0 / 256.0) > 50.0 {
            is50 = false;
        }
        if is50 && g(24, 1) != 0 && g(25, 10) * 2 > 600 {
            is50 = false;
        }
        if is50 && g(46, 1) != 0 && g(47, 10) * 2 > 500 {
            is50 = false;
        }
        // a coordinated turn: rate = g tan(roll) / true airspeed (not in the Riddle; its own example, -9.7 degrees at 466 kt, gives -0.40 deg/s as sent)
        if is50 && g(1, 1) != 0 && g(35, 1) != 0 && g(46, 1) != 0 && g(47, 10) > 20 {
            let tas = g(47, 10) as f64 * 2.0 * 0.514444;
            let expect = 9.80665 * tan(get_signed(mb, 2, 10) as f64 * 45.0 / 256.0 * core::f64::consts::PI / 180.0) / tas * 180.0
                / core::f64::consts::PI;
            if fabs(expect - get_signed(mb, 36, 10) as f64 * 8.0 / 256.0) > 1.0 {
                is50 = false;
            }
        }
        // ground speed and true airspeed differ by the wind: not by more than 200 kt
        if is50 && g(24, 1) != 0 && g(46, 1) != 0 && (g(25, 10) as i32 - g(47, 10) as i32).abs() * 2 > 200 {
            is50 = false;
        }
    }
    // 6,0 heading and speed report
    let mut is60 = status_ok(mb, 1, 2, 11)
        && status_ok(mb, 13, 14, 10)
        && status_ok(mb, 24, 25, 10)
        && status_ok(mb, 35, 36, 10)
        && status_ok(mb, 46, 47, 10);
    if is60 {
        let n60 = g(1, 1) + g(13, 1) + g(24, 1) + g(35, 1) + g(46, 1);
        if n60 < 4 {
            is60 = false;
        }
        if is60 && g(13, 1) != 0 && g(14, 10) > 500 {
            is60 = false;
        }
        if is60 && g(24, 1) != 0 && g(25, 10) as f64 * 0.004 > 1.0 {
            is60 = false;
        }
        if is60 && g(35, 1) != 0 && get_signed(mb, 36, 10).abs() * 32 > 6000 {
            is60 = false;
        }
        if is60 && g(46, 1) != 0 && get_signed(mb, 47, 10).abs() * 32 > 6000 {
            is60 = false;
        }
        // indicated airspeed cannot exceed Mach x the speed of sound at sea level (661.5 kt), with some room for the instrument error
        if is60 && g(13, 1) != 0 && g(24, 1) != 0 && g(14, 10) as f64 > g(25, 10) as f64 * 0.004 * 661.5 + 25.0 {
            is60 = false;
        }
    }
    // 1,0 data link capability (Doc 9871 table A-2-16): fixed code, reserved bits 10-14 zero, a subnetwork version that exists (0-5)
    let mut is10 = mb[0] == 0x10 && g(10, 5) == 0 && g(17, 7) <= 5;
    // 1,7 common usage GICB capability: bits 25-56 reserved; any aircraft that answers it has 2,0 (bit 7)
    let mut is17 = g(25, 32) == 0 && g(7, 1) != 0 && g(1, 24) != 0;
    // 3,0 ACAS RA (table A-2-48): fixed code, TTI 3 is not used, an address threat leaves bits 55-56 zero, no threat: no TID; a real RA has an
    // active RA or the terminated bit
    let mut is30 = mb[0] == 0x30;
    if is30 {
        let tti = g(29, 2);
        if tti == 3 || (tti == 1 && g(55, 2) != 0) || (tti == 0 && g(31, 26) != 0) {
            is30 = false;
        }
        if g(9, 14) == 0 && g(27, 1) == 0 {
            is30 = false;
        }
        if tti == 1 && g(31, 24) == 0 {
            is30 = false;
        }
        // one threat (bit 9): bits 16-22 (the multiple threat part) are 0 and so is MTE; several threats without bit 9 set MTE
        if g(9, 1) != 0 && (g(16, 7) != 0 || g(28, 1) != 0) {
            is30 = false;
        }
        if g(9, 1) == 0 && g(10, 13) != 0 && g(28, 1) == 0 {
            is30 = false;
        }
        if g(23, 2) == 3 || g(25, 2) != 0 {
            is30 = false; // "do not pass below" and "above" at once; turn complements (ACAS II has no horizontal RAs)
        }
        if tti == 2 && (g(51, 6) > 60 || g(51, 6) == 0 || g(44, 7) == 0 || decode_ac13(g(31, 13)).is_none()) {
            is30 = false; // bearing 1-60, a range, an altitude
        }
    }
    // 4,4 meteorological routine air report (table A-2-68): FOM 0-4, status bits, wind <= 250 kt, SAT -80..+60 C, pressure <= 1100 hPa; at least wind
    // and a temperature that is not exactly zero bits
    let mut is44 =
        g(1, 4) <= 4 && status_ok(mb, 5, 6, 18) && status_ok(mb, 35, 36, 11) && status_ok(mb, 47, 48, 2) && status_ok(mb, 50, 51, 6);
    let mut sat44 = 0.0;
    if is44 {
        sat44 = (g(25, 10) as i32 - if g(24, 1) != 0 { 1024 } else { 0 }) as f64 * 0.25;
        if g(5, 1) == 0 || g(6, 9) > 250 || sat44 < -80.0 || sat44 > 60.0 {
            is44 = false;
        }
        if is44 && g(35, 1) != 0 && (g(36, 11) > 1100 || g(36, 11) < 100) {
            is44 = false;
        }
        if is44 && g(50, 1) != 0 && g(51, 6) as f64 * 100.0 / 64.0 > 100.0 {
            is44 = false;
        }
        if is44 && g(25, 10) == 0 && g(24, 1) == 0 {
            is44 = false; // 0.00 C at cruise is rare; an all-zero tail is more often another register
        }
        if is44 && g(1, 4) == 0 {
            is44 = false; // FOM 0: invalid
        }
        if is44 && !weather_fits(alt_ft, sat44, g(35, 1) != 0, g(36, 11) as f64) {
            is44 = false;
        }
    }
    // 4,5 meteorological hazard report (table A-2-69): reserved bits 52-56 zero, every status bit consistent, SAT -80..+60, pressure, radio height
    // <= 5000 ft (it is only meaningful near the ground); needs the temperature and at least one hazard
    let mut is45 = g(52, 5) == 0
        && status_ok(mb, 1, 2, 2)
        && status_ok(mb, 4, 5, 2)
        && status_ok(mb, 7, 8, 2)
        && status_ok(mb, 10, 11, 2)
        && status_ok(mb, 13, 14, 2)
        && status_ok(mb, 16, 17, 10)
        && status_ok(mb, 27, 28, 11)
        && status_ok(mb, 39, 40, 12);
    let mut sat45 = 0.0;
    if is45 {
        sat45 = (g(18, 9) as i32 - if g(17, 1) != 0 { 512 } else { 0 }) as f64 * 0.25;
        let hz: u32 = (0..5).map(|i| g(1 + 3 * i, 1)).sum();
        if g(16, 1) == 0 || hz == 0 || sat45 < -80.0 || sat45 > 60.0 {
            is45 = false;
        }
        if is45 && g(27, 1) != 0 && (g(28, 11) > 1100 || g(28, 11) < 100) {
            is45 = false;
        }
        if is45 && g(39, 1) != 0 && g(40, 12) * 16 > 5000 {
            is45 = false;
        }
        if is45 && g(39, 1) != 0 && alt_ft != NO_ALTITUDE && (g(40, 12) as i32) * 16 > alt_ft + 2000 {
            is45 = false; // radio height above the field elevation
        }
        if is45 && !weather_fits(alt_ft, sat45, g(27, 1) != 0, g(28, 11) as f64) {
            is45 = false;
        }
    }
    // The registers decoded first (2,0 4,0 5,0 6,0) are asked for far more often: when one of them fits alone it wins over the newer ones. A
    // capability report that also passes as 4,0 without a baro setting is ambiguous (a real 4,0 almost always has the setting).
    let tier1 = b(is20) + b(is40) + b(is50) + b(is60);
    if tier1 > 1 {
        return out;
    }
    if tier1 == 1 {
        if is40 && is17 && g(27, 1) == 0 {
            return out;
        }
        if (is10 || is30) && !is20 {
            return out; // a fixed code byte that also passes as 4,0 / 5,0 / 6,0: decoded neither way
        }
        is10 = false;
        is17 = false;
        is30 = false;
        is44 = false;
        is45 = false;
    } else if b(is10) + b(is17) + b(is30) + b(is44) + b(is45) != 1 {
        return out;
    }
    if is10 {
        out.bds = 0x10;
        out.continuation = g(9, 1) != 0;
        out.subnet_version = g(17, 7) as i32;
        out.acas_operating = g(38, 1) != 0;
    } else if is17 {
        out.bds = 0x17;
        out.gicb_mask = g(1, 24);
    } else if is30 {
        out.bds = 0x30;
        out.ara = g(9, 14);
        out.rac = g(23, 4);
        out.ra_terminated = g(27, 1) != 0;
        out.multiple_threats = g(28, 1) != 0;
        out.tti = g(29, 2) as i32;
        if out.tti == 1 {
            out.threat_icao = g(31, 24);
        }
    } else if is44 {
        out.bds = 0x44;
        out.fom = g(1, 4) as i32;
        out.has_wind = true;
        out.wind_kt = g(6, 9) as i32;
        out.wind_dir_deg = g(15, 9) as f64 * 180.0 / 256.0;
        out.has_sat = true;
        out.sat_c = sat44;
        if g(35, 1) != 0 {
            out.has_pressure = true;
            out.pressure_hpa = g(36, 11) as i32;
        }
        if g(47, 1) != 0 {
            out.turbulence = g(48, 2) as i32;
        }
        if g(50, 1) != 0 {
            out.has_humidity = true;
            out.humidity_pct = g(51, 6) as f64 * 100.0 / 64.0;
        }
    } else if is45 {
        out.bds = 0x45;
        for i in 0..5 {
            if g(1 + 3 * i, 1) != 0 {
                let v = g(2 + 3 * i, 2) as i32;
                match i {
                    0 => out.turbulence = v,
                    1 => out.wind_shear = v,
                    2 => out.microburst = v,
                    3 => out.icing = v,
                    _ => out.wake_vortex = v,
                }
            }
        }
        out.has_sat = true;
        out.sat_c = sat45;
        if g(27, 1) != 0 {
            out.has_pressure = true;
            out.pressure_hpa = g(28, 11) as i32;
        }
        if g(39, 1) != 0 {
            out.has_radio_height = true;
            out.radio_height_ft = g(40, 12) as i32 * 16;
        }
    } else if is20 {
        out.bds = 0x20;
        let mut n = 8;
        while n > 0 && cs[n - 1] == b' ' {
            n -= 1;
        }
        out.callsign[..n].copy_from_slice(&cs[..n]);
        out.callsign_len = n as u8;
    } else if is40 {
        out.bds = 0x40;
        if g(1, 1) != 0 {
            out.has_sel_alt = true;
            out.sel_alt_mcp_ft = g(2, 12) as i32 * 16;
        }
        if g(14, 1) != 0 {
            out.has_sel_alt_fms = true;
            out.sel_alt_fms_ft = g(15, 12) as i32 * 16;
        }
        if g(27, 1) != 0 {
            out.has_baro = true;
            out.baro_mb = 800.0 + 0.1 * g(28, 12) as f64;
        }
    } else if is50 {
        out.bds = 0x50;
        if g(1, 1) != 0 {
            out.has_roll = true;
            out.roll_deg = get_signed(mb, 2, 10) as f64 * 45.0 / 256.0;
        }
        if g(12, 1) != 0 {
            out.has_track = true;
            let mut t = get_signed(mb, 13, 11) as f64 * 90.0 / 512.0;
            if t < 0.0 {
                t += 360.0;
            }
            out.track_deg = t;
        }
        if g(24, 1) != 0 {
            out.has_gs = true;
            out.gs_kt = g(25, 10) as i32 * 2;
        }
        if g(35, 1) != 0 {
            out.has_track_rate = true;
            out.track_rate_dps = get_signed(mb, 36, 10) as f64 * 8.0 / 256.0;
        }
        if g(46, 1) != 0 {
            out.has_tas = true;
            out.tas_kt = g(47, 10) as i32 * 2;
        }
    } else {
        out.bds = 0x60;
        if g(1, 1) != 0 {
            out.has_heading = true;
            let mut h = get_signed(mb, 2, 11) as f64 * 90.0 / 512.0;
            if h < 0.0 {
                h += 360.0;
            }
            out.heading_deg = h;
        }
        if g(13, 1) != 0 {
            out.has_ias = true;
            out.ias_kt = g(14, 10) as i32;
        }
        if g(24, 1) != 0 {
            out.has_mach = true;
            out.mach = g(25, 10) as f64 * 0.004;
        }
        if g(35, 1) != 0 {
            out.has_baro_rate = true;
            out.baro_rate_fpm = get_signed(mb, 36, 10) * 32;
        }
        if g(46, 1) != 0 {
            out.has_inertial_rate = true;
            out.inertial_rate_fpm = get_signed(mb, 47, 10) * 32;
        }
    }
    out
}

/// The text of a BDS 3,0 advisory ("climb, corrective, crossing ..."), written to `out`.
pub fn ra_text(ara: u32, rac: u32, terminated: bool, out: &mut crate::text::Buf) {
    if terminated {
        out.str("terminated");
        return;
    }
    if ara & 0x2000 != 0 {
        // bit 9 (first of the 14): one threat, the bits below give the advisory
        out.str(if ara & 0x0800 != 0 { "descend" } else { "climb" }); // bit 11: downward sense
        out.str(if ara & 0x1000 != 0 { ", corrective" } else { ", preventive" }); // bit 10: corrective
        if ara & 0x0400 != 0 {
            out.str(", increase rate"); // bit 12
        }
        if ara & 0x0200 != 0 {
            out.str(", sense reversal"); // bit 13
        }
        if ara & 0x0100 != 0 {
            out.str(", crossing"); // bit 14: altitude crossing
        }
        if ara & 0x0080 != 0 {
            out.str(", positive"); // bit 15: positive (vs. vertical speed limit)
        }
    } else if ara != 0 {
        out.str("multiple threats");
    } else {
        out.str("advisory");
    }
    if rac & 8 != 0 {
        out.str(" (no descend)");
    }
    if rac & 4 != 0 {
        out.str(" (no climb)");
    }
    if rac & 2 != 0 {
        out.str(" (no left)");
    }
    if rac & 1 != 0 {
        out.str(" (no right)");
    }
}
