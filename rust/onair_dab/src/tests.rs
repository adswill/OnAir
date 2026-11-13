// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests that need no C++ reference: the CRC check value, FIB walk, FIG 0/0, 0/2 and 1, MOT time and group, X-PAD split, dynamic label step.
use crate::crc::crc16;
use crate::ffi_types::*;
use crate::mot::{parse_group, parse_time, time_text, MotTime, GROUP_SEGMENT};
use crate::pad::{dls_step, pad_split, PadSub};
use crate::{fig, jl};

#[test]
fn crc_check_value() {
    // CRC-16/GENIBUS
    assert_eq!(crc16(b"123456789"), 0xD64E);
}

#[test]
fn fib_and_fig0() {
    let mut fib = [0xFFu8; 30];
    fib[..4].copy_from_slice(&[0x03, 0x00, 0xC0, 0x01]); // FIG 0/0 with ensemble id 0xC001
    fib[4] = 0x20 | 3;
    fib[5..8].copy_from_slice(&[0x01, 0x01, 0x01]); // FIG 1 header, junk
    let mut figs = [FigRef::default(); 4];
    let (n, used) = fig::fib_walk(&fib, &mut figs);
    assert_eq!((n, used, figs[0].typ, figs[0].off, figs[0].len), (2, 8, 0, 1, 3));
    assert_eq!(fig::fig0_eid(&fib[1..4]), Some(0xC001));
    assert_eq!(fig::fig0_eid(&[0x00, 0xC0]), None);
}

#[test]
fn fig0_services_and_label() {
    // FIG 0/2: one audio service 0xC221 with one stream audio component, sub-channel 5, DAB+ (ASCTy 63), primary
    let d = [0x02, 0xC2, 0x21, 0x01, 0x3F, 0x14 | 2];
    let (mut s, mut c) = ([Fig02Service::default(); 2], [Fig02Comp::default(); 4]);
    let (ns, nc) = fig::fig0_services(&d, &mut s, &mut c);
    assert_eq!((ns, nc, s[0].sid, s[0].ncomp), (1, 1, 0xC221, 1));
    assert_eq!((c[0].tmid, c[0].ascty, c[0].sub_id, c[0].primary, c[0].scid), (0, 63, 5, 1, -1));
    // FIG 1/1: service label
    let mut l = vec![0x01u8, 0xC2, 0x21];
    l.extend_from_slice(b"RADIO ONE       ");
    l.extend_from_slice(&[0, 0]);
    let r = fig::fig1(&l).unwrap();
    assert_eq!((r.ext, r.sid, r.off), (1, 0xC221, 3));
    assert_eq!(&l[3..11], b"RADIO ON");
}

#[test]
fn mot_time_and_group() {
    let mut t = MotTime::default();
    // MJD 60000 = 2023-02-25, 12:34 short form
    let v: u32 = 1 << 31 | 60000 << 14 | 12 << 6 | 34;
    assert_eq!(parse_time(&v.to_be_bytes(), &mut t), 4);
    let mut b = [0u8; 64];
    let n = time_text(&t, &mut b);
    assert_eq!(&b[..n], b"2023-02-25 12:34");
    // header group: type 3, CRC flag, segmentation, user access with transport id
    let mut g = vec![0x40 | 0x20 | 0x10 | 3, 0x00, 0x80, 0x00, 0x12, 0x12, 0x34, 0x00, 0x03, 0xAA, 0xBB, 0xCC];
    let c = crc16(&g);
    g.extend_from_slice(&c.to_be_bytes());
    let r = parse_group(&g);
    assert_eq!((r.status, r.typ, r.tid, r.seg_no, r.last, r.len), (GROUP_SEGMENT, 3, 0x1234, 0, 1, 3));
    assert_eq!(&g[r.off as usize..(r.off + r.len) as usize], &[0xAA, 0xBB, 0xCC]);
}

#[test]
fn xpad_and_label_step() {
    // short X-PAD (4 bytes) with a content indicator of application type 2, then 3 data bytes
    let mut lt = 0;
    let mut out = [PadSub::default(); 5];
    let n = pad_split(&[0x02, 1, 2, 3], [0x10, 0x02], &mut lt, &mut out);
    assert_eq!((n, out[0].typ, out[0].off, out[0].len, lt), (1, 2, 1, 3, 2));
    // a label segment of 3 characters with its CRC
    let mut s = vec![0xC2u8, 0x00, b'a', b'b', b'c'];
    let c = crc16(&s);
    s.extend_from_slice(&c.to_be_bytes());
    let st = dls_step(&s);
    assert_eq!((st.action, st.seg_len), (2, 5));
    assert_eq!(dls_step(&s[..4]).action, 0);
}

#[test]
fn journaline_group() {
    let mut g = vec![0x40u8, 0x00, 1, 2, 3];
    let c = crc16(&g);
    g.extend_from_slice(&c.to_be_bytes());
    let r = jl::data_group(&g);
    assert_eq!((r.status, r.typ, r.field_len), (0, 0, 3));
    g[3] ^= 1;
    assert_eq!(jl::data_group(&g).status, 2);
}
