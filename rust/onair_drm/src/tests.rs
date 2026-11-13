// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests that need no C++ reference: CRC check, a FAC round trip built by hand, SDC entities, the super frame split, MJD conversion, names.
use crate::bits::{crc16, crc8};
use crate::fac::fac_parse;
use crate::sdc::{sdc_check_bits, sdc_parse, SdcInfo};
use crate::{aac, data, names};

/// Push the bits of `v` (MSB first).
fn put(b: &mut Vec<u8>, v: u32, n: u32) {
    for i in (0..n).rev() {
        b.push(((v >> i) & 1) as u8);
    }
}

#[test]
fn crc_values() {
    // all-ones start and complement: the CRC of no bits is 0
    assert_eq!(crc8(&[]), 0);
    assert_eq!(crc16(&[]), 0);
    assert_ne!(crc8(&[1, 0, 1]), crc8(&[1, 0, 0]));
}

#[test]
fn fac_round_trip() {
    let mut b = Vec::new();
    put(&mut b, 0, 1); // base
    put(&mut b, 1, 2); // identity
    put(&mut b, 0, 1); // RM flag
    put(&mut b, 3, 3); // occupancy
    put(&mut b, 1, 1);
    put(&mut b, 0, 2);
    put(&mut b, 1, 1);
    put(&mut b, 5, 4); // one audio service
    put(&mut b, 2, 3);
    put(&mut b, 1, 1);
    put(&mut b, 0, 1);
    put(&mut b, 0x123456, 24);
    put(&mut b, 2, 2);
    put(&mut b, 1, 1);
    put(&mut b, 5, 4);
    put(&mut b, 0, 1);
    put(&mut b, 3, 5);
    put(&mut b, 0, 1);
    put(&mut b, 0, 6);
    let c = crc8(&b);
    put(&mut b, c, 8);
    assert_eq!(b.len(), 72);
    let f = fac_parse(&b, false).unwrap();
    assert_eq!(
        (f.identity, f.occupancy, f.num_services_code, f.svc[0].id, f.svc[0].language, f.svc[0].descriptor),
        (1, 3, 5, 0x123456, 5, 3)
    );
    b[10] ^= 1;
    assert!(fac_parse(&b, false).is_none());
    assert!(fac_parse(&b, true).is_none());
}

#[test]
fn sdc_label_and_check() {
    // label entity of short id 1: length 5, version 0, type 1, nibble = short id << 2; text control 0 then "Hi"
    let d = [5 << 1, (1 << 4) | (1 << 2), b'R', b'a', b'd', b'i', b'o', 0, 0];
    let mut info = SdcInfo::default();
    let mut other = [0u8; 8];
    assert_eq!(sdc_parse(&d, &mut info, &mut other), 1);
    assert_eq!((info.label[1].present, info.label[1].n_text), (1, 5));
    assert_eq!(&info.label[1].text[..5], b"Radio");
    // an unknown type goes to `other`
    let mut info2 = SdcInfo::default();
    let n = sdc_parse(&[2 << 1, 0xF0, 1, 2], &mut info2, &mut other);
    assert_eq!((n, info2.n_other, other[0]), (1, 1, 15));
    // a block with a good CRC
    let mut bits = Vec::new();
    put(&mut bits, 9, 4);
    for byte in [1u8, 2, 3] {
        put(&mut bits, byte as u32, 8);
    }
    let mut crc_in = vec![0u8; 4];
    put(&mut crc_in, 9, 4);
    for byte in [1u8, 2, 3] {
        put(&mut crc_in, byte as u32, 8);
    }
    let c = crc16(&crc_in);
    put(&mut bits, c, 16);
    assert_eq!(sdc_check_bits(&bits, 3), (9, true));
    bits[6] ^= 1;
    assert!(!sdc_check_bits(&bits, 3).1);
}

#[test]
fn super_frame_and_tables() {
    assert_eq!(
        (aac::num_frames(false, 12000), aac::num_frames(false, 24000), aac::num_frames(true, 48000), aac::num_frames(true, 12000)),
        (5, 10, 10, 0)
    );
    assert_eq!((aac::header_bytes(5), aac::header_bytes(10)), (6, 14));
    assert_eq!(data::date_to_mjd(2026, 10, 17), 61330);
    assert_eq!(data::mjd_to_date(61330), (2026, 10, 17));
    assert_eq!(names::language_name(5), b"English");
    assert_eq!(names::app_name(0, 0x44A), b"Journaline");
    assert_eq!((names::fac_audio_services(5), names::fac_data_services(15)), (1, 4));
}

#[test]
fn text_helpers() {
    let mut out = [0u8; 64];
    let n = data::display_text(b"a\r\nb\x01\xE9", &mut out);
    assert_eq!(&out[..n], b"a\nb \xC3\xA9");
    assert!(data::looks_text(b"hello world"));
    assert!(!data::looks_text(b"he\0llo"));
}
