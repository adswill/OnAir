// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Known vectors for the network layers.
use crate::*;

#[test]
fn atoi_like_c() {
    assert_eq!(atoi(b"1234"), 1234);
    assert_eq!(atoi(b"  -12x"), -12);
    assert_eq!(atoi(b"+7"), 7);
    assert_eq!(atoi(b""), 0);
    assert_eq!(atoi(b"99999999999999999999"), -1);
    assert_eq!(atoi(b"4294967297"), 1);
    assert_eq!(atoi(b"12\x0034"), 12);
}

#[test]
fn hash_vectors() {
    let h = hash::sha1(b"abc");
    assert_eq!(h[..4], [0xa9, 0x99, 0x3e, 0x36]);
    let mut o = [0u8; 28];
    hash::ws_accept_key(b"dGhlIHNhbXBsZSBub25jZQ==", &mut o);
    assert_eq!(&o, b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    let mut b = [0u8; 16];
    assert_eq!(hash::base64(b"fooba", &mut b), 8);
    assert_eq!(&b[..8], b"Zm9vYmE=");
}

#[test]
fn ws_roundtrip() {
    let mut out = [0u8; 300];
    let n = ws::frame(1, b"hi", &mut out);
    assert_eq!(&out[..n], [0x81, 2, b'h', b'i']);
    // a masked client frame "hi"
    let inp = [0x81, 0x82, 1, 2, 3, 4, b'h' ^ 1, b'i' ^ 2];
    let mut pl = [0u8; 4096];
    assert_eq!(ws::parse_client(&inp, &mut pl), ws::Client::Frame { used: 8, op: 1, fin: true, len: 2 });
    assert_eq!(&pl[..2], b"hi");
    assert_eq!(ws::parse_client(&inp[..3], &mut pl), ws::Client::Need);
    assert_eq!(ws::parse_client(&[0x81, 0x02, 0, 0], &mut pl), ws::Client::Close(1002));
}

#[test]
fn http_head() {
    let mut f = [http::Field::default(); 48];
    let h = b"POST /a/b?x=1&y=2 HTTP/1.1\r\nHost: localhost:8765\r\nContent-Length: 12\r\nX-A:   v \r\n";
    let r = http::parse(h, &mut f);
    assert_eq!(r.err, 0);
    assert_eq!(r.content_length, 12);
    assert_eq!(r.n_fields, 3);
    let t = |s: Span| &h[s.off as usize..(s.off + s.len) as usize];
    assert_eq!(t(r.method), b"POST");
    assert_eq!(t(r.path), b"/a/b");
    assert_eq!(t(r.query), b"x=1&y=2");
    assert_eq!(t(f[2].value), b"v");
    assert_eq!(http::parse(b"POST / HTTP/1.1", &mut f).err, http::E_LENGTH_REQUIRED);
    assert_eq!(http::parse(b"GET / HTTP/1.1\r\nTransfer-Encoding: chunked", &mut f).err, http::E_CHUNKED);
    assert_eq!(http::parse(b"GET / HTTP/1.1\r\nContent-Length: 5000", &mut f).err, http::E_BODY_TOO_LARGE);
    assert_eq!(http::parse(b"GET x HTTP/1.1", &mut f).err, http::E_TARGET);
    assert_eq!(http::parse(b"GET /\r\n", &mut f).err, http::E_REQUEST_LINE);
    assert_eq!(http::parse(b"GET / HTTP/1.1\r\nHost: a\r\nhost: b", &mut f).err, http::E_DUPLICATE);
    assert_eq!(http::parse(b"GET / HTTP/1.1\r\n:x", &mut f).err, http::E_HEADER);
    assert_eq!(http::scan(b"GET / HTTP/1.1\r\n\r\n").0, http::SCAN_FOUND);
    assert_eq!(http::scan(b"GET / HT").0, http::SCAN_NEED);
}

#[test]
fn rtl_tcp() {
    let g = rtltcp::greeting(b"RTL0\0\0\0\x05\0\0\0\x1d").unwrap();
    assert_eq!((g.tuner, g.n_gains, g.other), (5, 29, 0));
    assert_eq!(rtltcp::greeting(b"RTL0\0\0\0\x09\0\0\0\x1d").unwrap().other, 1);
    assert!(rtltcp::greeting(b"RT\x01\x00\0\0\0\x05\0\0\0\x1d").is_none());
    assert_eq!(rtltcp::command(1, 0x0102_0304), [1, 1, 2, 3, 4]);
    assert_eq!(rtltcp::tuner_gains(5).len(), 29);
    let (h, p) = rtltcp::split_host_port(b"[::1]:1234").unwrap();
    assert_eq!((h, p), (Span { off: 1, len: 3 }, 1234));
    assert!(rtltcp::split_host_port(b":1234").is_none());
    assert!(rtltcp::split_host_port(b"h:70000").is_none());
}

#[test]
fn spyserver() {
    let mut o = [0u8; 32];
    let n = spy::hello(&mut o);
    assert_eq!(n, 8 + 9);
    assert_eq!(&o[..8], [0, 0, 0, 0, 9, 0, 0, 0]);
    assert_eq!(&o[8..12], (0x0200_06a4u32).to_le_bytes());
    let mut r = [0f64; 24];
    assert_eq!(spy::rates(10_000_000, 9, 0, &mut r), 9);
    assert_eq!(r[1], 5_000_000.0);
    let mut h = [0u8; 20];
    h[4..8].copy_from_slice(&((3u32 << 16) | 100).to_le_bytes());
    h[16..20].copy_from_slice(&64u32.to_le_bytes());
    assert_eq!(spy::header(&h), Some((100, 3, 64)));
}

#[test]
fn radio_list() {
    let s = b"rtltcp,host1,1234;pluto, 192.168.2.1 ,0;bogus,x,1;soapyremote,tcp://h,55132;rtltcp,host1,1234;hl2,x";
    let mut r = [radios::Radio::default(); 8];
    let mut a = [0u8; 200];
    let n = radios::parse(s, &mut r, &mut a);
    assert_eq!(n, 3);
    let mut out = [0u8; 200];
    let m = radios::format(&r[..n], &a, &mut out);
    assert_eq!(&out[..m], b"rtltcp,host1,1234;pluto,192.168.2.1,0;soapyremote,h,55132");
}

#[test]
fn sdrpp_ui() {
    // a draw list: step 0x8b (checkbox) "Bias T##x_biast_Src" true
    let mut p = std::vec::Vec::new();
    p.extend([0u8, 0x8b, 0]);
    let id = b"Bias T##x_biast_Src";
    p.push(4);
    p.extend((id.len() as u16).to_le_bytes());
    p.extend(id);
    p.extend([1u8, 1]);
    let mut w = [sdrpp::Widget::default(); 4];
    let mut it = [Span::default(); 4];
    let mut ar = [0u8; 256];
    let c = sdrpp::parse_ui(&p, &mut w, &mut it, &mut ar);
    assert_eq!((c.ok, c.widgets), (1, 1));
    assert_eq!(w[0].kind, sdrpp::CHECKBOX);
    assert_eq!(w[0].b, 1);
    assert_eq!(w[0].label, Span { off: w[0].id.off, len: 6 });
    let mut l = [0u8; 64];
    let n = sdrpp::label_from_id(b"##x_biast_Src", &mut l);
    assert_eq!(&l[..n], b"Bias tee");
    assert_eq!(sdrpp::rate_unit(b"MHz"), 1e6);
    assert_eq!(sdrpp::rate_unit(b" k s/s"), 1e3);
    assert_eq!(sdrpp::rate_unit(b"x"), 0.0);
    assert!(sdrpp::parse_ui(&[9], &mut w, &mut it, &mut ar).ok == 0);
}
