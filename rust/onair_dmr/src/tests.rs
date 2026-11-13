// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests for the DMR layer with the known vectors that do not need the C++ reference: slot type, sync patterns, trellis and BPTC round trips, text formats.
use crate::bits::Out;
use crate::burst;
use crate::codes::{self, Hamming};
use crate::data;
use crate::lc;
use crate::product;
use crate::text::{self, Buf};

#[test]
fn slot_type_round_trip() {
    for cc in 0..16 {
        for dt in 0..16 {
            let info = (cc << 4 | dt) as u32;
            let w = (info << 12) | codes::golay2008_parity(info);
            assert_eq!(codes::golay2008_decode(w ^ 0b101), Some((2, info)));
        }
    }
}

#[test]
fn sync_patterns_match_themselves() {
    for s in 0..burst::SYNC_COUNT {
        let mut bits = [0u8; 48];
        Out::new(&mut bits).put(burst::SYNC_WORDS[s], 48);
        assert_eq!(burst::match_sync(&bits, 0), (s as i32, 0));
    }
}

#[test]
fn bptc_trellis_round_trip() {
    let mut info = [0u8; 96];
    for (i, x) in info.iter_mut().enumerate() {
        *x = ((i * 5 + 1) % 3 == 0) as u8;
    }
    let mut tx = product::bptc196_encode(&info);
    tx[40] ^= 1;
    let mut out = [0u8; 96];
    assert_eq!(product::bptc196_decode(&tx, &mut out), 1);
    assert_eq!(out, info);
    let data: [u8; 18] = core::array::from_fn(|i| (i * 17 + 3) as u8);
    let tx = product::trellis34_encode(&data);
    let mut sym = [0f32; 98];
    for k in 0..98 {
        sym[k] = product::dibit_to_symbol(u32::from(tx[2 * k]) << 1 | u32::from(tx[2 * k + 1])) as f32;
    }
    let mut d2 = [0u8; 18];
    assert_eq!(product::trellis34_decode(&sym, &mut d2), 0);
    assert_eq!(d2, data);
}

#[test]
fn hamming_corrects_one_bit() {
    for k in [Hamming::H1393, Hamming::H15113, Hamming::H16114, Hamming::H17123] {
        let (ki, p) = (k.info_bits(), k.parity_bits());
        let info = 0x155u32 & ((1 << ki) - 1);
        let w = (info << p) | codes::hamming_parity(k, info);
        for b in 0..ki + p {
            let mut x = w ^ (1 << b);
            assert_eq!(codes::hamming_correct(k, &mut x), 1);
            assert_eq!(x, w);
        }
    }
}

#[test]
fn csbk_and_data_header_round_trip() {
    let c = lc::Csbk { lb: true, pf: false, opcode: 0x26, fid: 0, data: [1, 2, 3, 4, 5, 6, 7, 8] };
    let mut info = [0u8; 96];
    lc::csbk_info(&c, &mut info);
    assert_eq!(lc::csbk_parse(&info), Some(c));
    info[3] ^= 1;
    assert_eq!(lc::csbk_parse(&info), None);
    let h = data::DataHeader {
        dpf: data::DPF_CONFIRMED,
        sap: 4,
        group: true,
        dst: 1234,
        src: 4321,
        blocks: 5,
        pad: 3,
        fmf: true,
        fsn: 2,
        ns: 3,
        ..Default::default()
    };
    let (ok, r) = data::parse_data_header(&data::pack_data_header(&h));
    assert!(ok);
    assert_eq!(r, h);
}

#[test]
fn text_formats() {
    let mut buf = [0u8; 64];
    let mut b = Buf::new(&mut buf);
    text::text_from_bytes(b"Hello", text::DD_ISO8859_1, false, &mut b);
    assert_eq!(&buf[..5], b"Hello");
    let mut b = Buf::new(&mut buf);
    text::text_from_bytes(&[0x00, 0x48, 0x00, 0x69], 3, true, &mut b);
    let n = b.n;
    assert_eq!(&buf[..n], b"Hi");
}

#[test]
fn utf16_le_sent_under_a_be_label_is_read_as_le() {
    // #300: the import radios send little-endian text under the big-endian format code
    for &fmt in &[text::DD_UTF16_BE, text::DD_UTF16] {
        for (src, want) in [(&[b'H', 0, b'i', 0, b'!', 0][..], &b"Hi!"[..]), (&[0, b'H', 0, b'i'][..], &b"Hi"[..])] {
            let mut buf = [0u8; 64];
            let mut b = Buf::new(&mut buf);
            text::text_from_bytes(src, fmt, false, &mut b);
            let n = b.n;
            assert_eq!(&buf[..n], want);
        }
    }
}
