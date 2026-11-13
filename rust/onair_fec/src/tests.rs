// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests of onair_fec: round trips and known vectors (the C++ differential test covers equality with the old code).
use crate::bch::BchCtx;
use crate::gf::GF;
use crate::rs;

fn lcg(s: &mut u64) -> u32 {
    *s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    (*s >> 33) as u32
}

#[test]
fn gf_tables() {
    assert_eq!(GF.exp[8], 0x1D); // alpha^8 = x^4 + x^3 + x^2 + 1
    for a in 1..=255u8 {
        assert_eq!(GF.mul(a, GF.inv(a)), 1);
    }
}

#[test]
fn rs_roundtrip_all_codes() {
    let mut s = 7u64;
    for iter in 0..300 {
        let mut data = [0u8; 188];
        for d in data.iter_mut() {
            *d = lcg(&mut s) as u8;
        }
        let mut cw = [0u8; 204];
        assert!(rs::DVBT.encode(&data, &mut cw));
        assert_eq!(&cw[..188], &data[..]);
        let mut w = cw;
        assert_eq!(rs::DVBT.decode(&mut w, false), 0);
        let nerr = iter % 9; // 0..8 errors: all correctable
        for _ in 0..nerr {
            let p = lcg(&mut s) as usize % 204;
            w[p] ^= (lcg(&mut s) as u8) | 1;
        }
        let r = rs::DVBT.decode(&mut w, false);
        assert!(r >= 0 && r as usize <= nerr, "{r} {nerr}");
        assert_eq!(w, cw);
    }
    let mut data = [0u8; 187];
    data.iter_mut().enumerate().for_each(|(i, d)| *d = (i * 7 + 3) as u8);
    let mut cw = [0u8; 207];
    assert!(rs::ATSC.encode(&data, &mut cw));
    let mut w = cw;
    for k in 0..10 {
        w[k * 20 + 1] ^= 0x55;
    }
    assert_eq!(rs::ATSC.decode(&mut w, false), 10);
    assert_eq!(w, cw);
    let mut d2 = [0u8; 110];
    d2.iter_mut().enumerate().for_each(|(i, d)| *d = (i * 13 + 1) as u8);
    let mut c2 = [0u8; 120];
    assert!(rs::DAB.encode(&d2, &mut c2));
    let mut w2 = c2;
    for k in 0..6 {
        w2[k * 19] ^= 0x81;
    }
    let before = w2;
    assert_eq!(rs::DAB.decode(&mut w2, true), -1);
    assert_eq!(w2, before, "the DAB variant keeps a word it cannot correct");
}

#[test]
fn rs_bad_lengths() {
    let mut w = [0u8; 10];
    assert_eq!(rs::DVBT.decode(&mut w, false), -2);
    let mut o = [0u8; 5];
    assert!(!rs::DVBT.encode(&[1, 2, 3], &mut o));
}

fn ctx(short: bool, t: u32) -> std::boxed::Box<[u64]> {
    let mut v = vec![0u64; BchCtx::size() / 8 + 1].into_boxed_slice();
    #[allow(unsafe_code)]
    unsafe {
        assert!((*(v.as_mut_ptr() as *mut BchCtx)).init(short, t));
    }
    v
}

#[allow(unsafe_code)]
fn as_bch(v: &[u64]) -> &BchCtx {
    unsafe { &*(v.as_ptr() as *const BchCtx) }
}

#[test]
fn bch_roundtrip() {
    let mut s = 99u64;
    for (short, t, k) in [(false, 12u32, 38688usize), (false, 10, 53840), (true, 12, 7032)] {
        let c = ctx(short, t);
        let b = as_bch(&c);
        let deg = b.parity_bits();
        assert_eq!(deg, if short { 14 } else { 16 } * t as usize);
        let mut bits = vec![0u8; k + deg];
        for x in bits.iter_mut().take(k) {
            *x = (lcg(&mut s) & 1) as u8;
        }
        assert!(b.encode(&mut bits, k));
        let good = bits.clone();
        assert_eq!(b.decode(&mut bits), 0);
        for nerr in 1..=t as usize {
            let mut w = good.clone();
            for i in 0..nerr {
                w[(i * 7919 + 13) % (k + deg)] ^= 1;
            }
            assert_eq!(b.decode(&mut w), nerr as i32);
            assert_eq!(w, good);
        }
        let mut w = good.clone();
        for i in 0..(t as usize + 3) {
            w[(i * 104729 + 5) % (k + deg)] ^= 1;
        }
        let r = b.decode(&mut w);
        assert!(r == -1 || r > t as i32 || w != good || r >= 0);
    }
}

fn conv_encode(bits: &[u8]) -> Vec<i8> {
    let mut s = 0u32; // 6 bit register, newest bit on top
    let mut soft = Vec::new();
    for &u in bits {
        let reg = ((u as u32) << 6) | s;
        let x = (reg & 0x79).count_ones() & 1;
        let y = (reg & 0x5B).count_ones() & 1;
        soft.push(if x == 1 { -40 } else { 40 });
        soft.push(if y == 1 { -40 } else { 40 });
        s = ((u as u32) << 5) | (s >> 1);
    }
    soft
}

#[test]
fn viterbi_decodes_and_neon_equals_scalar() {
    let mut s = 5u64;
    for rep in 0..40 {
        let steps = 50 + rep * 37;
        let mut bits: Vec<u8> = (0..steps).map(|_| (lcg(&mut s) & 1) as u8).collect();
        for b in bits.iter_mut().rev().take(6) {
            *b = 0; // flush
        }
        let mut soft = conv_encode(&bits);
        for _ in 0..steps / 20 {
            let i = lcg(&mut s) as usize % soft.len();
            soft[i] = -soft[i];
        }
        let mut out = vec![0u8; steps];
        let mut dec = vec![0u64; steps];
        let m = crate::viterbi::range(&soft, steps, &mut out, &mut dec).unwrap();
        assert_eq!(out, bits, "rep {rep}");
        let mut out2 = vec![0u8; steps];
        let mut dec2 = vec![0u64; steps];
        let m2 = crate::viterbi::range_scalar(&soft, steps, &mut out2, &mut dec2);
        assert_eq!((out, dec, m), (out2, dec2, m2));
        // random soft values (not a codeword) must agree too
        let noise: Vec<i8> = (0..2 * steps).map(|_| lcg(&mut s) as i8).collect();
        let (mut a, mut b) = (vec![0u8; steps], vec![0u8; steps]);
        let (mut da, mut db) = (vec![0u64; steps], vec![0u64; steps]);
        let ma = crate::viterbi::range(&noise, steps, &mut a, &mut da).unwrap();
        let mb = crate::viterbi::range_scalar(&noise, steps, &mut b, &mut db);
        assert_eq!((a, da, ma), (b, db, mb));
    }
    assert!(crate::viterbi::range(&[0; 3], 2, &mut [0; 2], &mut [0; 2]).is_none());
}
