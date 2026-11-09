//! Field extraction of a whole Mode S message (DF0 / 4 / 5 / 11 / 16 / 17 / 18 / 19 / 20 / 21) and of the ADS-B extended squitter: identification,
//! surface and airborne position, velocity, aircraft status, target state and operational status (DO-260B 2.2.3.2.*; the Riddle chapters).
use crate::altitude::{char_from_6bit, decode_ac13, decode_alt12, decode_id13};
use crate::bits::get;
use crate::commb::{self, CommB};
use crate::crc::df_length;
use crate::ffi::math::{atan2, sqrt};

/// The decoded message. `repr(C)`: this is the C ABI struct (core/include/dect2/rs/adsb.h, `OnairAdsbMsg`); the fields keep the order and meaning of
/// the C++ `dect2::adsb::Msg`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Msg {
    pub df: i32,
    pub nbits: i32,
    /// DF11 / 17 / 18 / 19: the AA field. Address / parity formats: set by the caller from the remainder.
    pub icao: u32,
    /// Capability (DF11, DF17) or control field (DF18).
    pub ca: i32,
    /// On the ground (flight status, surface position, capability).
    pub ground: bool,
    pub alert: bool,
    pub spi: bool,
    /// Flight status (DF4 / 5 / 20 / 21), -1 when none.
    pub fs: i32,
    /// Vertical status (DF0 / 16), -1 when none.
    pub vs: i32,
    /// Barometric altitude (DF0 / 4 / 16 / 20, TC 9-18), or GNSS height (TC 20-22).
    pub has_alt: bool,
    pub alt_ft: i32,
    pub alt_gnss: bool,
    /// DF5 / 21, TC 28.
    pub has_squawk: bool,
    pub squawk: i32,
    /// Extended squitter type code 1 - 31 (-1 none) and subtype.
    pub tc: i32,
    pub st: i32,
    /// TC 1 - 4: identification; the category set is 'A' to 'D'.
    pub has_ident: bool,
    pub callsign: [u8; 9],
    pub callsign_len: u8,
    pub cat_set: u8,
    pub cat_code: i32,
    /// TC 5-8, 9-18, 20-22.
    pub has_cpr: bool,
    pub cpr_odd: bool,
    pub surface: bool,
    pub cpr_lat: i32,
    pub cpr_lon: i32,
    /// Surface movement and track (TC 5-8).
    pub has_move: bool,
    pub move_kt: f64,
    pub has_track: bool,
    pub track_deg: f64,
    /// TC 19: speed kind 0 ground speed, 1 indicated airspeed, 2 true airspeed.
    pub has_vel: bool,
    pub speed_kt: f64,
    pub speed_kind: i32,
    /// Track for ground speed, magnetic heading for airspeed.
    pub has_heading: bool,
    pub heading_deg: f64,
    pub heading_is_track: bool,
    pub has_vrate: bool,
    pub vrate_fpm: i32,
    pub vrate_baro: bool,
    /// GNSS height minus barometric altitude.
    pub has_gnss_diff: bool,
    pub gnss_diff_ft: i32,
    /// TC 28: emergency state, 0 none, -1 not sent.
    pub emergency: i32,
    // TC 29 target state and status (subtype 1)
    pub has_sel_alt: bool,
    pub sel_alt_ft: i32,
    pub sel_alt_fms: bool,
    pub has_baro: bool,
    pub baro_mb: f64,
    pub has_sel_hdg: bool,
    pub sel_hdg_deg: f64,
    pub ts_nacp: i32,
    pub autopilot: bool,
    pub vnav: bool,
    pub alt_hold: bool,
    pub approach: bool,
    pub lnav: bool,
    pub ts_mode_valid: bool,
    // TC 31 operational status
    pub adsb_version: i32,
    pub nacp: i32,
    pub nic_supp_a: i32,
    pub sil: i32,
    pub gva: i32,
    pub nic_baro: i32,
    /// DF20 / 21 Comm-B.
    pub commb: CommB,
}

impl Msg {
    /// An empty message (every "not sent" field at -1, like the C++ defaults).
    pub fn new() -> Msg {
        Msg {
            df: 0,
            nbits: 0,
            icao: 0,
            ca: 0,
            ground: false,
            alert: false,
            spi: false,
            fs: -1,
            vs: -1,
            has_alt: false,
            alt_ft: 0,
            alt_gnss: false,
            has_squawk: false,
            squawk: 0,
            tc: -1,
            st: -1,
            has_ident: false,
            callsign: [0; 9],
            callsign_len: 0,
            cat_set: 0,
            cat_code: 0,
            has_cpr: false,
            cpr_odd: false,
            surface: false,
            cpr_lat: 0,
            cpr_lon: 0,
            has_move: false,
            move_kt: 0.0,
            has_track: false,
            track_deg: 0.0,
            has_vel: false,
            speed_kt: 0.0,
            speed_kind: 0,
            has_heading: false,
            heading_deg: 0.0,
            heading_is_track: false,
            has_vrate: false,
            vrate_fpm: 0,
            vrate_baro: false,
            has_gnss_diff: false,
            gnss_diff_ft: 0,
            emergency: -1,
            has_sel_alt: false,
            sel_alt_ft: 0,
            sel_alt_fms: false,
            has_baro: false,
            baro_mb: 0.0,
            has_sel_hdg: false,
            sel_hdg_deg: 0.0,
            ts_nacp: -1,
            autopilot: false,
            vnav: false,
            alt_hold: false,
            approach: false,
            lnav: false,
            ts_mode_valid: false,
            adsb_version: -1,
            nacp: -1,
            nic_supp_a: -1,
            sil: -1,
            gva: -1,
            nic_baro: -1,
            commb: CommB::new(),
        }
    }
}

impl Default for Msg {
    fn default() -> Self {
        Msg::new()
    }
}

/// Movement field of a surface position (DO-260B surface position movement encoding; the ranges as printed in the Riddle, chapter Surface Position): the lower edge of the
/// range in kt, `None` for 0 (no information) and the reserved codes above 124.
pub fn movement_to_kt(c: u32) -> Option<f64> {
    if c == 0 || c > 124 {
        return None;
    }
    let c = c as f64;
    Some(if c == 1.0 {
        0.0
    } else if c <= 8.0 {
        0.125 * (c - 1.0)
    } else if c <= 12.0 {
        1.0 + 0.25 * (c - 9.0)
    } else if c <= 38.0 {
        2.0 + 0.5 * (c - 13.0)
    } else if c <= 93.0 {
        15.0 + (c - 39.0)
    } else if c <= 108.0 {
        70.0 + 2.0 * (c - 94.0)
    } else if c <= 123.0 {
        100.0 + 5.0 * (c - 109.0)
    } else {
        175.0
    })
}

/// Airborne velocity, TC 19 (DO-260B 2.2.3.2.6). ME bits (message bit 32 + n): 9 IC, 10 resv, 11-13 NAC, 14 Sew / SH, 15-24 Vew / heading, 25 Sns /
/// airspeed type, 26-35 Vns / airspeed, 36 vertical source, 37 Svr, 38-46 Vr, 49 Sdif, 50-56 Dif.
fn decode_velocity(b: &[u8], m: &mut Msg) {
    let st = m.st;
    if !(1..=4).contains(&st) {
        return;
    }
    if st <= 2 {
        let vew = get(b, 32 + 15, 10);
        let vns = get(b, 32 + 26, 10);
        if vew == 0 || vns == 0 {
            return;
        }
        let mul = if st == 2 { 4.0 } else { 1.0 };
        let ve = (vew - 1) as f64 * mul * if get(b, 32 + 14, 1) != 0 { -1.0 } else { 1.0 }; // 1 = towards the west
        let vn = (vns - 1) as f64 * mul * if get(b, 32 + 25, 1) != 0 { -1.0 } else { 1.0 }; // 1 = towards the south
        m.has_vel = true;
        m.speed_kt = sqrt(ve * ve + vn * vn);
        m.speed_kind = 0;
        m.has_heading = true;
        m.heading_is_track = true;
        let mut h = atan2(ve, vn) * 180.0 / core::f64::consts::PI;
        if h < 0.0 {
            h += 360.0;
        }
        m.heading_deg = h;
    } else {
        if get(b, 32 + 14, 1) != 0 {
            m.has_heading = true;
            m.heading_is_track = false;
            m.heading_deg = get(b, 32 + 15, 10) as f64 * 360.0 / 1024.0;
        }
        let air = get(b, 32 + 26, 10);
        if air != 0 {
            m.has_vel = true;
            m.speed_kt = (air - 1) as f64 * if st == 4 { 4.0 } else { 1.0 };
            m.speed_kind = if get(b, 32 + 25, 1) != 0 { 2 } else { 1 };
        }
    }
    let vr = get(b, 32 + 38, 9);
    if vr != 0 {
        m.has_vrate = true;
        m.vrate_fpm = (vr as i32 - 1) * 64 * if get(b, 32 + 37, 1) != 0 { -1 } else { 1 };
        m.vrate_baro = get(b, 32 + 36, 1) != 0;
    }
    let dif = get(b, 32 + 50, 7);
    if dif != 0 {
        m.has_gnss_diff = true;
        m.gnss_diff_ft = (dif as i32 - 1) * 25 * if get(b, 32 + 49, 1) != 0 { -1 } else { 1 };
        // 1 = GNSS height below barometric altitude
    }
}

/// The ME field of DF17 / 18 / 19 (message bits 33 - 88).
fn decode_extended(b: &[u8], m: &mut Msg) {
    m.tc = get(b, 33, 5) as i32;
    let tc = m.tc;
    if (1..=4).contains(&tc) {
        m.cat_set = b'A' + (4 - tc) as u8;
        m.cat_code = get(b, 38, 3) as i32;
        let mut cs = [b' '; 8];
        for (i, c) in cs.iter_mut().enumerate() {
            let ch = char_from_6bit(get(b, 41 + 6 * i as i32, 6));
            *c = if ch == b'#' { b' ' } else { ch };
        }
        let mut n = 8;
        while n > 0 && cs[n - 1] == b' ' {
            n -= 1;
        }
        m.has_ident = true;
        m.callsign[..n].copy_from_slice(&cs[..n]);
        m.callsign_len = n as u8;
    } else if (5..=8).contains(&tc) {
        m.surface = true;
        m.ground = true;
        if let Some(kt) = movement_to_kt(get(b, 38, 7)) {
            m.has_move = true;
            m.move_kt = kt;
        }
        if get(b, 45, 1) != 0 {
            m.has_track = true;
            m.track_deg = get(b, 46, 7) as f64 * 360.0 / 128.0;
        }
        m.has_cpr = true;
        m.cpr_odd = get(b, 54, 1) != 0;
        m.cpr_lat = get(b, 55, 17) as i32;
        m.cpr_lon = get(b, 72, 17) as i32;
    } else if (9..=18).contains(&tc) || (20..=22).contains(&tc) {
        // The altitude field of TC 20-22 is GNSS height. readsb (and dump1090) decode it like the barometric altitude, 25 ft steps with the Q bit; the
        // Riddle says metres, which cannot be right for a 12 bit field (4095 m = 13435 ft). Not checked against DO-260B: see docs/modes/adsb.md.
        if let Some(a) = decode_alt12(get(b, 41, 12)) {
            m.has_alt = true;
            m.alt_ft = a;
            m.alt_gnss = tc >= 20;
        }
        let ss = get(b, 38, 2);
        if ss == 1 || ss == 2 {
            m.alert = true;
        }
        if ss == 3 {
            m.spi = true;
        }
        m.has_cpr = true;
        m.cpr_odd = get(b, 54, 1) != 0;
        m.cpr_lat = get(b, 55, 17) as i32;
        m.cpr_lon = get(b, 72, 17) as i32;
    } else if tc == 0 {
        // no position information: the altitude field may still be filled
        if let Some(a) = decode_alt12(get(b, 41, 12)) {
            m.has_alt = true;
            m.alt_ft = a;
        }
    } else if tc == 19 {
        m.st = get(b, 38, 3) as i32;
        decode_velocity(b, m);
    } else if tc == 28 {
        m.st = get(b, 38, 3) as i32;
        if m.st == 1 {
            m.emergency = get(b, 41, 3) as i32;
            let id = get(b, 44, 13);
            if id != 0 {
                m.has_squawk = true;
                m.squawk = decode_id13(id);
            }
        }
    } else if tc == 29 {
        m.st = get(b, 38, 2) as i32;
        if m.st == 1 {
            // DO-260B target state and status; field positions as in readsb mode_s.c (decodeESTargetState), not read from the standard
            m.sel_alt_fms = get(b, 41, 1) != 0;
            let sa = get(b, 42, 11);
            if sa != 0 {
                m.has_sel_alt = true;
                m.sel_alt_ft = (sa as i32 - 1) * 32;
            }
            let bp = get(b, 53, 9);
            if bp != 0 {
                m.has_baro = true;
                m.baro_mb = 800.0 + (bp as i32 - 1) as f64 * 0.8;
            }
            if get(b, 62, 1) != 0 {
                m.has_sel_hdg = true;
                let mut h = get(b, 63, 9) as i32;
                if h >= 256 {
                    h -= 512;
                }
                let mut d = h as f64 * 180.0 / 256.0;
                if d < 0.0 {
                    d += 360.0;
                }
                m.sel_hdg_deg = d;
            }
            m.ts_nacp = get(b, 72, 4) as i32;
            if get(b, 79, 1) != 0 {
                m.ts_mode_valid = true;
                m.autopilot = get(b, 80, 1) != 0;
                m.vnav = get(b, 81, 1) != 0;
                m.alt_hold = get(b, 82, 1) != 0;
                m.approach = get(b, 84, 1) != 0;
                m.lnav = get(b, 86, 1) != 0;
            }
        }
    } else if tc == 31 {
        m.st = get(b, 38, 3) as i32;
        if m.st == 0 || m.st == 1 {
            let ver = get(b, 73, 3) as i32;
            m.adsb_version = ver;
            if ver >= 1 {
                // NIC supplement-A, NACp, SIL: bits 76, 77-80, 83-84 (version 1 and 2)
                m.nic_supp_a = get(b, 76, 1) as i32;
                m.nacp = get(b, 77, 4) as i32;
                m.sil = get(b, 83, 2) as i32;
                if m.st == 0 {
                    m.nic_baro = get(b, 85, 1) as i32;
                    if ver >= 2 {
                        m.gva = get(b, 81, 2) as i32;
                    }
                }
            }
        }
    }
}

/// Field extraction for a message whose length and DF are right (the caller has done the CRC and the address checks): `b` holds the `nbits` bits.
/// Returns false when the length does not fit the DF, or for the formats whose ME field is not decoded (DF18 with CA 4 or 7, DF19 with CA != 0 return
/// true with the header fields only, like the C++ original). The message is filled as far as it got.
pub fn decode(b: &[u8], nbits: i32) -> (bool, Msg) {
    let mut m = Msg::new();
    m.df = get(b, 1, 5) as i32;
    m.nbits = nbits;
    if df_length(m.df) != nbits {
        return (false, m);
    }
    match m.df {
        0 | 16 => {
            m.vs = get(b, 6, 1) as i32;
            m.ground = m.vs == 1;
            if let Some(a) = decode_ac13(get(b, 20, 13)) {
                m.has_alt = true;
                m.alt_ft = a;
            }
        }
        4 | 5 | 20 | 21 => {
            m.fs = get(b, 6, 3) as i32;
            m.ground = m.fs == 1 || m.fs == 3;
            m.alert = (2..=4).contains(&m.fs);
            m.spi = m.fs == 4 || m.fs == 5;
            if m.df == 4 || m.df == 20 {
                if let Some(a) = decode_ac13(get(b, 20, 13)) {
                    m.has_alt = true;
                    m.alt_ft = a;
                }
            } else {
                m.has_squawk = true;
                m.squawk = decode_id13(get(b, 20, 13));
            }
            if m.df >= 20 {
                let alt = if m.df == 20 && m.has_alt { m.alt_ft } else { commb::NO_ALTITUDE };
                m.commb = commb::infer(b.get(4..).unwrap_or(&[]), alt);
            }
        }
        11 => {
            m.ca = get(b, 6, 3) as i32;
            m.icao = get(b, 9, 24);
            m.ground = m.ca == 4;
        }
        17..=19 => {
            m.ca = get(b, 6, 3) as i32;
            m.icao = get(b, 9, 24);
            // DO-260B 2.2.3.2.6.1 / readsb: CF 3 is coarse TIS-B (its own ME layout, not the ADS-B one), CF 4 TIS-B management, CF 7 reserved: no ME decoding
            if m.df == 18 && (m.ca == 3 || m.ca == 4 || m.ca == 7) {
                return (true, m);
            }
            if m.df == 19 && m.ca != 0 {
                return (true, m); // military: only application field 0 has the extended squitter layout
            }
            decode_extended(b, &mut m);
        }
        _ => {}
    }
    (true, m)
}
