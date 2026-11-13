// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use crate::parse::{parse_pdu, put_fcs, Msg};
use crate::phy::*;
use crate::stations::*;

fn text(s: &impl AsRefBytes) -> String {
    String::from_utf8_lossy(s.bytes()).into_owned()
}
trait AsRefBytes {
    fn bytes(&self) -> &[u8];
}
impl<const N: usize> AsRefBytes for onair_common::text::Str<N> {
    fn bytes(&self) -> &[u8] {
        self.as_bytes()
    }
}

#[test]
fn fcs_check_value() {
    assert_eq!(fcs(b"123456789"), 0x906E);
    let mut v = b"123456789".to_vec();
    v.extend_from_slice(&[0x6E, 0x90]);
    assert!(fcs_ok(&v));
    v[3] ^= 1;
    assert!(!fcs_ok(&v));
}

#[test]
fn rates() {
    assert_eq!(coded_bits(3), 72 * 30 * 3);
    assert_eq!(payload_octets(0), (72 * 30 / 4 - 6) / 8);
    assert_eq!(burst_syms(7), 531 + 168 * 45);
    assert_eq!(rate(99).m1, 7);
    assert_eq!(rate(-4).m1, 0);
}

#[test]
fn interleaver_is_a_permutation() {
    for m in 0..8 {
        let n = coded_bits(m) as usize;
        let mut t = vec![0i32; 2 * n];
        assert_eq!(interleave(m, &mut t), n);
        let mut seen = vec![false; n];
        for &v in &t[..n] {
            assert!(!seen[v as usize]);
            seen[v as usize] = true;
        }
        let mut small = vec![0i32; n];
        assert_eq!(interleave(m, &mut small), usize::MAX);
    }
}

#[test]
fn scrambler_known_answer() {
    let mut s = [0i8; 240];
    scrambler_signs(&mut s);
    let want = b"00010011000110111100010000100101";
    for i in 0..32 {
        assert_eq!(s[i] < 0, want[i] == b'1', "bit {i}");
    }
    assert_eq!(&s[..120], &s[120..]);
}

#[test]
fn viterbi_round_trip() {
    let bits: Vec<u8> = (0..300u32).map(|i| (i.wrapping_mul(2654435761u32) >> 13 & 1) as u8).collect();
    let mut c = vec![0u8; 600];
    assert!(conv_encode(&bits, &mut c));
    let soft: Vec<f32> = c.iter().map(|&b| if b != 0 { -1.0 } else { 1.0 }).collect();
    let mut out = vec![0u8; 300];
    let mut surv = vec![0u64; 300];
    assert!(viterbi(&soft, 300, &mut out, &mut surv));
    assert_eq!(&out[..294], &bits[..294]);
    assert!(!viterbi(&soft, 301, &mut out, &mut surv));
}

#[test]
fn conv_order_is_6d_then_4f() {
    let mut c = [0u8; 2];
    assert!(conv_encode(&[1], &mut c));
    // the register holds 1: both polynomials have bit 0 set
    assert_eq!(c, [1, 1]);
    let mut c = [0u8; 4];
    assert!(conv_encode(&[1, 0], &mut c));
    // register 0b10: 0x6D bit 1 is 0, 0x4F bit 1 is 1
    assert_eq!(&c[2..], &[0, 1]);
}

#[test]
fn known_symbols() {
    let mut p = [0i8; PRE_LEN];
    for m in 0..8 {
        assert!(preamble_ref(m, &mut p));
        assert!(p.iter().all(|&v| v == 1 || v == -1));
    }
    assert!(!preamble_ref(0, &mut [0i8; 10]));
}

#[test]
fn stations_and_masks() {
    let mut o = [0i32; 20];
    let n = mask_khz(1, 0b101, &mut o);
    assert_eq!(&o[..n], &[21934, 13276]);
    let n = mask_khz(99, 0b11, &mut o);
    assert_eq!(&o[..n], &[-1, -2]);
    assert_eq!(station_on_khz(5508), 1);
    assert_eq!(station_on_khz(1), 0);
}

fn spdu_bytes(gs: u8) -> Vec<u8> {
    let mut b = vec![0u8; 66];
    b[0] = 0x20;
    b[1] = (gs & 0x7F) | 0x80;
    b[2] = 3;
    b[54] = 0x30;
    let f = fcs(&b[..64]);
    b[64] = f as u8;
    b[65] = (f >> 8) as u8;
    b
}

#[test]
fn spdu_text() {
    let mut out = vec![Msg::default(); 4];
    let n = parse_pdu(&spdu_bytes(1), &mut out).unwrap();
    assert_eq!(n, 1);
    assert_eq!(text(&out[0].kind), "SPDU");
    assert!(text(&out[0].summary).starts_with("Squitter San Francisco, frame 3"));
    assert!(text(&out[0].detail).contains("UTC synchronised"));
    let mut bad = spdu_bytes(1);
    bad[10] ^= 1;
    assert!(parse_pdu(&bad, &mut out).is_none());
    assert!(parse_pdu(&bad[..3], &mut out).is_none());
}

#[test]
fn logon_in_a_downlink_mpdu() {
    let mut lp = [0u8; 8];
    let l = put_fcs(&[0x8F, 0xD2, 0x48, 0x2C], &mut lp);
    let mut mp = vec![0x03 | (1 << 2), 1, 7, 0, 0, 0, (l - 1) as u8];
    let f = fcs(&mp);
    mp.extend_from_slice(&[f as u8, (f >> 8) as u8]);
    mp.extend_from_slice(&lp[..l]);
    let mut out = vec![Msg::default(); 4];
    assert_eq!(parse_pdu(&mp, &mut out), Some(1));
    assert_eq!(text(&out[0].kind), "Logon request");
    assert_eq!(out[0].icao, 0x4B1234);
    assert_eq!(text(&out[0].summary), "Logon request from 4B1234");
    assert_eq!(out[0].ac_id, 7);
}


#[test]
fn logon_icao_address_uses_bit_reversed_octets() {
    // Independently observed HFDL LPDU wire octets; reversing the *bits*
    // (not the octet order) gives these ICAO 24-bit addresses.
    for (wire, expected) in [
        ([0x1E, 0x38, 0x6B], 0x781CD6),
        ([0x1E, 0x08, 0xFC], 0x78103F),
    ] {
        let mut lp = [0u8; 8];
        let l = put_fcs(&[0x8F, wire[0], wire[1], wire[2]], &mut lp);
        let mut mp = vec![0x03 | (1 << 2), 1, 7, 0, 0, 0, (l - 1) as u8];
        let f = fcs(&mp);
        mp.extend_from_slice(&[f as u8, (f >> 8) as u8]);
        mp.extend_from_slice(&lp[..l]);
        let mut out = vec![Msg::default(); 4];
        assert_eq!(parse_pdu(&mp, &mut out), Some(1));
        assert_eq!(out[0].icao, expected);
        assert!(text(&out[0].summary).contains(&format!("{expected:06X}")));
    }
}

#[test]
fn mutated_input_never_panics() {
    let base = spdu_bytes(2);
    let mut out = vec![Msg::default(); 128];
    let mut x = 12345u32;
    for _ in 0..20000 {
        let mut v = base.clone();
        for _ in 0..3 {
            x = x.wrapping_mul(1664525).wrapping_add(1013904223);
            let i = (x >> 8) as usize % v.len();
            v[i] = (x >> 24) as u8;
        }
        x = x.wrapping_mul(1664525).wrapping_add(1013904223);
        let n = (x >> 8) as usize % (v.len() + 1);
        let _ = parse_pdu(&v[..n], &mut out);
        let _ = parse_pdu(&v[..n], &mut out[..0]);
    }
}
