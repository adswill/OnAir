// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests for the P25 layer that do not need the C++ reference: NID round trip with errors, Reed-Solomon correction, Golay / Hamming, trellis, TSBK and link control.
use crate::msg::{self, Lc, Tsbk};
use crate::{golay, nid, rs, trellis};

#[test]
fn nid_corrects_errors() {
    let mut bits = [0u8; 64];
    nid::encode(0x293, msg::LDU1, &mut bits);
    for k in [3usize, 17, 30, 41, 59] {
        bits[k] ^= 1;
    }
    let r = nid::decode(&bits);
    assert!(r.ok && r.nac == 0x293 && r.duid == msg::LDU1 && r.errs == 5);
}

#[test]
fn reed_solomon_round_trip() {
    for (n, k) in [(36usize, 20usize), (24, 12), (24, 16)] {
        let mut sym = [0u8; 63];
        for i in 0..k {
            sym[i] = ((i * 7 + 3) % 64) as u8;
        }
        assert!(rs::encode(&mut sym, n, k));
        let good = sym;
        let t = (n - k) / 2;
        for e in 0..t {
            sym[(e * 5 + 1) % n] ^= (e + 1) as u8;
        }
        assert_eq!(rs::decode(&mut sym, n, k), (true, t as i32));
        assert_eq!(sym[..n], good[..n]);
    }
}

#[test]
fn golay_and_hamming() {
    for d in [0, 1, 0x555, 0xABC, 0xFFF] {
        let cw = golay::golay24_encode(d);
        assert_eq!(golay::golay24_decode(cw ^ 0x10101), (d, 3));
    }
    for h in 0..64 {
        assert_eq!(golay::golay18_decode(golay::golay18_encode(h) ^ 0x4), (h, 1));
        assert_eq!(golay::hamming10_decode(golay::hamming10_encode(h) ^ 0x20), (h, 1));
    }
}

#[test]
fn trellis_round_trip() {
    let mut bits = [0u8; 96];
    for (i, b) in bits.iter_mut().enumerate() {
        *b = ((i * 11 + 2) % 5 < 2) as u8;
    }
    let mut d = [0u8; 98];
    trellis::encode(&bits, &mut d);
    let mut lv = [0f32; 98];
    for i in 0..98 {
        lv[i] = trellis::dibit_to_sym(i32::from(d[i])) as f32;
    }
    let mut out = [0u8; 96];
    assert_eq!(trellis::decode(&lv, &mut out), (0.0, true));
    assert_eq!(out, bits);
}

#[test]
fn tsbk_and_lc() {
    let t = Tsbk { last: true, prot: false, opcode: msg::OP_GRP_V_GRANT, mfid: 0, args: 0x0123_4567_89AB_CDEF };
    let mut b = [0u8; 96];
    msg::tsbk_bits(&t, &mut b);
    assert_eq!(msg::tsbk_parse(&b), (t, true));
    b[7] ^= 1;
    assert!(!msg::tsbk_parse(&b).1);
    let l = Lc { lco: msg::LC_GROUP, mfid: 0, prot: false, svc_opts: 0x80, tg: 1234, src: 567_890, dst: -1, raw: 0 };
    let mut hex = [0u8; 12];
    msg::lc_hexbits(&l, &mut hex);
    let p = msg::lc_parse(&hex);
    assert_eq!((p.lco, p.svc_opts, p.tg, p.src), (0, 0x80, 1234, 567_890));
    let mut s = [0u8; 8];
    let n = msg::channel_text(0x1005, &mut s);
    assert_eq!(&s[..n], b"1-5");
}
