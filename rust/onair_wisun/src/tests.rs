// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests of the Wi-SUN layers that do not need the C++ reference: check values, the whitening sequence, a built-by-hand PAN advertisement.
use crate::{mac, phy};

#[test]
fn crc_check_values() {
    assert_eq!(phy::crc32(b"123456789"), 0xCBF4_3926);
    assert_eq!(phy::crc16(b"123456789"), 0x2189);
    let mut p = b"hello".to_vec();
    let c = phy::crc32(&p);
    p.extend_from_slice(&c.to_le_bytes());
    assert!(phy::fcs_ok(&p, false));
    p[0] ^= 1;
    assert!(!phy::fcs_ok(&p, false));
}

#[test]
fn phr_round_trip() {
    let p = phy::parse_phr(0x9A3F);
    assert_eq!((p.mode_switch, p.fcs2, p.whitened, p.length), (1, 1, 1, 0x23F));
    assert_eq!(phy::build_phr(&p), 0x9A3F & !0x6000);
}

#[test]
fn whitening_sequence() {
    let mut d = [0u8; 8];
    phy::whiten(&mut d);
    assert_eq!(d, [0xf0, 0x0e, 0xcd, 0xf6, 0xc2, 0x19, 0x12, 0x75]);
    phy::whiten(&mut d);
    assert_eq!(d, [0u8; 8]);
}

#[test]
fn parse_pan_advert() {
    // frame control: data, IE list, dest none, version 2, src extended, PAN compression off, seq suppressed
    let mut m: Vec<u8> = vec![];
    let fc: u16 = 1 | 1 << 8 | 1 << 9 | 2 << 12 | 3 << 14;
    m.extend_from_slice(&fc.to_le_bytes());
    m.extend_from_slice(&0xFFFFu16.to_le_bytes()); // source PAN
    m.extend_from_slice(&0x18DF_C1FF_FE8B_0D4Du64.to_le_bytes());
    // WH-IE UTT: PAN advert (0), UFSI 0
    m.extend_from_slice(&((5u16) | 0x2A << 7).to_le_bytes());
    m.extend_from_slice(&[0x01, 0x00, 0, 0, 0]);
    m.extend_from_slice(&(0x7Eu16 << 7).to_le_bytes()); // header termination 1
                                                        // WP-IE with a NETNAME sub-IE "Net"
    let sub = [(3u16 | 0x05 << 8) as u8, 0x05, b'N', b'e', b't'];
    m.extend_from_slice(&((sub.len() as u16) | 4 << 11 | 0x8000).to_le_bytes());
    m.extend_from_slice(&sub);
    let f = mac::parse(&m);
    assert_eq!((f.ok, f.utt, f.has_net_name), (1, 0, 1));
    assert_eq!(f.net_name.as_bytes(), b"Net");
    assert_eq!(f.src_ext, 0x18DF_C1FF_FE8B_0D4D);
    assert_eq!(f.ies.as_bytes(), b"UTT HT1 NETNAME");
    assert_eq!(f.info.as_bytes(), b"PAN Advert \"Net\"");
}
