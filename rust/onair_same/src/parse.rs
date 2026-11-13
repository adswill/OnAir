// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The SAME header parser (47 CFR 11.31 (a), (c)): `ZCZC-ORG-EEE-PSSCCC[-PSSCCC...]+TTTT-JJJHHMM-LLLLLLLL-`, and the end of message `NNNN`.
use crate::locate::{digits, event_kind, event_name, num, originator_name, valid_char};

/// The result of [`parse`]. Like the C++ parser it keeps the fields set before the first error, so `err` says where it stopped.
pub struct Parsed {
    /// Every field passed its check.
    pub valid: bool,
    /// The message is "NNNN".
    pub eom: bool,
    /// Why the message is not valid (NUL-terminated), "" when it is.
    pub err: &'static str,
    /// The originator (3 letters at byte 5) was read.
    pub org_set: bool,
    /// The event code (3 letters at byte 9) was read.
    pub event_set: bool,
    /// Location codes read; location k is the six digits at byte `13 + 7 * k`.
    pub nloc: usize,
    /// TTTT in minutes.
    pub purge_min: i32,
    pub issue_day: i32,
    pub issue_hour: i32,
    pub issue_min: i32,
    /// The call sign was cut out: bytes `call_start .. call_start + call_len`.
    pub call_set: bool,
    pub call_start: usize,
    pub call_len: usize,
    /// Originator name, event name and event kind (NUL-terminated), set when valid.
    pub org_name: &'static str,
    pub event_name: &'static str,
    pub event_kind: &'static str,
}

/// Start of the first location code: "ZCZC-" 5, ORG 3, '-', EEE 3, '-'.
pub const LOC_START: usize = 13;

fn is_cap(c: Option<&u8>) -> bool {
    c.is_some_and(u8::is_ascii_uppercase)
}

/// Parses and checks one message.
pub fn parse(text: &[u8]) -> Parsed {
    let mut m = Parsed {
        valid: false,
        eom: false,
        err: "\0",
        org_set: false,
        event_set: false,
        nloc: 0,
        purge_min: 0,
        issue_day: 0,
        issue_hour: 0,
        issue_min: 0,
        call_set: false,
        call_start: 0,
        call_len: 0,
        org_name: "\0",
        event_name: "\0",
        event_kind: "\0",
    };
    if text == b"NNNN" {
        m.valid = true;
        m.eom = true;
        return m;
    }
    macro_rules! fail {
        ($why:expr) => {{
            m.err = concat!($why, "\0");
            return m;
        }};
    }
    if !text.starts_with(b"ZCZC-") {
        fail!("does not start with ZCZC-");
    }
    let mut i = 5;
    // ORG-EEE-
    if !(0..3).all(|k| is_cap(text.get(i + k))) {
        fail!("bad originator");
    }
    m.org_set = true;
    i += 3;
    if text.get(i) != Some(&b'-') {
        fail!("missing dash after the originator");
    }
    i += 1;
    if !(0..3).all(|k| is_cap(text.get(i + k))) {
        fail!("bad event code");
    }
    m.event_set = true;
    i += 3;
    if text.get(i) != Some(&b'-') {
        fail!("missing dash after the event code");
    }
    i += 1;
    // locations until '+'
    loop {
        if !digits(text, i, 6) {
            fail!("bad location code");
        }
        m.nloc += 1;
        i += 6;
        match text.get(i) {
            None => fail!("message ends in the location list"),
            Some(b'+') => {
                i += 1;
                break;
            }
            Some(b'-') => {}
            Some(_) => fail!("bad separator in the location list"),
        }
        i += 1;
        if m.nloc >= 31 {
            fail!("more than 31 locations");
        }
    }
    // +TTTT: hours and minutes the alert is valid
    if !digits(text, i, 4) {
        fail!("bad purge time");
    }
    let (hh, mm) = (num(text, i, 2), num(text, i + 2, 2));
    if mm > 59 {
        fail!("purge time minutes over 59");
    }
    m.purge_min = hh * 60 + mm;
    if m.purge_min == 0 {
        fail!("purge time zero");
    }
    i += 4;
    if text.get(i) != Some(&b'-') {
        fail!("missing dash after the purge time");
    }
    i += 1;
    // JJJHHMM: day of the year and UTC time
    if !digits(text, i, 7) {
        fail!("bad issue time");
    }
    m.issue_day = num(text, i, 3);
    m.issue_hour = num(text, i + 3, 2);
    m.issue_min = num(text, i + 5, 2);
    if m.issue_day < 1 || m.issue_day > 366 {
        fail!("issue day out of range");
    }
    if m.issue_hour > 23 || m.issue_min > 59 {
        fail!("issue time out of range");
    }
    i += 7;
    if text.get(i) != Some(&b'-') {
        fail!("missing dash after the issue time");
    }
    i += 1;
    // LLLLLLLL-: the sender's call sign, '/' replaces '-'
    let Some(d) = text[i..].iter().position(|&c| c == b'-').map(|p| p + i) else {
        fail!("missing dash after the call sign");
    };
    m.call_set = true;
    m.call_start = i;
    m.call_len = d - i;
    if m.call_len == 0 || m.call_len > 8 {
        fail!("call sign length");
    }
    if !text[i..d].iter().all(|&c| c.is_ascii_uppercase() || c.is_ascii_digit() || c == b'/' || c == b' ') {
        fail!("bad call sign");
    }
    if d + 1 != text.len() {
        fail!("text after the message");
    }
    m.org_name = originator_name(&text[5..8]);
    m.event_name = event_name(&text[9..12]);
    m.event_kind = event_kind(&text[9..12]);
    m.valid = true;
    m
}

/// The message cut to what the format says (from "ZCZC" or "NNNN" up to the last '-'); returns (start, length), (0, 0) when neither marker is there.
pub fn trim_message(b: &[u8]) -> (usize, usize) {
    let find = |pat: &[u8]| b.windows(4).position(|w| w == pat);
    let (s, e) = (find(b"ZCZC"), find(b"NNNN"));
    let Some(s) = s else {
        return match e {
            Some(e) => (e, 4),
            None => (0, 0),
        };
    };
    if e.is_some_and(|e| e < s) {
        return (e.unwrap_or(0), 4);
    }
    let m = &b[s..];
    // cut after the call sign: the dash that follows "+TTTT-JJJHHMM-LLLLLLLL"
    if let Some(p) = m.iter().position(|&c| c == b'+') {
        if m.len() > p + 14 && m[p + 5] == b'-' && m[p + 13] == b'-' {
            if let Some(d) = m[p + 14..].iter().position(|&c| c == b'-').map(|q| q + p + 14) {
                if d <= p + 14 + 8 {
                    return (s, d + 1);
                }
            }
        }
    }
    let mut n = m.len();
    while n > 0 && !valid_char(m[n - 1]) {
        n -= 1;
    }
    (s, n)
}
