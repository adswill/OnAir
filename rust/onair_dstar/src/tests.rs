// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests for the D-STAR layer that do not need the C++ reference: CRC check value, header round trip through the FEC, GPS text.
use crate::gps::{self, Fix, Outcome};
use crate::{fec, header};

#[test]
fn crc_check_value() {
    assert_eq!(header::crc(b"123456789"), 0x906E);
}

#[test]
fn header_fec_round_trip() {
    let mut h = [0u8; 41];
    header::pack([0, 0, 0], b"DIRECT", b"DIRECT", b"CQCQCQ", b"LY1TST", b"D75", &mut h);
    let mut bits = [0u8; 660];
    fec::header_encode(&h, &mut bits);
    let mut soft = [0f32; 660];
    for i in 0..660 {
        soft[i] = if bits[i] != 0 { 1.0 } else { -1.0 };
    }
    for k in [5usize, 90, 300, 301, 590] {
        soft[k] = -soft[k];
    }
    let mut out = [0u8; 41];
    assert_eq!(fec::header_decode(&soft, &mut out), 5);
    assert_eq!(out, h);
    let (u, ok) = header::unpack(&out);
    assert!(ok && &u.my.b[..u.my.n] == b"LY1TST" && &u.sfx.b[..u.sfx.n] == b"D75");
}

#[test]
fn nmea_rmc() {
    let body = b"GPRMC,123519,A,5441.2320,N,02516.7800,E,019.4,090.0,230394,003.1,W";
    let mut line = [0u8; 80];
    line[0] = b'$';
    line[1..1 + body.len()].copy_from_slice(body);
    line[1 + body.len()] = b'*';
    line[2 + body.len()..4 + body.len()].copy_from_slice(&gps::nmea_checksum(body));
    let mut f = Fix::default();
    assert_eq!(gps::parse_nmea(&line[..4 + body.len()], &mut f), Outcome::Parsed);
    assert!((f.lat - 54.6872).abs() < 1e-4 && (f.lon - 25.2797).abs() < 1e-4 && f.vel_valid && f.course == 90);
}
