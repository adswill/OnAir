// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use crate::locate::{self, Country};
use crate::text::Buf;
use crate::{county, parse, time};

const MSG: &[u8] = b"ZCZC-WXR-TOR-029001-029003+0030-1231415-KEAX/NWS-";

fn s(b: &[u8]) -> &str {
    core::str::from_utf8(b).unwrap()
}

#[test]
fn parses_a_tornado_warning() {
    let m = parse::parse(MSG);
    assert!(m.valid && !m.eom);
    assert_eq!(m.nloc, 2);
    assert_eq!((m.purge_min, m.issue_day, m.issue_hour, m.issue_min), (30, 123, 14, 15));
    assert_eq!(&MSG[m.call_start..m.call_start + m.call_len], b"KEAX/NWS");
    assert_eq!(m.event_name, "Tornado Warning\0");
    assert_eq!(m.event_kind, "warning\0");
    assert_eq!(m.org_name, "National Weather Service\0");
}

#[test]
fn end_of_message_and_errors() {
    let e = parse::parse(b"NNNN");
    assert!(e.valid && e.eom);
    assert_eq!(parse::parse(b"ZCZC-WXR-TOR-029001+0000-1231415-KEAX/NWS-").err, "purge time zero\0");
    assert_eq!(parse::parse(b"junk").err, "does not start with ZCZC-\0");
    assert_eq!(parse::parse(b"ZCZC-WXR-TOR-029001+0030-1231415-KEAX/NWS-x").err, "text after the message\0");
}

#[test]
fn trims_noise() {
    let mut v = [b'x'; 3].to_vec();
    v.extend_from_slice(MSG);
    v.extend_from_slice(b"\x00\xff");
    let (st, l) = parse::trim_message(&v);
    assert_eq!(&v[st..st + l], MSG);
    assert_eq!(parse::trim_message(b"abc"), (0, 0));
}

#[test]
fn locations_in_words() {
    let mut buf = [0u8; 200];
    let mut b = Buf::new(&mut buf);
    locate::locate(b"129201", Country::Us, b"", &mut b);
    assert_eq!(s(&buf[..b_len(&buf)]), "Northwest part of county FIPS 201, Missouri");
    let mut buf = [0u8; 200];
    let mut b = Buf::new(&mut buf);
    locate::locate(b"048000", Country::Us, b"", &mut b);
    assert_eq!(s(&buf[..b_len(&buf)]), "All of Texas");
    let mut buf = [0u8; 200];
    let mut b = Buf::new(&mut buf);
    locate::locate(b"048201", Country::Us, b"Harris County", &mut b);
    assert_eq!(s(&buf[..b_len(&buf)]), "Harris County, TX");
}

fn b_len(b: &[u8]) -> usize {
    b.iter().position(|&c| c == 0).unwrap_or(b.len())
}

#[test]
fn times() {
    assert_eq!(time::issue_epoch(2024, 1, 0, 1), 1_704_067_260);
    assert_eq!(time::issue_epoch(2024, 367, 0, 0), 0);
    let mut buf = [0u8; 32];
    let mut b = Buf::new(&mut buf);
    time::issue_text(2024, 123, 14, 30, &mut b);
    assert_eq!(s(&buf[..b_len(&buf)]), "May 02 14:30 UTC"); // 2024 is a leap year: day 123 is 2 May
    let mut buf = [0u8; 32];
    let mut b = Buf::new(&mut buf);
    time::purge_text(90, &mut b);
    assert_eq!(s(&buf[..b_len(&buf)]), "1 h 30 min");
}

#[test]
fn county_csv() {
    let mut out = [0u8; 200];
    let mut scr = [0u8; 100];
    let n = county::read_csv(b"State,x\n48201,\"Harris, TX\"\nAL,01,001,Autauga County,H1\n01,003,Baldwin\n", &mut out, &mut scr).unwrap();
    // three records: 48201, 1001, 1003
    let mut at = 0;
    let mut keys = [0u32; 3];
    for k in &mut keys {
        *k = u32::from_le_bytes(out[at..at + 4].try_into().unwrap());
        let l = u32::from_le_bytes(out[at + 4..at + 8].try_into().unwrap()) as usize;
        at += 8 + l;
    }
    assert_eq!(at, n);
    assert_eq!(keys, [48201, 1001, 1003]);
}
