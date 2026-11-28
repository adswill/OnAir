// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests for the NXDN layer: LICH round trip, CRC sensitivity, channel encode / decode with bit errors, message round trips.
use crate::chan::{self, Ch};
use crate::lich::{self, Lich};
use crate::{crc, msg};

#[test]
fn lich_round_trip() {
    for v in 0..128 {
        let l = Lich { rfct: (v >> 5) & 3, fct: (v >> 3) & 3, opt: (v >> 1) & 3, dir: v & 1 };
        let e = lich::encode(&l);
        assert_eq!(lich::decode(e), Some(l));
        assert_eq!(lich::decode(e ^ 0x80), None);
    }
}

#[test]
fn pn9_starts_with_seed() {
    let mut p = [0u8; 8];
    lich::pn9(&mut p);
    assert_eq!(p, [0, 0, 1, 0, 0, 1, 1, 1]);
}

#[test]
fn crc_catches_bit_error() {
    let mut b = [0u8; 40];
    for (i, x) in b.iter_mut().enumerate() {
        *x = ((i * 7 + 3) % 5 < 2) as u8;
    }
    let (a, c, d) = (crc::crc6(&b), crc::crc12(&b), crc::crc16(&b));
    b[13] ^= 1;
    assert!(crc::crc6(&b) != a && crc::crc12(&b) != c && crc::crc16(&b) != d);
}

#[test]
fn channels_round_trip_and_fix_errors() {
    for (c, fix) in [(Ch::Sacch, 3usize), (Ch::Facch1, 3), (Ch::Cac, 10)] {
        let ni = chan::info_bits(c);
        let na = chan::air_bits(c);
        let mut info = [0u8; 155];
        for (i, x) in info.iter_mut().enumerate() {
            *x = ((i * 13 + 5) % 7 < 3) as u8;
        }
        let mut air = [0u8; 300];
        chan::encode(c, &info[..ni], &mut air[..na]).unwrap();
        let mut soft = [0f32; 300];
        for k in 0..na {
            soft[k] = if air[k] != 0 { 1.0 } else { -1.0 };
        }
        for e in 0..fix {
            let k = (e * 977 + 11) % na;
            soft[k] = -soft[k];
        }
        let mut out = [0u8; 155];
        let d = chan::decode(c, &soft[..na], &mut out[..ni]).unwrap();
        assert!(d.ok);
        assert_eq!(&out[..ni], &info[..ni]);
        assert_eq!(d.errs as usize, fix);
    }
}

#[test]
fn message_round_trips() {
    let mut m = msg::Msg { typ: msg::VCALL, call_type: 4, option: 9, src: 1234, dst: 4321, cipher: 2, key_id: 17, ..Default::default() };
    let mut bits = [0u8; 72];
    assert_eq!(msg::build(&m, &mut bits), 64);
    let mut p = msg::Msg::default();
    assert!(msg::parse(&bits[..64], &mut p));
    assert_eq!(p, m);
    assert!(!msg::parse(&bits[..63], &mut p));
    m = msg::Msg { typ: msg::SD_DATA, block: 3, text: *b"HELLO\0\0\0", text_len: 5, ..Default::default() };
    assert_eq!(msg::build(&m, &mut bits), 80);
    assert!(!msg::parse(&bits[..70], &mut p));
    assert_eq!(p.typ, msg::SD_DATA);
}

#[test]
fn vcall_octets_match_nxdn_layout() {
    // Octets from a clear group call (issue #716): type 01, CC option 00, call type 001 in the top bits of octet 2, source 1001, talkgroup 200, cipher 0, key 0.
    let bytes = [0x01u8, 0x00, 0x20, 0x03, 0xE9, 0x00, 0xC8, 0x00];
    let bits: Vec<u8> = bytes.iter().flat_map(|&b| (0..8).rev().map(move |i| (b >> i) & 1)).collect();
    let mut p = msg::Msg::default();
    assert!(msg::parse(&bits, &mut p));
    assert_eq!(p.typ, msg::VCALL);
    assert_eq!((p.option, p.call_type), (0, 1));
    assert_eq!((p.src, p.dst), (1001, 200));
    assert_eq!((p.cipher, p.key_id), (0, 0));
}

#[test]
fn voice_call_assignment_round_trips() {
    let m = msg::Msg { typ: msg::VCALL_ASSGN, call_type: 1, option: 0, src: 1001, dst: 2002, channel: 517, ..Default::default() };
    let mut bits = [0u8; 80];
    assert_eq!(msg::build(&m, &mut bits), 58);
    let mut p = msg::Msg::default();
    assert!(msg::parse(&bits[..58], &mut p));
    assert_eq!(p, m);
    assert!(!msg::parse(&bits[..40], &mut p));
    assert_eq!(msg::msg_name(msg::VCALL_ASSGN), "VCALL_ASSGN\0");
}
