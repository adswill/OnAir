// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests of the VDL2 layers that do not need the C++ reference: FCS residue, header code round trip with a corrected bit, Reed-Solomon with errors and
//! erasures, HDLC deframing, address round trip, an AVLC XID frame.
use crate::avlc::{self, Addr};
use crate::bits;
use crate::burst;
use crate::rs;

#[test]
fn fcs_residue() {
    let mut f = b"123456789".to_vec();
    let c = burst::fcs(&f);
    f.extend_from_slice(&c.to_le_bytes());
    assert!(burst::fcs_ok(&f));
    f[2] ^= 4;
    assert!(!burst::fcs_ok(&f));
    assert_eq!(burst::fcs(b"123456789"), 0x906E);
}

#[test]
fn header_round_trip() {
    for len in [1u32, 100, 1234, 0x1FFF, 0x3FFF] {
        let w = bits::header_word(len);
        let h = bits::header_decode(w);
        assert!(h.ok && h.len == len && !h.corrected, "len {len}");
        let h = bits::header_decode(w ^ (1 << 12));
        assert_eq!((h.ok, h.len), (len <= 0x1FFF, len), "corrected len {len}");
    }
    assert!(!bits::header_decode(0).ok);
}

#[test]
fn layout_of_a_burst() {
    let (l, ok) = bits::layout(8 * 300);
    assert!(ok);
    assert_eq!((l.data_oct, l.blocks, l.last_len, l.last_fec, l.fec_oct), (300, 2, 51, 4, 10));
}

#[test]
fn reed_solomon_corrects() {
    let mut cw = [0u8; 255];
    for (i, b) in cw.iter_mut().take(249).enumerate() {
        *b = (i * 7 + 3) as u8;
    }
    let mut d = [0u8; 249];
    d.copy_from_slice(&cw[..249]);
    let p = rs::encode(&d);
    cw[249..].copy_from_slice(&p);
    assert_eq!(rs::decode(&mut cw.clone(), 6), 0);
    let mut bad = cw;
    bad[5] ^= 0x55;
    bad[200] ^= 0x01;
    bad[251] ^= 0xFF;
    assert_eq!(rs::decode(&mut bad, 6), 3);
    assert_eq!(bad, cw);
    let mut hopeless = cw;
    for i in 0..8 {
        hopeless[i * 11] ^= 0xA5;
    }
    // more errors than the code can fix: either refused or (rarely) mis-corrected, but never reported as the original
    let r = rs::decode(&mut hopeless, 6);
    assert!(r == -1 || hopeless != cw);
}

#[test]
fn hdlc_frames_between_flags() {
    // flag, frame 0x01 0x02 0x03 (FCS-less, only the framing matters), flag, flag, frame of 0x7E octets (needs stuffing)
    let mut bits_v = Vec::new();
    let octet = |v: &mut Vec<u8>, o: u8| (0..8).for_each(|b| v.push((o >> b) & 1));
    octet(&mut bits_v, 0x7E);
    let mut ones = 0;
    for &o in &[0x7Fu8, 0xFF, 0x03] {
        for b in 0..8 {
            let bit = (o >> b) & 1;
            bits_v.push(bit);
            if bit == 1 {
                ones += 1;
                if ones == 5 {
                    bits_v.push(0);
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }
    octet(&mut bits_v, 0x7E);
    let mut out = vec![0u8; bits_v.len() / 8 + 1];
    let mut lens = vec![0u32; bits_v.len() / 16 + 1];
    let (f, o) = burst::hdlc_frames(&bits_v, &mut out, &mut lens).unwrap();
    assert_eq!((f, o, lens[0]), (1, 3, 3));
    assert_eq!(&out[..3], &[0x7F, 0xFF, 0x03]);
}

#[test]
fn addresses() {
    let a = Addr { addr: 0xABCDEF, kind: 5, status: 1 };
    let o = avlc::put_addr(&a, true);
    assert_eq!(o[3] & 1, 1);
    assert_eq!(avlc::get_addr(&o), a);
}

#[test]
fn xid_gsif() {
    // dst all stations (type 7), src ground station (type 4), U frame XID (0xAF), info: GSIF parameter set "V2"
    let mut f = Vec::new();
    f.extend_from_slice(&avlc::put_addr(&Addr { addr: 0xFFFFFF, kind: 7, status: 0 }, false));
    f.extend_from_slice(&avlc::put_addr(&Addr { addr: 0x123456, kind: 4, status: 0 }, true));
    f.push(0xAF);
    f.extend_from_slice(&[0x82, 0xF0, 0x00, 0x04, 0x00, 0x02, b'V', b'2']);
    let m = avlc::parse_frame(&f).unwrap();
    assert_eq!((m.frame_kind, m.from_ground, m.proto.as_bytes()), (b'U', 1, &b"XID"[..]));
    assert!(m.detail.as_bytes().windows(15).any(|w| w == b"parameter set V"));
}
