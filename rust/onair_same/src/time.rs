// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Issue and purge times of a SAME header (47 CFR 11.31 (c): `JJJHHMM` is the day of the year and the UTC time, `TTTT` the purge time).
use crate::text::Buf;

/// Gregorian leap year.
pub fn leap(y: i32) -> bool {
    (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

/// Leap days in the years 1 ..= n.
fn leaps_upto(n: i64) -> i64 {
    n / 4 - n / 100 + n / 400
}

/// The issue time as an epoch (UTC seconds) of the given year; 0 when the date is out of range (so 1970-01-01 00:00 is also 0).
pub fn issue_epoch(year: i32, day: i32, hour: i32, minute: i32) -> i64 {
    let days_in_year = if leap(year) { 366 } else { 365 };
    if year < 1970 || day < 1 || day > days_in_year || !(0..=23).contains(&hour) || !(0..=59).contains(&minute) {
        return 0;
    }
    // whole days from 1970-01-01 to January 1st of the year, then the day of the year
    let y = year as i64;
    let days = (y - 1970) * 365 + (leaps_upto(y - 1) - leaps_upto(1969)) + (day as i64 - 1);
    days * 86400 + hour as i64 * 3600 + minute as i64 * 60
}

/// "May 03 14:30 UTC" for a day of the year (leap years follow the year); empty when out of range.
pub fn issue_text(year: i32, day: i32, hour: i32, minute: i32, out: &mut Buf) {
    const ML: [[i32; 12]; 2] = [[31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31], [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]];
    const MN: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    if issue_epoch(year, day, hour, minute) == 0 {
        return;
    }
    let ml = &ML[leap(year) as usize];
    let (mut d, mut mo) = (day, 0usize);
    while mo < 12 && d > ml[mo] {
        d -= ml[mo];
        mo += 1;
    }
    out.str(MN[mo.min(11)]);
    out.byte(b' ');
    out.dec(d as i64, 2);
    out.byte(b' ');
    out.dec(hour as i64, 2);
    out.byte(b':');
    out.dec(minute as i64, 2);
    out.str(" UTC");
}

/// "1 h 30 min", "45 min", "2 h": the purge time (minutes) in words.
pub fn purge_text(minutes: i32, out: &mut Buf) {
    if minutes % 60 == 0 {
        out.dec((minutes / 60) as i64, 1);
        out.str(" h");
    } else if minutes < 60 {
        out.dec(minutes as i64, 1);
        out.str(" min");
    } else {
        out.dec((minutes / 60) as i64, 1);
        out.str(" h ");
        out.dec((minutes % 60) as i64, 1);
        out.str(" min");
    }
}
