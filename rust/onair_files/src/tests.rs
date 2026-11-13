// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests of onair_files with known vectors (the C++ differential test covers equality with the old code).
use crate::{container, csv, json, sigmf, sqlite, tle, wav};

#[test]
fn csv_records() {
    let t = b"a,\"b,c\",\"d\"\"e\"\n\n x ,,\r\n";
    let s = csv::measure(t);
    assert_eq!((s.records, s.fields), (2, 6));
    let mut data = vec![0u8; s.bytes];
    let mut ends = vec![0u32; s.fields];
    let mut counts = vec![0u32; s.records];
    assert!(csv::parse(t, &mut data, &mut ends, &mut counts));
    assert_eq!(counts, [3, 3]);
    assert_eq!(&data[..ends[0] as usize], b"a");
    assert_eq!(&data[ends[0] as usize..ends[1] as usize], b"b,c");
    assert_eq!(&data[ends[1] as usize..ends[2] as usize], b"d\"e");
    assert_eq!(&data[ends[2] as usize..ends[3] as usize], b" x ");
    let mut out = [0u8; 32];
    let n = csv::quote(b"say \"hi\"", &mut out);
    assert_eq!(&out[..n], b"\"say \"\"hi\"\"\"");
    assert_eq!(csv::quote_len(b"plain"), 5);
    let mut e = [0u32; 8];
    assert_eq!(csv::split_list(b" A | B||C ", b'|', &mut e), 3);
    assert_eq!((e[0], e[1]), (1, 2));
}

const META: &[u8] =
    br#"{"global": {"core:datatype": "cf32_le", "core:sample_rate": 2.4e6, "core:description": "a \"quoted\" A", "core:header_bytes": 16},
 "captures": [{"core:sample_start": 100, "core:frequency": 100.5e6}], "annotations": [{"core:freq_lower_edge": 1}, {}]}"#;

#[test]
fn sigmf_reads_meta() {
    let m = sigmf::parse(META);
    assert_eq!(m.status, 0);
    assert_eq!((m.format, m.big_endian, m.annotations), (2, 0, 2));
    assert_eq!((m.sample_rate, m.center_hz), (2.4e6, 100.5e6));
    assert_eq!((m.sample_start, m.header_bytes), (100, 16));
    let raw = &META[m.description.off as usize..(m.description.off + m.description.len) as usize];
    let mut out = [0u8; 64];
    let n = json::unescape(raw, &mut out);
    assert_eq!(&out[..n], b"a \"quoted\" A");
    assert_eq!(sigmf::parse(b"[1]").status, 1);
    assert_eq!(sigmf::parse(b"{\"global\":{\"core:datatype\":\"cf64_le\"}}").status, 4);
    assert_eq!(sigmf::parse(b"{\"global\":{}}").status, 3);
    assert_eq!(sigmf::parse(b"{}").status, 2);
    // depth, duplicates (the last wins), trailing text
    assert_eq!(sigmf::parse(b"{\"global\":{\"core:datatype\":\"ci16_be\",\"core:datatype\":\"cu8\"}} trailing").format, 1);
}

#[test]
fn sigmf_writers() {
    let mut buf = [0u8; 600];
    let n = sigmf::write_tool_meta(b"cf32_le", 2.4e6, 100e6, 5, b"x\"y\n", &mut buf);
    let s = core::str::from_utf8(&buf[..n]).unwrap();
    assert!(
        s.contains("\"core:sample_rate\": 2400000,")
            && s.contains("\"core:description\": \"x\\\"y\"")
            && s.contains("\"core:frequency\": 100000000}"),
        "{s}"
    );
    let n2 = sigmf::write_recorder_meta(b"ci8_le", 10e6, 554e6, b"2026-10-13T09:15:30Z", b"a.cs8", 1000, b"", &mut buf);
    let s2 = core::str::from_utf8(&buf[..n2]).unwrap();
    assert!(
        s2.contains("\"core:sample_rate\": 10000000,") && s2.contains("\"core:frequency\": 554000000.0") && !s2.contains("description"),
        "{s2}"
    );
    // too short a buffer: the length is still reported
    assert_eq!(
        sigmf::write_recorder_meta(b"ci8_le", 10e6, 554e6, b"", b"", 0, b"", &mut []),
        sigmf::write_recorder_meta(b"ci8_le", 10e6, 554e6, b"", b"", 0, b"", &mut buf)
    );
}

#[test]
fn wav_pieces() {
    let mut fmt = [0u8; 16];
    fmt[0] = 1;
    fmt[2] = 2;
    fmt[4..8].copy_from_slice(&2_400_000u32.to_le_bytes());
    fmt[14] = 16;
    let f = wav::parse_fmt(&fmt, 16);
    assert_eq!((f.tag, f.channels, f.rate, f.bits), (1, 2, 2_400_000, 16));
    assert_eq!(wav::classify(&f), (0, 3));
    assert_eq!(wav::data_end(44, 0, 1000), 0);
    assert_eq!(wav::data_end(44, 100, 1000), 144);
    assert_eq!(wav::data_end(44, 100000, 1000), 0);
    assert_eq!(wav::center_from_name(b"sdr_100.000mhz_2msps.wav"), 100e6);
    assert_eq!(wav::center_from_name(b"nothing.wav"), 0.0);
}

const L1: &[u8] = b"1 25544U 98067A   08264.51782528 -.00002182  00000-0 -11606-4 0  2927";
const L2: &[u8] = b"2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.72125391563537";

#[test]
fn tle_iss() {
    let t = tle::parse(L1, L2, b" 0 ISS (ZARYA) ", 0);
    assert_eq!(t.status, 0);
    assert_eq!(t.norad, 25544);
    assert!((t.mean_motion - 15.72125391).abs() < 1e-9 && (t.ecc - 0.0006703).abs() < 1e-12 && (t.bstar - (-0.11606e-4)).abs() < 1e-12);
    assert_eq!(&b" 0 ISS (ZARYA) "[t.name.off as usize..(t.name.off + t.name.len) as usize], b"ISS (ZARYA)");
    // 2008 day 264.51782528
    assert!((t.epoch_jd - 2454730.01782528).abs() < 1e-6, "{}", t.epoch_jd);
    assert_eq!(tle::parse(L1, &L2[..40], b"", 0).status, 1);
    let mut text = b"ISS\r\n".to_vec();
    text.extend_from_slice(L1);
    text.extend_from_slice(b"\r\n");
    text.extend_from_slice(L2);
    text.extend_from_slice(b"\n\nnoise\n");
    let mut out = [tle::Tle::default(); 4];
    assert_eq!(tle::scan_text(&text, &mut out), 1);
    assert_eq!(out[0].norad, 25544);
}

#[test]
fn sqlite_pieces() {
    assert_eq!(sqlite::varint(&[0x81, 0x00]), Some((128, 2)));
    assert_eq!(sqlite::varint(&[0x81]), None);
    assert_eq!(sqlite::varint(&[0xFF; 9]), Some((u64::MAX, 9)));
    // record: header size 4, types 1 (1 byte int), 0x13 (text 3), 0 null; data 7, "abc"
    let rec = [4u8, 1, 19, 0, 7, b'a', b'b', b'c'];
    let mut cols = [sqlite::Col::default(); 4];
    assert_eq!(sqlite::record(&rec, &mut cols), Some(3));
    assert_eq!((cols[0].kind, cols[0].int, cols[1].kind, cols[1].data.off, cols[1].data.len, cols[2].kind), (1, 7, 3, 5, 3, 0));
    assert_eq!(sqlite::record(&[9u8, 1], &mut cols), None);
    assert_eq!(sqlite::payload_local(100, 4096, false), Some((100, false)));
    assert_eq!(sqlite::payload_local(1 << 29, 4096, false), None);
    assert_eq!(sqlite::tile_row(2, 1, 0), Some(3));
    assert_eq!(sqlite::tile_row(31, 0, 0), None);
    let mut scratch = [0u8; 200];
    let sql = b"CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)";
    assert!(sqlite::tiles_schema_ok(sql, &mut scratch).is_some());
    assert!(sqlite::tiles_schema_ok(b"CREATE TABLE tiles (a, b)", &mut scratch).is_none());
}

#[test]
fn container_header() {
    let h = container::signature_hash(b"{i4s1}");
    let mut b = [0u8; 20];
    assert!(container::write_header(h, &mut b));
    assert_eq!(container::check_header(&b, h), Some(16));
    assert_eq!(container::check_header(&b, h ^ 1), None);
    assert_eq!(container::fnv1a(b""), 1469598103934665603);
}
