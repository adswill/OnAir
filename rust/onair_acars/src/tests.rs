// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

use crate::block::{self, Stage};
use crate::text::Buf;
use crate::{crc, label, repair};

/// Builds a block the way a transmitter does: parity on every character, then the check bytes.
fn frame(mode: u8, reg: &[u8; 7], ack: u8, lab: [u8; 2], id: u8, text: &[u8], etx: u8) -> ([u8; 64], usize, [u8; 2]) {
    let mut v = [0u8; 64];
    let mut n = 0;
    let par = |c: u8| if c.count_ones() & 1 == 1 { c } else { c | 0x80 };
    let mut push = |b: u8| {
        v[n] = b;
        n += 1;
    };
    push(par(mode));
    for &c in reg {
        push(par(c));
    }
    push(par(ack));
    push(par(lab[0]));
    push(par(lab[1]));
    push(par(id));
    if !text.is_empty() {
        push(block::STX);
        for &c in text {
            push(par(c));
        }
    }
    push(etx);
    let c = crc::crc(&v[..n], 0);
    (v, n, [(c & 0xff) as u8, (c >> 8) as u8])
}

#[test]
fn crc_of_a_good_block_is_zero() {
    let (v, n, ck) = frame(b'2', b".N12345", 0x15, *b"H1", b'3', b"M01AEK0201hello", 0x83);
    let mut c = crc::crc(&v[..n], 0);
    c = crc::update(c, ck[0]);
    c = crc::update(c, ck[1]);
    assert_eq!(c, 0);
}

#[test]
fn repairs_one_flipped_bit() {
    let (mut v, n, ck) = frame(b'2', b".N12345", 0x15, *b"H1", b'3', b"M01AEK0201hello", 0x83);
    v[6] ^= 0x04;
    let r = repair::repair_block(&mut v[..n], ck, 3);
    assert!(r.ok && r.fixed_bits == 1 && r.parity_errors == 1);
}

#[test]
fn parses_a_downlink() {
    let (v, n, _) = frame(b'2', b".N12345", 0x15, *b"H1", b'3', b"M01AEK0201hello", 0x83);
    let mut plain = v;
    for b in &mut plain[..n] {
        *b &= 0x7f;
    }
    let mut dec = [0u8; block::DECODED_CAP];
    let (f, _) = block::parse(&plain[..n], &mut dec);
    assert!(f.stage == Stage::Full && f.downlink && f.final_block);
    assert_eq!(&f.reg[..f.reg_len], b"N12345");
    assert_eq!(f.ack, b'!');
    assert_eq!(&f.msg_num, b"M01");
    assert_eq!(&f.flight_id, b"EK0201");
    assert_eq!(&f.text[..f.text_len], b"hello");
}

#[test]
fn reads_oooi_and_media_advisory() {
    let mut out = [0u8; 200];
    let mut b = Buf::new(&mut out);
    label::decode_text(b"QA", b"OMDB1030", &mut b);
    let n = b.len();
    assert_eq!(&out[..n], b"from OMDB out 1030");
    let mut out = [0u8; 200];
    let mut b = Buf::new(&mut out);
    label::decode_text(b"SA", b"0EV123456SH/x", &mut b);
    let n = b.len();
    assert_eq!(&out[..n], b"link established: VHF ACARS at 12:34:56, available: default SATCOM, HF");
    assert_eq!(label::label_text(b'Q', b'1'), "OOOI report (out, off, on, in)\0");
}
