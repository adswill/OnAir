// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Meshtastic packets after decryption: the 16 byte radio header and the `Data` protobuf (portnum first) with the port payloads Text, Position, NodeInfo,
//! Telemetry, Routing, Traceroute and NeighborInfo turned into one detail line plus at most one node update or text message. Sources: meshtastic/firmware
//! src/mesh/RadioInterface.h (PacketHeader, flag masks) and meshtastic/protobufs mesh.proto (Data, Position, User, Routing, RouteDiscovery, NeighborInfo),
//! portnums.proto, telemetry.proto, config.proto. The AES-CTR decryption and the channel table stay in C++ (they need keys).
use crate::abi::{Message, Node, S2048, S64};
use crate::ffi::cfn::format_f;
use crate::names::{self, HW_MODELS, PORT_NUMS, ROLES, ROUTING_ERRORS};
use crate::pb::{self, Field, Reader};
use onair_common::text::Buf;

/// The 16 byte header in front of the payload (RadioInterface.h PacketHeader): destination, sender, packet id (little endian), flags, channel hash, next
/// hop, relay node.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Header {
    pub to: u32,
    pub from: u32,
    pub id: u32,
    /// Bits 0..2 hop limit, 3 want-ack, 4 via MQTT, 5..7 hop start.
    pub flags: u8,
    pub channel: u8,
    pub next_hop: u8,
    pub relay: u8,
}

/// Reads the header; `None` when fewer than 16 bytes.
pub fn parse_header(p: &[u8]) -> Option<Header> {
    if p.len() < 16 {
        return None;
    }
    let le = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    Some(Header { to: le(0), from: le(4), id: le(8), flags: p[12], channel: p[13], next_hop: p[14], relay: p[15] })
}

/// The Data protobuf of a packet (mesh.proto Data), payload is a slice of the decrypted bytes.
#[derive(Default)]
pub struct Data<'a> {
    pub portnum: i32,
    pub payload: &'a [u8],
    pub want_response: bool,
    pub dest: u32,
    pub source: u32,
    pub request_id: u32,
    pub reply_id: u32,
}

/// The firmware accepts any Data that parses and has a portnum; to avoid taking random bytes from a wrong key for a message the portnum must also come
/// first (nanopb writes fields in order and the portnum is never zero for real traffic, at most 511 here). Fields above 10 are not part of Data.
pub fn parse_data(p: &[u8]) -> Option<Data<'_>> {
    let mut r = Reader::new(p);
    let mut d = Data::default();
    let mut first = true;
    while let Some(f) = r.next() {
        if f.num > 10 {
            return None;
        }
        if first {
            if f.num != 1 || f.wire != 0 || f.value == 0 || f.value > 511 {
                return None;
            }
            first = false;
        }
        match f.num {
            1 if f.wire == 0 => d.portnum = f.value as i32,
            2 if f.wire == 2 => d.payload = f.data,
            3 => d.want_response = f.value != 0,
            4 => d.dest = f.value as u32,
            5 => d.source = f.value as u32,
            6 => d.request_id = f.value as u32,
            7 => d.reply_id = f.value as u32,
            _ => {}
        }
    }
    if r.ok() && !first {
        Some(d)
    } else {
        None
    }
}

/// `!%08x` of a node number.
pub fn node_id(w: &mut Buf, n: u32) {
    w.byte(b'!');
    w.hex(n as u64, 8, false);
}

/// `^all` for the broadcast address, else the node id text.
fn node_str(w: &mut Buf, n: u32) {
    if n == 0xFFFF_FFFF {
        w.str("^all");
    } else {
        node_id(w, n);
    }
}

/// Hardware model name, or `HW_<n>`.
pub fn hw_model_name(w: &mut Buf, hw: i32) {
    match names::name(&HW_MODELS, hw) {
        Some(n) => w.str(n),
        None => {
            w.str("HW_");
            w.idec(hw as i64);
        }
    }
}

/// Port name, or `PORT_<n>`.
pub fn port_name(w: &mut Buf, port: i32) {
    match names::name(&PORT_NUMS, port) {
        Some(n) => w.str(n),
        None => {
            w.str("PORT_");
            w.idec(port as i64);
        }
    }
}

/// Text with control characters (except newline) replaced by blanks; other bytes, UTF-8 included, are kept.
fn clean_text(w: &mut Buf, p: &[u8]) {
    for &c in p {
        w.byte(if c < 0x20 && c != b'\n' { b' ' } else { c });
    }
}

/// `s` if at most `n` bytes, else its first `n` bytes and "...", cut before a UTF-8 character that would not fit whole.
fn clip(w: &mut Buf, s: &[u8], n: usize) {
    if s.len() <= n {
        w.bytes(s);
    } else {
        let mut k = n;
        while k > 0 && s[k] & 0xC0 == 0x80 {
            k -= 1;
        }
        w.bytes(&s[..k]);
        w.str("...");
    }
}

/// What the packet is known for after decoding a port: the detail line and a node update or a text message.
#[derive(Default)]
pub struct Decoded {
    pub detail: S2048,
    pub node: Option<Node>,
    pub message: Option<Message>,
}

/// Context of the packet around the Data: sender, destination and the hop / id fields of the header.
pub struct Ctx {
    pub from: u32,
    pub to: u32,
    pub hop_start: i32,
    pub hop_limit: i32,
    pub packet_id: u32,
}

/// Decodes the payload of the port in `d` (the port payload formats of mesh.proto / telemetry.proto) into a detail line and the records.
pub fn decode_port(d: &Data, c: &Ctx) -> Decoded {
    let mut out = Decoded::default();
    let mut from_id = S64::default();
    from_id.write(|w| node_id(w, c.from));
    match d.portnum {
        1 => {
            let mut m = Message::default();
            m.from.set(from_id.as_bytes());
            m.to.write(|w| node_str(w, c.to));
            m.text.write(|w| clean_text(w, d.payload));
            m.hops = if c.hop_start > 0 { c.hop_start - c.hop_limit } else { -1 };
            m.packet_id = c.packet_id;
            m.reply_id = d.reply_id;
            out.detail.write(|w| {
                clip(w, m.text.as_bytes(), 80);
                if d.reply_id != 0 {
                    w.str(" (reply)");
                }
            });
            out.message = Some(m);
        }
        3 => position(d, &from_id, &mut out),
        8 => waypoint(d, &mut out),
        10 | 11 | 32 | 66 => {
            // plain text ports: detection sensor, alert, reply, range test
            let label: &str = match d.portnum {
                10 => "sensor: ",
                11 => "ALERT: ",
                32 => "reply: ",
                _ => "range test: ",
            };
            out.detail.write(|w| {
                w.str(label);
                clip(w, d.payload, 120);
            });
        }
        34 => paxcount(d, &mut out),
        65 => store_forward(d, &mut out),
        73 => map_report(d, &mut out),
        4 => nodeinfo(d, &from_id, &mut out),
        67 => telemetry(d, &mut out),
        5 => {
            let mut r = Reader::new(d.payload);
            let mut err = 0i32;
            let mut has_route = false;
            while let Some(f) = r.next() {
                if f.num == 3 && f.wire == 0 {
                    err = f.value as i32;
                } else if (f.num == 1 || f.num == 2) && f.wire == 2 {
                    has_route = true;
                }
            }
            out.detail.write(|w| {
                if has_route {
                    w.str("route discovery");
                } else if err == 0 {
                    w.str("ack of ");
                    w.hex(d.request_id as u64, 8, false);
                } else {
                    w.str("error ");
                    w.str(names::name(&ROUTING_ERRORS, err).unwrap_or("?"));
                    w.str(" for ");
                    w.hex(d.request_id as u64, 8, false);
                }
            });
        }
        70 => traceroute(d, c, &from_id, &mut out),
        71 => neighbors(d, &mut out),
        _ => out.detail.write(|w| {
            w.udec(d.payload.len() as u64);
            w.str(" bytes");
        }),
    }
    out
}

/// POSITION_APP (mesh.proto Position): a fix only when latitude and longitude are present and not both zero.
fn position(d: &Data, from_id: &S64, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let (mut has_lat, mut has_lon, mut has_alt, mut has_hae) = (false, false, false, false);
    let (mut lat, mut lon, mut alt, mut alt_hae) = (0i32, 0i32, 0i32, 0i32);
    let (mut t, mut ts, mut prec) = (0u32, 0u32, 0u32);
    let mut sats = -1i32;
    let (mut gs, mut track, mut hdop) = (None::<u64>, None::<u64>, None::<u64>);
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (1, 5) => {
                lat = f.as_i32();
                has_lat = true;
            }
            (2, 5) => {
                lon = f.as_i32();
                has_lon = true;
            }
            (3, 0) => {
                alt = f.as_i32();
                has_alt = true;
            }
            (4, 5) => t = f.value as u32,
            (7, 5) => ts = f.value as u32,
            (9, 0) => {
                alt_hae = f.as_sint32();
                has_hae = true;
            }
            (15, 0) => gs = Some(f.value),
            (16, 0) => track = Some(f.value),
            (12, 0) => hdop = Some(f.value),
            (19, 0) => sats = f.value as i32,
            (23, 0) => prec = f.value as u32,
            _ => {}
        }
    }
    if has_lat && has_lon && (lat != 0 || lon != 0) {
        let mut u = Node::default();
        u.has_position = 1;
        u.lat = lat as f64 * 1e-7;
        u.lon = lon as f64 * 1e-7;
        u.alt_m = if has_alt {
            alt as f64
        } else if has_hae {
            alt_hae as f64
        } else {
            0.0
        };
        u.position_time = if t != 0 { t } else { ts };
        u.sats = sats;
        let _ = from_id;
        out.detail.write(|w| {
            w.fixed(format_f, 5, u.lat);
            w.str(", ");
            w.fixed(format_f, 5, u.lon);
            w.str(", ");
            w.idec(u.alt_m as i32 as i64);
            w.str(" m");
            if let Some(v) = gs {
                w.str(", ");
                w.udec(v);
                w.str(" m/s");
            }
            if let Some(v) = track {
                w.str(", track ");
                w.fixed(format_f, 1, v as f64 * 1e-5);   // mesh.proto: 1e-5 degrees
            }
            if let Some(v) = hdop {
                w.str(", HDOP ");
                w.fixed(format_f, 2, v as f64 / 100.0);
            }
            if prec != 0 && prec < 32 {
                w.str(", ");
                w.udec(prec as u64);
                w.str(" bit precision");
            }
        });
        out.node = Some(u);
    } else {
        out.detail.set(b"no position fix");
    }
}

/// NODEINFO_APP (mesh.proto User): names, hardware model, role and whether a 32 byte public key is attached.
fn nodeinfo(d: &Data, _from_id: &S64, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let mut u = Node::default();
    let (mut hw, mut role) = (0i32, 0i32);
    let mut has_key = false;
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (2, 2) => u.long_name.set(f.data),
            (3, 2) => u.short_name.set(f.data),
            (5, 0) => hw = f.value as i32,
            (7, 0) => role = f.value as i32,
            (8, 2) => has_key = f.data.len() == 32,
            _ => {}
        }
    }
    u.hw_model.write(|w| hw_model_name(w, hw));
    u.role.write(|w| match names::name(&ROLES, role) {
        Some(n) => w.str(n),
        None => {
            w.str("ROLE_");
            w.idec(role as i64);
        }
    });
    out.detail.write(|w| {
        w.bytes(u.long_name.as_bytes());
        w.str(" (");
        w.bytes(u.short_name.as_bytes());
        w.str("), ");
        w.bytes(u.hw_model.as_bytes());
        w.str(", ");
        w.bytes(u.role.as_bytes());
        if has_key {
            w.str(", has public key");
        }
    });
    out.node = Some(u);
}

/// Device metrics (telemetry.proto DeviceMetrics, `local` false) or local stats (LocalStats, `local` true) into the node update.
fn metrics(m: &Field, local: bool, u: &mut Node) {
    let mut r = Reader::new(m.data);
    while let Some(g) = r.next() {
        if local {
            match (g.num, g.wire) {
                (1, 0) => u.uptime_s = g.value as i64,
                (2, 5) => u.channel_util_pct = g.as_f32() as f64,
                (3, 5) => u.air_util_tx_pct = g.as_f32() as f64,
                _ => {}
            }
        } else {
            match (g.num, g.wire) {
                (1, 0) => u.battery_pct = if g.value > 100 { 101.0 } else { g.value as f64 }, // 101 = on external power, shown as "powered"
                (2, 5) => u.voltage = g.as_f32() as f64,
                (3, 5) => u.channel_util_pct = g.as_f32() as f64,
                (4, 5) => u.air_util_tx_pct = g.as_f32() as f64,
                (5, 0) => u.uptime_s = g.value as i64,
                _ => {}
            }
        }
    }
}

/// TELEMETRY_APP (telemetry.proto Telemetry): device metrics (2), environment metrics (3) or local stats (6); the last one present gives the detail line.
/// PAXCOUNTER_APP (paxcount.proto): people counted by Wi-Fi and BLE devices seen.
fn paxcount(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let (mut wifi, mut ble, mut up) = (0u64, 0u64, 0u64);
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (1, 0) => wifi = f.value,
            (2, 0) => ble = f.value,
            (3, 0) => up = f.value,
            _ => {}
        }
    }
    out.detail.write(|w| {
        w.str("paxcounter: wifi ");
        w.udec(wifi);
        w.str(", ble ");
        w.udec(ble);
        w.str(", uptime ");
        w.udec(up);
        w.str(" s");
    });
}

/// STORE_FORWARD_APP (storeforward.proto): the request/response code and, for a router, its heartbeat or history size.
fn store_forward(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let mut rr = 0u64;
    let (mut period, mut secondary, mut hist) = (None::<u64>, 0u64, None::<u64>);
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (1, 0) => rr = f.value,
            (3, 2) => {
                let mut m = Reader::new(f.data);
                while let Some(g) = m.next() {
                    if g.num == 1 && g.wire == 0 {
                        hist = Some(g.value);
                    }
                }
            }
            (4, 2) => {
                let mut m = Reader::new(f.data);
                while let Some(g) = m.next() {
                    match (g.num, g.wire) {
                        (1, 0) => period = Some(g.value),
                        (2, 0) => secondary = g.value,
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
    let name = match rr {
        1 => "router error",
        2 => "router heartbeat",
        3 => "router ping",
        4 => "router pong",
        5 => "router busy",
        6 => "router history",
        7 => "router stats",
        8 => "text direct",
        9 => "text broadcast",
        64 => "client error",
        65 => "client history request",
        66 => "client stats request",
        67 => "client ping",
        68 => "client pong",
        106 => "client abort",
        _ => "unset",
    };
    out.detail.write(|w| {
        w.str("store and forward: ");
        w.str(name);
        if let Some(p) = period {
            w.str(", every ");
            w.udec(p);
            w.str(" s");
            if secondary != 0 {
                w.str(", secondary");
            }
        }
        if let Some(h) = hist {
            w.str(", ");
            w.udec(h);
            w.str(" messages");
        }
    });
}

/// MAP_REPORT_APP (mqtt.proto `MapReport`): name, firmware, position, nodes online.
fn map_report(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let (mut name, mut fw): (&[u8], &[u8]) = (&[], &[]);
    let (mut lat, mut lon, mut nodes) = (None::<i32>, None::<i32>, None::<u64>);
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (1, 2) => name = f.data,
            (5, 2) => fw = f.data,
            (9, 5) => lat = Some(f.as_i32()),
            (10, 5) => lon = Some(f.as_i32()),
            (13, 0) => nodes = Some(f.value),
            _ => {}
        }
    }
    out.detail.write(|w| {
        w.str("map report: ");
        clip(w, name, 40);
        if !fw.is_empty() {
            w.str(", fw ");
            clip(w, fw, 24);
        }
        if let (Some(a), Some(b)) = (lat, lon) {
            w.str(", ");
            w.fixed(format_f, 5, f64::from(a) * 1e-7);
            w.str(" ");
            w.fixed(format_f, 5, f64::from(b) * 1e-7);
        }
        if let Some(n) = nodes {
            w.str(", ");
            w.udec(n);
            w.str(" nodes");
        }
    });
}

/// WAYPOINT_APP (mesh.proto `Waypoint`): a named map pin.
fn waypoint(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let (mut lat, mut lon, mut id) = (None::<i32>, None::<i32>, 0u64);
    let mut name: &[u8] = &[];
    let mut desc: &[u8] = &[];
    while let Some(f) = r.next() {
        match (f.num, f.wire) {
            (1, 0) => id = f.value,
            (2, 5) => lat = Some(f.as_i32()),
            (3, 5) => lon = Some(f.as_i32()),
            (6, 2) => name = f.data,
            (7, 2) => desc = f.data,
            _ => {}
        }
    }
    out.detail.write(|w| {
        w.str("waypoint ");
        w.udec(id);
        if !name.is_empty() {
            w.str(" ");
            clean_text(w, name);
        }
        if !desc.is_empty() {
            w.str(": ");
            clean_text(w, desc);
        }
        if let (Some(a), Some(b)) = (lat, lon) {
            w.str(" at ");
            w.fixed(format_f, 5, f64::from(a) * 1e-7);
            w.str(" ");
            w.fixed(format_f, 5, f64::from(b) * 1e-7);
        }
    });
}

fn telemetry(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let mut u = Node::default();
    let mut det = S2048::default();
    while let Some(f) = r.next() {
        if f.num == 2 && f.wire == 2 {
            metrics(&f, false, &mut u);
            let powered = u.battery_pct > 100.0; // the firmware reports 101 when external power is used
            det.write(|w| {
                if powered {
                    w.str("powered, ");
                    w.fixed(format_f, 2, u.voltage);
                    w.str(" V");
                } else if u.battery_pct >= 0.0 {
                    w.str("battery ");
                    w.idec(u.battery_pct as i32 as i64);
                    w.str(" %, ");
                    w.fixed(format_f, 2, u.voltage);
                    w.str(" V");
                } else {
                    w.str("device metrics");
                }
                w.str(", ch util ");
                w.fixed(format_f, 1, u.channel_util_pct);
                w.str(" %, air tx ");
                w.fixed(format_f, 1, u.air_util_tx_pct);
                w.str(" %");
            });
        } else if f.num == 4 && f.wire == 2 {
            // air quality (telemetry.proto, AirQualityMetrics)
            let mut mr = Reader::new(f.data);
            let (mut pm25, mut pm10, mut co2) = (None, None, None);
            while let Some(g) = mr.next() {
                match (g.num, g.wire) {
                    (2, 0) => pm25 = Some(g.value),
                    (3, 0) => pm10 = Some(g.value),
                    (13, 0) => co2 = Some(g.value),
                    _ => {}
                }
            }
            det.write(|w| {
                w.str("air quality");
                if let Some(v) = pm25 {
                    w.str(", PM2.5 ");
                    w.udec(v);
                }
                if let Some(v) = pm10 {
                    w.str(", PM10 ");
                    w.udec(v);
                }
                if let Some(v) = co2 {
                    w.str(", CO2 ");
                    w.udec(v);
                    w.str(" ppm");
                }
            });
        } else if f.num == 5 && f.wire == 2 {
            // power channels (PowerMetrics): voltage and current as fixed32 floats
            let mut mr = Reader::new(f.data);
            let mut v = [None::<f32>; 3];
            let mut i = [None::<f32>; 3];
            while let Some(g) = mr.next() {
                if g.wire == 5 && (1..=6).contains(&g.num) {
                    let idx = ((g.num - 1) / 2) as usize;
                    if (g.num - 1) % 2 == 0 {
                        v[idx] = Some(g.as_f32());
                    } else {
                        i[idx] = Some(g.as_f32());
                    }
                }
            }
            det.write(|w| {
                w.str("power");
                for k in 0..3 {
                    if let Some(vv) = v[k] {
                        w.str(", ch");
                        w.udec((k + 1) as u64);
                        w.str(" ");
                        w.fixed(format_f, 2, f64::from(vv));
                        w.str(" V");
                        if let Some(ii) = i[k] {
                            w.str(" ");
                            w.fixed(format_f, 1, f64::from(ii));
                            w.str(" mA");
                        }
                    }
                }
            });
        } else if f.num == 3 && f.wire == 2 {
            let mut mr = Reader::new(f.data);
            while let Some(g) = mr.next() {
                match (g.num, g.wire) {
                    (1, 5) => {
                        u.temp_c = g.as_f32() as f64;
                        u.has_env = 1;
                    }
                    (2, 5) => {
                        u.humidity = g.as_f32() as f64;
                        u.has_env = 1;
                    }
                    (3, 5) => {
                        u.pressure_hpa = g.as_f32() as f64;
                        u.has_env = 1;
                    }
                    _ => {}
                }
            }
            det.write(|w| {
                w.str("environment ");
                w.fixed(format_f, 1, u.temp_c);
                w.str(" C, ");
                w.fixed(format_f, 0, u.humidity);
                w.str(" %, ");
                w.fixed(format_f, 0, u.pressure_hpa);
                w.str(" hPa");
            });
        } else if f.num == 6 && f.wire == 2 {
            metrics(&f, true, &mut u);
            det.write(|w| {
                w.str("local stats, uptime ");
                w.idec(u.uptime_s);
                w.str(" s");
            });
        }
    }
    if det.len == 0 {
        out.detail.set(b"telemetry");
    } else {
        out.detail = det;
    }
    out.node = Some(u);
}

/// TRACEROUTE_APP (mesh.proto RouteDiscovery): request or reply, the route as node ids, the SNRs (quarter dB) and the number of hops back.
fn traceroute(d: &Data, c: &Ctx, from_id: &S64, out: &mut Decoded) {
    let req = d.want_response || d.request_id == 0;
    let mut back = 0usize;
    let mut n_snr = 0usize;
    let mut r = Reader::new(d.payload);
    while let Some(f) = r.next() {
        match f.num {
            2 => pb::each_varint(&f, |_| n_snr += 1),
            3 => pb::each_fixed32(&f, |_| back += 1),
            _ => {}
        }
    }
    out.detail.write(|w| {
        w.str(if req { "traceroute request: " } else { "traceroute reply: " });
        if req {
            w.bytes(from_id.as_bytes());
        } else {
            node_str(w, c.to);
        }
        let mut r = Reader::new(d.payload);
        while let Some(f) = r.next() {
            if f.num == 1 {
                pb::each_fixed32(&f, |n| {
                    w.str(" > ");
                    node_id(w, n);
                });
            }
        }
        w.str(" > ");
        if req {
            node_str(w, c.to);
        } else {
            w.bytes(from_id.as_bytes());
        }
        if n_snr > 0 {
            w.str(" (SNR");
            let mut r = Reader::new(d.payload);
            while let Some(f) = r.next() {
                if f.num == 2 {
                    pb::each_varint(&f, |v| {
                        w.byte(b' ');
                        w.fixed(format_f, 1, (v as u32 as i32) as f64 / 4.0);
                    });
                }
            }
            w.str(" dB)");
        }
        if back > 0 {
            w.str(", back ");
            w.udec(back as u64);
            w.str(" hops");
        }
    });
}

/// NEIGHBORINFO_APP (mesh.proto NeighborInfo): the number of neighbours and the first four with their SNR.
fn neighbors(d: &Data, out: &mut Decoded) {
    let mut r = Reader::new(d.payload);
    let mut count = 0usize;
    let mut list = S2048::default();
    while let Some(f) = r.next() {
        if f.num == 4 && f.wire == 2 {
            let mut nr = Reader::new(f.data);
            let mut id = 0u32;
            let mut snr = 0f32;
            while let Some(g) = nr.next() {
                if g.num == 1 {
                    id = g.value as u32;
                } else if g.num == 2 && g.wire == 5 {
                    snr = g.as_f32();
                }
            }
            if count < 4 {
                let first = list.len == 0;
                let prev = list;
                list.write(|w| {
                    w.bytes(prev.as_bytes());
                    if !first {
                        w.str(", ");
                    }
                    node_id(w, id);
                    w.byte(b' ');
                    w.fixed(format_f, 1, snr as f64);
                    w.str(" dB");
                });
            }
            count += 1;
        }
    }
    out.detail.write(|w| {
        w.udec(count as u64);
        w.str(" neighbors");
        if list.len != 0 {
            w.str(": ");
            w.bytes(list.as_bytes());
        }
    });
}
