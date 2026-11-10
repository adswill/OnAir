use crate::text::Buf;
use crate::{altitude, commb, cpr, crc, geo, msg};

fn hex(s: &str) -> [u8; 14] {
    let mut v = [0u8; 14];
    for (i, b) in v.iter_mut().enumerate().take(s.len() / 2) {
        *b = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
    }
    v
}

#[test]
fn crc_of_riddle_examples() {
    // "The 1090 Megahertz Riddle": good extended squitters leave a zero remainder
    for s in ["8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7", "8D40621D58C386435CC412692AD6"] {
        assert_eq!(crc::crc24(&hex(s), 112), 0, "{s}");
    }
    let mut bad = hex("8D4840D6202CC371C32CE0576098");
    bad[5] ^= 0x10;
    let rem = crc::crc24(&bad, 112);
    assert_ne!(rem, 0);
    assert_eq!(crc::single_bit_position(112, rem), 5 * 8 + 3);
}

#[test]
fn single_bit_syndromes_are_unique() {
    for n in [56, 112] {
        let mut seen = std::collections::BTreeSet::new();
        for p in 0..n {
            assert!(seen.insert(crc::single_bit_syndrome(n, p)), "bit {p} of {n}");
            assert_eq!(crc::single_bit_position(n, crc::single_bit_syndrome(n, p)), p);
        }
    }
}

#[test]
fn identification_message() {
    let (ok, m) = msg::decode(&hex("8D4840D6202CC371C32CE0576098"), 112);
    assert!(ok && m.df == 17 && m.icao == 0x4840D6 && m.tc == 4);
    assert_eq!(&m.callsign[..m.callsign_len as usize], b"KLM1023");
    assert_eq!((m.cat_set, m.cat_code), (b'A', 0));
}

#[test]
fn cpr_riddle_example() {
    let (_, e) = msg::decode(&hex("8D40621D58C382D690C8AC2863A7"), 112);
    let (_, o) = msg::decode(&hex("8D40621D58C386435CC412692AD6"), 112);
    let p = cpr::global_airborne(e.cpr_lat, e.cpr_lon, o.cpr_lat, o.cpr_lon, true).unwrap();
    assert!((p.lat - 52.26578).abs() < 1e-4 && (p.lon - 3.93891).abs() < 1e-4, "{} {}", p.lat, p.lon);
    assert_eq!(e.alt_ft, 38000);
}

#[test]
fn velocity_message() {
    let (_, m) = msg::decode(&hex("8D485020994409940838175B284F"), 112);
    assert!(m.has_vel && (m.speed_kt - 159.2).abs() < 0.1 && m.speed_kind == 0);
    assert!((m.heading_deg - 182.88).abs() < 0.01 && m.heading_is_track);
    assert!(m.has_vrate && m.vrate_fpm == -832);
}

#[test]
fn codes_round_trip() {
    for alt in (-1200..=126700).step_by(100) {
        let c = altitude::altitude_to_gillham(alt).unwrap();
        assert_eq!(altitude::gillham_to_altitude(c), Some(alt));
    }
    for sq in [0, 1200, 7700, 7500, 4567] {
        assert_eq!(altitude::decode_id13(altitude::encode_id13(sq)), sq);
    }
    assert_eq!(altitude::decode_ac13(0x0390), Some(0xE0 * 25 - 1000)); // Q = 1: 25 ft steps
}

#[test]
fn nl_thresholds_match_the_formula() {
    for n in 2..=59 {
        let t = (((1.0 - (core::f64::consts::PI / 30.0).cos()) / (1.0 - (2.0 * core::f64::consts::PI / n as f64).cos())).sqrt())
            .acos()
            .to_degrees();
        assert!((t - cpr::NL_THRESHOLDS[n - 2]).abs() < 1e-12, "n = {n}");
    }
    assert_eq!(cpr::nl(0.0), 59);
    assert_eq!(cpr::nl(87.5), 1);
    assert_eq!(cpr::nl(52.0), 36);
}

#[test]
fn encode_then_decode_a_position() {
    let (la, lo) = cpr::encode_airborne(52.2572, 3.91937, false);
    let (lb, lob) = cpr::encode_airborne(52.2572, 3.91937, true);
    let p = cpr::global_airborne(la, lo, lb, lob, true).unwrap();
    assert!((p.lat - 52.2572).abs() < 1e-3 && (p.lon - 3.91937).abs() < 1e-3);
}

#[test]
fn geometry_and_text() {
    assert!((geo::distance_nm(0.0, 0.0, 0.0, 1.0) - 60.04).abs() < 0.05);
    assert!((geo::bearing_deg(0.0, 0.0, 1.0, 0.0)).abs() < 1e-9);
    let mut out = [0u8; 100];
    let mut b = Buf::new(&mut out);
    commb::ra_text(0x2000 | 0x1000 | 0x0100, 0x8, false, &mut b);
    let n = b.len();
    assert_eq!(&out[..n], b"climb, corrective, crossing (no descend)");
    assert_eq!(altitude::category_name(4, 3), "Large (75000 - 300000 lb)\0");
    assert_eq!(altitude::category_name(1, 3), "Reserved\0");
}

#[test]
fn comm_b_identification() {
    // BDS 2,0: 0x20 then 8 six bit characters "EZY6AB  "
    let mut mb = [0u8; 7];
    mb[0] = 0x20;
    let chars = *b"EZY6AB  ";
    for (i, c) in chars.iter().enumerate() {
        crate::bits::set(&mut mb, 9 + 6 * i as i32, 6, crate::altitude::char_to_6bit(*c) as u32);
    }
    let r = commb::infer(&mb, commb::NO_ALTITUDE);
    assert_eq!(r.bds, 0x20);
    assert_eq!(&r.callsign[..r.callsign_len as usize], b"EZY6AB");
}

/// DF18 frame with control field `cf`, a valid CRC, and an ME that would read as airborne position (TC 11) under the ADS-B layout.
fn df18_with_position_me(cf: u8) -> Vec<u8> {
    let mut b = vec![0x90 | cf, 0x4B, 0x16, 0x9E, 0x58, 0xB9, 0x82, 0x56, 0x60, 0x5F, 0x3E, 0, 0, 0];
    let p = crc::crc24(&b, 112);
    b[11] = (p >> 16) as u8;
    b[12] = (p >> 8) as u8;
    b[13] = p as u8;
    assert_eq!(crc::crc24(&b, 112), 0);
    b
}

#[test]
fn coarse_tisb_is_not_read_as_adsb() {
    // CF 3: the ME is the coarse TIS-B layout, so nothing of it may become a position, altitude or velocity
    let (ok, m) = msg::decode(&df18_with_position_me(3), 112);
    assert!(ok);
    assert_eq!((m.df, m.ca, m.icao), (18, 3, 0x4B169E));
    assert_eq!(m.tc, -1);
    assert!(!m.has_cpr && !m.has_alt && !m.has_vel && !m.has_move && !m.has_track);
    // the same ME under CF 0 (ADS-B from a non-transponder device) and CF 2 (fine TIS-B) keeps the normal layout: the skip must stay this narrow
    for cf in [0u8, 2] {
        let (ok, m) = msg::decode(&df18_with_position_me(cf), 112);
        assert!(ok);
        assert_eq!(m.tc, 11, "CF {cf}");
        assert!(m.has_cpr && m.has_alt, "CF {cf}");
    }
}

#[test]
fn op_status_and_ra_broadcast() {
    // DF17 TC31 subtype 0, version 2: set ACAS (ME 11), 1090 In (ME 12), UAT In (ME 19), RA active (ME 27), ident (ME 28), ATC (ME 29)
    let mut b = [0u8; 14];
    b[0] = 0x8d;
    let set = |b: &mut [u8; 14], bit: usize, n: usize, v: u32| {
        for i in 0..n {
            let p = bit + i - 1;
            if (v >> (n - 1 - i)) & 1 != 0 {
                b[p / 8] |= 0x80 >> (p % 8);
            }
        }
    };
    set(&mut b, 33, 5, 31);
    set(&mut b, 38, 3, 0);
    set(&mut b, 43, 1, 1);
    set(&mut b, 44, 1, 1);
    set(&mut b, 51, 1, 1);
    set(&mut b, 59, 3, 0b111);
    set(&mut b, 73, 3, 2);
    let (ok, m) = msg::decode(&b, 112);
    assert!(ok);
    assert_eq!((m.cap_acas, m.cap_1090in, m.cap_uatin, m.ra_active, m.ident_on, m.atc_svc), (1, 1, 1, 1, 1, 1));
    // version 1: the ACAS bit is inverted
    let mut c = [0u8; 14];
    c[0] = 0x8d;
    set(&mut c, 33, 5, 31);
    set(&mut c, 73, 3, 1);
    set(&mut c, 43, 1, 1);
    let (_, m) = msg::decode(&c, 112);
    assert_eq!(m.cap_acas, 0);
    // surface, version 2
    let mut d = [0u8; 14];
    d[0] = 0x8d;
    set(&mut d, 33, 5, 31);
    set(&mut d, 38, 3, 1);
    set(&mut d, 73, 3, 2);
    set(&mut d, 49, 3, 5);
    set(&mut d, 53, 4, 11);
    let (_, m) = msg::decode(&d, 112);
    assert_eq!((m.nacv, m.length_code), (5, 11));
    // RA broadcast, threat identity
    let mut e = [0u8; 14];
    e[0] = 0x8d;
    set(&mut e, 33, 5, 28);
    set(&mut e, 38, 3, 2);
    set(&mut e, 41, 14, 0x1234);
    set(&mut e, 55, 4, 9);
    set(&mut e, 59, 1, 1);
    set(&mut e, 61, 2, 1);
    set(&mut e, 63, 24, 0x4840D6);
    let (_, m) = msg::decode(&e, 112);
    assert!(m.has_ra_bcast);
    assert_eq!((m.ra_ara, m.ra_rac, m.ra_term, m.ra_tti, m.ra_threat), (0x1234, 9, true, 1, 0x4840D6));
    // TC 19 NACv
    let mut f = [0u8; 14];
    f[0] = 0x8d;
    set(&mut f, 33, 5, 19);
    set(&mut f, 38, 3, 1);
    set(&mut f, 43, 3, 3);
    let (_, m) = msg::decode(&f, 112);
    assert_eq!(m.nacv, 3);
}
