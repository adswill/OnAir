// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Unit tests of the UAT decoder that do not need the C reference: the DO-282 short frame vector and an uplink frame with one FIS-B frame of each time format.
use crate::downlink::{self, Math};
use crate::uplink;

fn math() -> Math {
    Math { atan2: f64::atan2, sqrt: f64::sqrt }
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
}

#[test]
fn downlink_vector() {
    let m = downlink::decode_adsb(&hex("007E6987D2D74238C1453855F89980C7524F"), &math());
    assert_eq!(m.address, 0x7E6987);
    assert_eq!((m.has_sv, m.position_valid, m.mdb_type), (1, 1, 0));
    assert!((m.lat - -31.75252676010132).abs() < 1e-9 && (m.lon - 39.9058198928833).abs() < 1e-9);
}

#[test]
fn short_input_reads_zero() {
    let m = downlink::decode_adsb(&[], &math());
    assert_eq!((m.mdb_type, m.address, m.position_valid), (0, 0, 0));
    let u = uplink::decode_uplink(&[]);
    assert_eq!((u.app_data_valid, u.num_info_frames), (0, 0));
}

#[test]
fn uplink_fisb_time_formats() {
    for t_opt in 0u8..4 {
        let mut f = vec![0u8; 432];
        f[6] = 0x20 | 5;
        // one frame: length 10, type 0, APDU header with product id 8 (AIRMET) and the time format under test
        f[8] = 10 >> 1;
        f[9] = 0; // (10 & 1) << 7
        f[10] = 0x80;
        f[11] = (8 << 2) | 0x02 | (t_opt >> 1);
        f[12] = (t_opt & 1) << 7;
        let u = uplink::decode_uplink(&f);
        assert_eq!((u.app_data_valid, u.slot_id, u.num_info_frames), (1, 5, 1));
        let i = u.info[0];
        assert_eq!((i.is_fisb, i.length), (1, 10));
        assert_eq!((i.fisb.product_id, i.fisb.a_flag, i.fisb.s_flag), (8, 1, 1));
        let skip = [4u16, 5, 5, 6][t_opt as usize];
        assert_eq!(i.fisb.length, 10 - skip);
        assert_eq!(i.fisb.data_off, 2 + skip);
        assert_eq!((i.fisb.monthday_valid, i.fisb.seconds_valid), (t_opt >> 1, t_opt & 1));
    }
}

#[test]
fn uplink_overrun_stops() {
    let mut f = vec![0u8; 432];
    f[6] = 0x20;
    f[8] = 255; // length 510 does not fit
    let u = uplink::decode_uplink(&f);
    assert_eq!((u.num_info_frames, u.tail_valid, u.tail_length), (0, 1, 510));
}
