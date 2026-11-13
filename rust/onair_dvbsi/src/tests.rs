// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests that need no C++ reference: Hamming codes, character sets, the teletext data unit, the renderer, subtitle pixel strings and colours.
use crate::charset::{self, Out};
use crate::subpix::{decode_field, default_clut, ycrcb};
use crate::teletext::{decode_unit, g0, hamming2418, hamming84, pes_units, reverse8, UNIT_HEADER, UNIT_IGNORE};
use crate::ttxrender::{render, row_text, Cell, COLS, ROWS};

/// Convert with a function writing into a vector.
fn text(f: impl FnOnce(&mut Out)) -> Vec<u8> {
    let mut b = vec![0u8; 512];
    let mut o = Out::new(&mut b);
    f(&mut o);
    assert!(!o.overflow);
    let n = o.len();
    b.truncate(n);
    b
}

#[test]
fn hamming_codes() {
    assert_eq!(hamming84(0x15), 0);
    for n in 0..16i32 {
        // exactly one code word per nibble: the byte whose single-bit neighbours all decode to the same nibble
        let words = (0..=255u8).filter(|&b| hamming84(b) == n && (0..8).all(|k| hamming84(b ^ (1 << k)) == n)).count();
        assert_eq!(words, 1);
    }
    assert_eq!(reverse8(0x01), 0x80);
    assert_eq!(hamming2418(&[0, 0, 0]), 0);
}

#[test]
fn charsets() {
    assert_eq!(text(|o| charset::ebu_latin(&[0x41, 0x91], o)), "A\u{e4}".as_bytes());
    assert_eq!(text(|o| charset::iso6937(&[0xC1, b'e', b'x'], o)), "\u{e8}x".as_bytes());
    assert_eq!(text(|o| charset::iso6937(&[0xC2, b'e'], o)), "\u{e9}".as_bytes());
    assert_eq!(text(|o| charset::ucs2be(&[0xD8, 0x3D, 0xDE, 0x00, 0x00, 0x41], o)), "\u{1F600}A".as_bytes());
    let mut b = [0u8; 64];
    let n = charset::dvb_text(&[0x15, b'H', b'i', 0x0A, b'x', 0xC2, 0x86], false, None, &mut b);
    assert_eq!(&b[..n], b"Hi x");
    let n = charset::dvb_text(b"\x15  Caf\xc3\xa9\t ", false, None, &mut b);
    assert_eq!(&b[..n], b"Caf\xc3\xa9");
}

#[test]
fn teletext_header_unit() {
    // hamming-coded address 0 (magazine 1 row 0), page 100, nothing else
    let enc = |n: u8| (0..=255u8).find(|&b| hamming84(b) == n as i32).unwrap();
    let mut b = [0u8; 42];
    b[0] = enc(1);
    b[1] = enc(0);
    b[2] = enc(0);
    b[3] = enc(0);
    for i in 4..10 {
        b[i] = enc(0);
    }
    let mut u = [0u8; 44];
    u[1] = 0xE4;
    for i in 0..42 {
        u[2 + i] = reverse8(b[i]);
    }
    let d = decode_unit(&u);
    assert_eq!((d.kind, d.mag, d.row, d.number, d.page_ok), (UNIT_HEADER, 1, 0, 100, 1));
    u[1] = 0;
    assert_eq!(decode_unit(&u).kind, UNIT_IGNORE);
}

#[test]
fn pes_walk_and_g0() {
    let mut pes = vec![0, 0, 1, 0xBD, 0, 0, 0x80, 0x00, 0x00, 0x10];
    pes.extend_from_slice(&[0x02, 44]);
    pes.extend_from_slice(&[0u8; 44]);
    pes.extend_from_slice(&[0x03, 4, 1, 2, 3, 4]);
    let mut o = [0u32; 4];
    assert_eq!(pes_units(&pes, &mut o), 1);
    assert_eq!(o[0], 12);
    assert_eq!(g0(0x23, 0), 0xA3);
    assert_eq!(g0(0x23, 1), 0x23);
    assert_eq!(g0(0x7F, 0), 0x25A0);
}

#[test]
fn render_colour_and_double_height() {
    let mut rows = [0x20u8; ROWS * COLS];
    rows[COLS] = 1; // red
    rows[COLS + 1] = b'A';
    rows[COLS + 2] = 0x0D; // double height
    rows[COLS + 3] = b'B';
    let mut g = [Cell::blank(); ROWS * COLS];
    render(&rows, 0b10, 0, &mut g);
    assert_eq!((g[COLS + 1].ch, g[COLS + 1].fg), (b'A' as u32, 1));
    assert_eq!((g[COLS + 3].dh, g[2 * COLS + 3].dh), (1, 2));
    let mut out = [0u8; 120];
    let n = row_text(&g, 1, &mut out);
    assert_eq!(&out[..4], b" A B");
    assert_eq!(n, 40);
}

#[test]
fn subtitle_pixels_and_colours() {
    assert_eq!(ycrcb(0, 128, 128, 0), 0);
    assert_eq!(ycrcb(235, 128, 128, 0), 0xFFFF_FFFF);
    assert_eq!(ycrcb(16, 128, 128, 255), 0x0000_0000);
    let (mut e2, mut e4, mut e8) = ([0u32; 4], [0u32; 16], [0u32; 256]);
    default_clut(&mut e2, &mut e4, &mut e8);
    assert_eq!((e2[1], e4[1], e8[0]), (0xFFFF_FFFF, 0xFF00_00FF, 0));
    // 2-bit string: pixel 1, pixel 2, run of 3 x pixel 3, end of string
    let f = [0x10, 0b0110_0010, 0b0011_0000, 0b0000_0000, 0xF0];
    let mut pix = [0u8; 16];
    decode_field(&f, 0, 0, 2, false, 4, 2, 2, &mut pix);
    assert_eq!(&pix[..4], &[1, 2, 3, 3]);
}
