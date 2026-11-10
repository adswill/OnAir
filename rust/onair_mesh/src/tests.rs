//! Unit tests of the mesh layers that do not need the C++ reference.
use crate::abi::S64;
use crate::{aprs, lorawan, meshcore, meshtastic, pb, tables};

#[test]
fn pb_fields() {
    // field 1 varint 150, field 2 string "ab", field 5 fixed32
    let b = [0x08, 0x96, 0x01, 0x12, 0x02, b'a', b'b', 0x2d, 1, 0, 0, 0];
    let mut r = pb::Reader::new(&b);
    let f = r.next().unwrap();
    assert_eq!((f.num, f.wire, f.value), (1, 0, 150));
    assert_eq!(r.next().unwrap().data, b"ab");
    assert_eq!(r.next().unwrap().value, 1);
    assert!(r.next().is_none() && r.ok());
    let mut bad = pb::Reader::new(&[0x0b]);
    assert!(bad.next().is_none() && !bad.ok());
}

#[test]
fn text_message() {
    // Data: portnum 1, payload "hi"
    let plain = [0x08, 0x01, 0x12, 0x02, b'h', b'i'];
    let d = meshtastic::parse_data(&plain).unwrap();
    let c = meshtastic::Ctx { from: 0x1234abcd, to: 0xFFFF_FFFF, hop_start: 3, hop_limit: 2, packet_id: 7 };
    let o = meshtastic::decode_port(&d, &c);
    let m = o.message.unwrap();
    assert_eq!((m.text.as_bytes(), m.to.as_bytes(), m.hops), (&b"hi"[..], &b"^all"[..], 1));
    assert_eq!(o.detail.as_bytes(), b"hi");
    assert!(meshtastic::parse_data(&[0x10, 0x01]).is_none());
}

#[test]
fn keys() {
    let mut k = [0u8; 32];
    assert_eq!(tables::expand_psk(&[1], &mut k), Some(16));
    assert_eq!(k[15], tables::DEFAULT_PSK[15]);
    assert_eq!(tables::expand_psk(&[0; 33], &mut k), None);
    assert_eq!(tables::channel_hash(b"LongFast", &tables::DEFAULT_PSK), 0x08);
    assert_eq!(tables::hw_model_value(b"TBEAM"), 4);
}

#[test]
fn lorawan_frames() {
    // unconfirmed up, DevAddr 0x26011BDA (TTN), FCtrl 0x00, FCnt 1, FPort 1, payload 2 bytes, MIC
    let f = [0x40, 0xDA, 0x1B, 0x01, 0x26, 0x00, 0x01, 0x00, 0x01, 0xAA, 0xBB, 1, 2, 3, 4];
    let l = lorawan::decode(&f);
    assert_eq!((l.ok, l.fport, l.frm_len, l.fcnt, l.mic), (1, 1, 2, 1, 0x01020304));
    assert_eq!(l.network.as_bytes(), b"The Things Network");
    assert_eq!(l.detail.as_bytes(), b"FPort 1, 2 bytes encrypted, MIC 01020304");
    assert_eq!(lorawan::decode(&[0x01, 0, 0]).ok, 0);
    let mut jr = [0u8; 23];
    jr[0] = 0x00;
    assert_eq!(lorawan::decode(&jr).ok, 1);
}

#[test]
fn lora_aprs_and_meshcom() {
    let a = aprs::lora_aprs(b"<\xff\x01LY1TST-7>APLRT1,WIDE1-1:!5441.23N/02516.78E>test\n");
    assert_eq!((a.ok, a.src.as_bytes(), a.dest.as_bytes(), a.path.as_bytes()), (1, &b"LY1TST-7"[..], &b"APLRT1"[..], &b"WIDE1-1"[..]));
    assert!(a.info.as_bytes().starts_with(b"!5441"));
    assert_eq!(aprs::lora_aprs(b"xx").ok, 0);
    // ack frame
    let ack = [0x41, 1, 0, 0, 0, 0x83, 9, 0, 0, 0, 1, 0];
    let m = aprs::meshcom(&ack);
    assert_eq!((m.ok, m.hop_limit, m.packet_id), (1, 3, 1));
    assert_eq!(m.detail.as_bytes(), b"ack of 00000009");
}

#[test]
fn meshcore_header_and_group() {
    // flood GRP_TXT, no path, 4 payload bytes
    let p = [(5 << 2) | 1, 0, 0xAA, 1, 2, 3];
    let h = meshcore::parse_header(&p).unwrap();
    assert_eq!((h.kind, h.m, h.type_name.as_bytes(), h.via.as_bytes()), (5, 4, &b"GRP_TXT"[..], &b", direct from sender"[..]));
    let o = meshcore::decode(&h, &[], &p[2..], false);
    assert_eq!(o.need_crypto, 1);
    assert!(meshcore::parse_header(&[1, 0]).is_err());
    let mut plain = vec![1, 0, 0, 0, 0];
    plain.extend_from_slice(b"Bob: hello");
    let g = meshcore::group_plain(5, 2, b", 2 hops", &plain);
    assert_eq!((g.has_message, g.message.from.as_bytes(), g.message.text.as_bytes()), (1, &b"Bob"[..], &b"hello"[..]));
    let _ = S64::default();
}

#[test]
fn text_detail_cut_on_a_character() {
    // 79 ASCII letters then "ção": the 80-byte cut falls inside the 2-byte "ç", which must not be split
    let mut text = vec![b'a'; 79];
    text.extend_from_slice("ção".as_bytes());
    let mut plain = vec![0x08, 0x01, 0x12, text.len() as u8];
    plain.extend_from_slice(&text);
    let d = meshtastic::parse_data(&plain).unwrap();
    let c = meshtastic::Ctx { from: 1, to: 0xFFFF_FFFF, hop_start: 3, hop_limit: 3, packet_id: 1 };
    let o = meshtastic::decode_port(&d, &c);
    let mut want = vec![b'a'; 79];
    want.extend_from_slice(b"...");
    assert_eq!(o.detail.as_bytes(), &want[..]);
    assert!(std::str::from_utf8(o.detail.as_bytes()).is_ok());
}

#[test]
fn text_detail_cut_on_an_emoji() {
    // 78 ASCII letters then a 4-byte emoji: the cut at 80 falls inside it, so the emoji goes whole
    let mut text = vec![b'a'; 78];
    text.extend_from_slice("📡 ok".as_bytes());
    let mut plain = vec![0x08, 0x01, 0x12, text.len() as u8];
    plain.extend_from_slice(&text);
    let d = meshtastic::parse_data(&plain).unwrap();
    let c = meshtastic::Ctx { from: 1, to: 0xFFFF_FFFF, hop_start: 3, hop_limit: 3, packet_id: 1 };
    let o = meshtastic::decode_port(&d, &c);
    let mut want = vec![b'a'; 78];
    want.extend_from_slice(b"...");
    assert_eq!(o.detail.as_bytes(), &want[..]);
}

#[test]
fn waypoint_and_telemetry_variants() {
    // Data: portnum 8, Waypoint: id 42, latitude_i 546789000 (sfixed32), longitude_i 252791000, name "Camp", description "Water"
    let mut wp = vec![0x08u8, 0x2A, 0x15];
    wp.extend_from_slice(&546789000i32.to_le_bytes());
    wp.push(0x1D);
    wp.extend_from_slice(&252791000i32.to_le_bytes());
    wp.extend_from_slice(&[0x32, 4, b'C', b'a', b'm', b'p', 0x3A, 5, b'W', b'a', b't', b'e', b'r']);
    let mut plain = vec![0x08, 0x08, 0x12, wp.len() as u8];
    plain.extend_from_slice(&wp);
    let d = meshtastic::parse_data(&plain).unwrap();
    let c = meshtastic::Ctx { from: 1, to: 2, hop_start: 3, hop_limit: 3, packet_id: 1 };
    let o = meshtastic::decode_port(&d, &c);
    assert_eq!(o.detail.as_bytes(), b"waypoint 42 Camp: Water at 54.67890 25.27910");
    // Telemetry: air quality (oneof 4): pm25 12, pm10 30, co2 900
    let air = [0x10u8, 12, 0x18, 30, 0x68, 0x84, 0x07];
    let mut tel = vec![0x22, air.len() as u8];
    tel.extend_from_slice(&air);
    let mut plain = vec![0x08, 67, 0x12, tel.len() as u8];
    plain.extend_from_slice(&tel);
    let o = meshtastic::decode_port(&meshtastic::parse_data(&plain).unwrap(), &c);
    assert_eq!(o.detail.as_bytes(), b"air quality, PM2.5 12, PM10 30, CO2 900 ppm");
    // power (oneof 5): ch1 3.3 V, 120 mA
    let mut p = vec![0x0D];
    p.extend_from_slice(&3.3f32.to_le_bytes());
    p.push(0x15);
    p.extend_from_slice(&120.0f32.to_le_bytes());
    let mut tel = vec![0x2A, p.len() as u8];
    tel.extend_from_slice(&p);
    let mut plain = vec![0x08, 67, 0x12, tel.len() as u8];
    plain.extend_from_slice(&tel);
    let o = meshtastic::decode_port(&meshtastic::parse_data(&plain).unwrap(), &c);
    assert_eq!(o.detail.as_bytes(), b"power, ch1 3.30 V 120.0 mA");
}

#[test]
fn more_ports() {
    let c = meshtastic::Ctx { from: 1, to: 2, hop_start: 3, hop_limit: 3, packet_id: 1 };
    let pay = |port: u8, p: &[u8]| {
        let mut v = vec![0x08, port, 0x12, p.len() as u8];
        v.extend_from_slice(p);
        let o = meshtastic::decode_port(&meshtastic::parse_data(&v).unwrap(), &c);
        String::from_utf8_lossy(o.detail.as_bytes()).into_owned()
    };
    assert_eq!(pay(66, b"seq 12"), "range test: seq 12");
    assert_eq!(pay(11, b"Fire"), "ALERT: Fire");
    assert_eq!(pay(34, &[0x08, 12, 0x10, 5, 0x18, 100]), "paxcounter: wifi 12, ble 5, uptime 100 s");
    // heartbeat every 300 s from a secondary router
    assert_eq!(pay(65, &[0x08, 2, 0x22, 5, 0x08, 0xAC, 0x02, 0x10, 1]), "store and forward: router heartbeat, every 300 s, secondary");
    let mut m = vec![0x0A, 3, b'N', b'o', b'd', 0x2A, 3, b'2', b'.', b'6', 0x4D];
    m.extend_from_slice(&546789000i32.to_le_bytes());
    m.push(0x55);
    m.extend_from_slice(&252791000i32.to_le_bytes());
    m.extend_from_slice(&[0x68, 7]);
    assert_eq!(pay(73, &m), "map report: Nod, fw 2.6, 54.67890 25.27910, 7 nodes");
    // position with speed, track, HDOP
    let mut p = vec![0x0D];
    p.extend_from_slice(&546789000i32.to_le_bytes());
    p.push(0x15);
    p.extend_from_slice(&252791000i32.to_le_bytes());
    p.extend_from_slice(&[0x18, 100, 0x60, 150, 1, 0x78, 7, 0x80, 1, 0xC0, 0x84, 0x3D]);
    assert_eq!(pay(3, &p), "54.67890, 25.27910, 100 m, 7 m/s, track 10.0, HDOP 1.50");
}
