// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! APRS weather (APRS101 chapter 12): the wind / temperature / rain / humidity / pressure fields of a position comment or of a positionless
//! weather report, and their one-line text. `NaN` marks a value that was not measured.
use crate::arena::Arena;

/// Weather values (units as sent: mph, degrees F, inches, percent, mbar). NaN = not measured.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Weather {
    /// Wind direction, degrees.
    pub wind_dir_deg: f64,
    /// Sustained wind, mph.
    pub wind_mph: f64,
    /// Gust, mph.
    pub gust_mph: f64,
    /// Temperature, degrees F.
    pub temp_f: f64,
    /// Rain in the last hour, inches.
    pub rain_1h_in: f64,
    /// Rain in the last 24 hours, inches.
    pub rain_24h_in: f64,
    /// Rain since midnight, inches.
    pub rain_midnight_in: f64,
    /// Humidity, percent.
    pub humidity_pct: f64,
    /// Barometric pressure, mbar.
    pub pressure_mbar: f64,
}

impl Default for Weather {
    fn default() -> Self {
        let n = f64::NAN;
        Weather {
            wind_dir_deg: n,
            wind_mph: n,
            gust_mph: n,
            temp_f: n,
            rain_1h_in: n,
            rain_24h_in: n,
            rain_midnight_in: n,
            humidity_pct: n,
            pressure_mbar: n,
        }
    }
}

impl Weather {
    /// True when any value was measured.
    pub fn any(&self) -> bool {
        !(self.wind_dir_deg.is_nan()
            && self.wind_mph.is_nan()
            && self.gust_mph.is_nan()
            && self.temp_f.is_nan()
            && self.rain_1h_in.is_nan()
            && self.rain_24h_in.is_nan()
            && self.rain_midnight_in.is_nan()
            && self.humidity_pct.is_nan()
            && self.pressure_mbar.is_nan())
    }
}

/// `atof` for the strings the weather fields can hold (digits, `.`, space, `-`): optional leading blanks, an optional minus, digits with an optional
/// fraction; nothing convertible gives 0.0. Up to 22 digits the quotient of two exact integers is the correctly rounded value, as `strtod` returns.
pub fn atof_simple(s: &[u8]) -> f64 {
    let mut i = 0;
    while i < s.len() && s[i] == b' ' {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && s[i] == b'-' {
        neg = true;
        i += 1;
    }
    let (mut mant, mut digits, mut frac) = (0u64, 0, 0u32);
    while i < s.len() && s[i].is_ascii_digit() {
        mant = mant * 10 + u64::from(s[i] - b'0');
        digits += 1;
        i += 1;
    }
    if i < s.len() && s[i] == b'.' {
        i += 1;
        while i < s.len() && s[i].is_ascii_digit() {
            mant = mant * 10 + u64::from(s[i] - b'0');
            digits += 1;
            frac += 1;
            i += 1;
        }
    }
    if digits == 0 {
        return 0.0;
    }
    let mut v = mant as f64;
    if frac > 0 {
        let mut p = 1f64;
        for _ in 0..frac {
            p *= 10.0;
        }
        v /= p;
    }
    if neg {
        -v
    } else {
        v
    }
}

/// A field of `n` characters at `pos`: digits, `.`, space, `-`; Some(NaN) when it has no digit (not measured), None when it is not a number field.
fn wx_number(s: &[u8], pos: usize, n: usize) -> Option<f64> {
    if pos + n > s.len() {
        return None;
    }
    let t = &s[pos..pos + n];
    let mut any_digit = false;
    for &c in t {
        if c.is_ascii_digit() {
            any_digit = true;
        } else if !(c == b'.' || c == b' ' || c == b'-') {
            return None;
        }
    }
    Some(if any_digit { atof_simple(t) } else { f64::NAN })
}

/// The weather part of a comment or of a positionless report: wind first (`DDD/SSS` or `cDDDsSSS`), then letter-coded fields. False when nothing
/// looked like weather.
pub fn parse_weather(s: &[u8], w: &mut Weather) -> bool {
    let mut p = 0;
    if s.len() >= 7 && s[3] == b'/' {
        if let (Some(d), Some(sp)) = (wx_number(s, 0, 3), wx_number(s, 4, 3)) {
            w.wind_dir_deg = d;
            w.wind_mph = sp;
            p = 7;
        }
    } else if s.len() >= 8 && s[0] == b'c' && s[4] == b's' {
        if let (Some(d), Some(sp)) = (wx_number(s, 1, 3), wx_number(s, 5, 3)) {
            w.wind_dir_deg = d;
            w.wind_mph = sp;
            p = 8;
        }
    }
    while p < s.len() {
        let k = s[p];
        let n = match k {
            b'g' | b't' | b'r' | b'p' | b'P' | b'L' | b'l' | b's' | b'#' => 3,
            b'h' => 2,
            b'b' => 5,
            _ => 0,
        };
        if n == 0 {
            break;
        }
        let Some(v) = wx_number(s, p + 1, n) else { break };
        match k {
            b'g' => w.gust_mph = v,
            b't' => w.temp_f = v,
            b'r' => w.rain_1h_in = v / 100.0,
            b'p' => w.rain_24h_in = v / 100.0,
            b'P' => w.rain_midnight_in = v / 100.0,
            b'h' => w.humidity_pct = if v == 0.0 { 100.0 } else { v }, // 00 means 100 %
            b'b' => w.pressure_mbar = v / 10.0,
            _ => {}
        }
        p += 1 + n;
    }
    w.any()
}

/// One line of weather for the table: "72 F, wind 5 mph from 270 deg, gust 12 mph, 55% RH, 1013.2 mbar, rain 0.10 in/h".
pub fn put_summary(o: &mut Arena, w: &Weather) {
    let mut first = true;
    let mut sep = |o: &mut Arena| {
        if !first {
            o.put(b", ");
        }
        first = false;
    };
    if !w.temp_f.is_nan() {
        sep(o);
        o.fixed(0, w.temp_f);
        o.put(b" F");
    }
    if !w.wind_mph.is_nan() {
        sep(o);
        o.put(b"wind ");
        o.fixed(0, w.wind_mph);
        o.put(b" mph");
        if !w.wind_dir_deg.is_nan() {
            o.put(b" from ");
            o.fixed(0, w.wind_dir_deg);
            o.put(b" deg");
        }
    }
    if !w.gust_mph.is_nan() {
        sep(o);
        o.put(b"gust ");
        o.fixed(0, w.gust_mph);
        o.put(b" mph");
    }
    if !w.humidity_pct.is_nan() {
        sep(o);
        o.fixed(0, w.humidity_pct);
        o.put(b"% RH");
    }
    if !w.pressure_mbar.is_nan() {
        sep(o);
        o.fixed(1, w.pressure_mbar);
        o.put(b" mbar");
    }
    if !w.rain_1h_in.is_nan() {
        sep(o);
        o.put(b"rain ");
        o.fixed(2, w.rain_1h_in);
        o.put(b" in/h");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn atof() {
        assert_eq!(atof_simple(b"03.50"), 3.5);
        assert_eq!(atof_simple(b" 12"), 12.0);
        assert_eq!(atof_simple(b"-"), 0.0);
        assert!(atof_simple(b"-0").is_sign_negative());
        assert_eq!(atof_simple(b"1 5"), 1.0);
    }
    #[test]
    fn weather() {
        let mut w = Weather::default();
        assert!(parse_weather(b"220/004g005t077r000p000P000h50b09900", &mut w));
        assert_eq!(
            (w.wind_dir_deg, w.wind_mph, w.gust_mph, w.temp_f, w.humidity_pct, w.pressure_mbar),
            (220.0, 4.0, 5.0, 77.0, 50.0, 990.0)
        );
        let mut w2 = Weather::default();
        assert!(!parse_weather(b"hello", &mut w2));
    }
}
