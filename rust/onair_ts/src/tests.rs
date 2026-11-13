// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests that need no C++ reference: CRC check value, packet header, PAT packet round trip, a small PAT / SDT / TDT / EIT.
use crate::crc::mpeg_crc32;
use crate::eit::{parse_eit, EitEvent};
use crate::packet::{packet_header, pat_packet};
use crate::si::{parse_pat, parse_sdt, parse_tdt, PatEntry, SdtService};

/// Append the CRC_32 of a section.
fn with_crc(mut v: Vec<u8>) -> Vec<u8> {
    let c = mpeg_crc32(&v);
    v.extend_from_slice(&c.to_be_bytes());
    v
}

#[test]
fn crc_check_value() {
    assert_eq!(mpeg_crc32(b"123456789"), 0x0376_E6E7);
}

#[test]
fn pat_packet_round_trip() {
    let mut p = [0u8; 188];
    pat_packet(0x1234, 0x0101, 0x0100, 7, &mut p);
    let h = packet_header(&p).unwrap();
    assert_eq!((h.pid, h.pusi, h.cc, h.afc, h.payload_off), (0, 1, 7, 1, 4));
    let sec = &p[5..5 + 16];
    assert_eq!(mpeg_crc32(sec), 0);
    let mut e = [PatEntry::default(); 4];
    let (tsid, n) = parse_pat(sec, &mut e).unwrap();
    assert_eq!((tsid, n, e[0].prog, e[0].pid), (0x1234, 1, 0x0101, 0x0100));
}

#[test]
fn bad_sync_and_short() {
    assert!(packet_header(&[0u8; 188]).is_none());
    assert!(packet_header(&[0x47, 0, 0]).is_none());
    assert!(parse_pat(&[0u8; 7], &mut []).is_none());
}

#[test]
fn sdt_service_name() {
    let mut v = vec![0x42, 0xF0, 0, 0x12, 0x34, 0xC1, 0, 0, 0x20, 0x01, 0xFF];
    let desc = [0x48, 12, 0x01, 3, b'A', b'B', b'C', 4, b'T', b'e', b's', b't'];
    v.extend_from_slice(&[0x01, 0x01, 0xFC, 0x80, desc.len() as u8]);
    v.extend_from_slice(&desc);
    let n = v.len() + 4 - 3;
    v[2] = n as u8;
    let s = with_crc(v);
    let mut o = [SdtService::default(); 4];
    let (tsid, onid, c) = parse_sdt(&s, &mut o).unwrap();
    assert_eq!((tsid, onid, c, o[0].sid, o[0].typ), (0x1234, 0x2001, 1, 0x0101, 1));
    let (p, nm) = (o[0].provider, o[0].name);
    assert_eq!(&s[p.off as usize..(p.off + p.len) as usize], b"ABC");
    assert_eq!(&s[nm.off as usize..(nm.off + nm.len) as usize], b"Test");
}

#[test]
fn tdt_date() {
    // MJD 0xC079 = 49273 = 1993-10-13, 12:45:00 (EN 300 468 Annex C example)
    let t = parse_tdt(&[0x70, 0x70, 5, 0xC0, 0x79, 0x12, 0x45, 0x00]).unwrap();
    assert_eq!(&t.text[..t.text_len as usize], b"1993-10-13 12:45:00 UTC");
    assert_eq!(t.unix, (49273 - 40587) * 86400 + 12 * 3600 + 45 * 60);
}

#[test]
fn eit_event() {
    let mut v = vec![0x4E, 0xF0, 0, 0x01, 0x01, 0xC1, 0, 0, 0x20, 0x01, 0x00, 0x01, 0x4E, 0x4E];
    let desc = [0x4D, 9, b'e', b'n', b'g', 3, b'N', b'e', b'w', 1, b'x'];
    v.extend_from_slice(&[0x00, 0x05, 0xC0, 0x79, 0x12, 0x00, 0x00, 0x00, 0x30, 0x00, 0x80, desc.len() as u8]);
    v.extend_from_slice(&desc);
    let n = v.len() + 4 - 3;
    v[2] = n as u8;
    let s = with_crc(v);
    let mut o = [EitEvent::default(); 4];
    let i = parse_eit(&s, &mut o).unwrap();
    assert_eq!((i.sid, i.n_events, i.legacy, o[0].event_id, o[0].duration), (0x0101, 1, 1, 5, 1800));
    let t = o[0].title;
    assert_eq!(&s[t.off as usize..(t.off + t.len) as usize], b"New");
}
