// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! LoRaWAN PHY payload headers (no keys, nothing decrypted). Facts: LoRaWAN L2 1.0.4 (LoRa Alliance TS001-1.0.4) chapters 4 (MAC frame formats) and 5 (MAC
//! commands), LoRaWAN 1.1 (rejoin requests, the 1.1 MAC commands), LoRaWAN Backend Interfaces 1.0 (TS002) 6.1 (NetID and DevAddr types). Decodes MHDR, FHDR
//! (DevAddr, FCtrl, FCnt, FOpts of 1.0.x with the MAC command names), FPort, the length of the encrypted FRMPayload and the MIC; join requests (JoinEUI,
//! DevEUI, DevNonce), join accepts (length only) and rejoin requests. Payloads are never decrypted (they need the device's keys).
use crate::abi::{S128, S2048, S256, S64};
use crate::ffi::cfn::format_f;
use crate::lorawan_names::{eui_text, mac_def, mac_name, mtype_name, network, vendor, Network, MACS};
use onair_common::text::Buf;

fn le32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}
fn le64(p: &[u8]) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&p[..8]);
    u64::from_le_bytes(b)
}
fn be32(p: &[u8]) -> u32 {
    u32::from_be_bytes([p[0], p[1], p[2], p[3]])
}

/// The decoded LoRaWAN fields (MeshDecodeResult::Lorawan) and the packet fields the decoder sets.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Lorawan {
    /// 1 when the payload parsed (`ok`), 1 when the LoRaWAN record is meaningful (`has_lorawan`).
    pub ok: u8,
    pub has_lorawan: u8,
    /// MHDR MType (0 join request, 1 join accept, 2 / 3 unconfirmed up / down, 4 / 5 confirmed up / down, 6 rejoin request, 7 proprietary), -1 before parsing.
    pub mtype: i32,
    pub major: i32,
    pub uplink: u8,
    pub confirmed: u8,
    /// FCtrl flags.
    pub adr: u8,
    pub adr_ack_req: u8,
    pub ack: u8,
    pub fpending: u8,
    pub class_b: u8,
    /// DevAddr type (-1 none), FOptsLen, FPort (-1 none), DevStatusAns battery and margin.
    pub net_id_type: i32,
    pub fopts_len: i32,
    pub fport: i32,
    pub dev_status_battery: i32,
    pub dev_status_margin: i32,
    pub rejoin_type: i32,
    pub fcnt: u16,
    pub dev_nonce: u16,
    pub dev_addr: u32,
    pub nwk_id: u32,
    pub net_id: u32,
    pub mic: u32,
    pub rejoin_net_id: u32,
    pub frm_len: u32,
    pub join_eui: u64,
    pub dev_eui: u64,
    /// Network name, device maker, MAC detail line and the number / names of the MAC commands in FOpts.
    pub network: S64,
    pub vendor: S64,
    pub mac_detail: S256,
    pub mac_count: u32,
    pub mac_names: [S64; 16],
    /// Packet fields: type text, from, to, detail, note, packet id (the FCnt of data frames).
    pub type_name: S64,
    pub from: S64,
    pub to: S64,
    pub detail: S2048,
    pub note: S128,
    pub packet_id: u32,
}

impl Default for Lorawan {
    fn default() -> Self {
        Lorawan {
            ok: 0,
            has_lorawan: 0,
            mtype: -1,
            major: 0,
            uplink: 1,
            confirmed: 0,
            adr: 0,
            adr_ack_req: 0,
            ack: 0,
            fpending: 0,
            class_b: 0,
            net_id_type: -1,
            fopts_len: 0,
            fport: -1,
            dev_status_battery: -1,
            dev_status_margin: 0,
            rejoin_type: -1,
            fcnt: 0,
            dev_nonce: 0,
            dev_addr: 0,
            nwk_id: 0,
            net_id: 0,
            mic: 0,
            rejoin_net_id: 0,
            frm_len: 0,
            join_eui: 0,
            dev_eui: 0,
            network: S64::default(),
            vendor: S64::default(),
            mac_detail: S256::default(),
            mac_count: 0,
            mac_names: [S64::default(); 16],
            type_name: S64::default(),
            from: S64::default(),
            to: S64::default(),
            detail: S2048::default(),
            note: S128::default(),
            packet_id: 0,
        }
    }
}

impl Lorawan {
    fn push_mac(&mut self, f: impl FnOnce(&mut Buf)) {
        if (self.mac_count as usize) < self.mac_names.len() {
            self.mac_names[self.mac_count as usize].write(f);
            self.mac_count += 1;
        }
    }
    fn note_fmt(&mut self, f: impl FnOnce(&mut Buf)) {
        self.note.write(f);
    }
}

/// The FOpts MAC commands: their names, and the values of the ones that say something about the device or the link.
fn mac_commands(p: &[u8], uplink: bool, lw: &mut Lorawan) {
    let n = p.len();
    let mut i = 0usize;
    while i < n {
        let cid = p[i];
        if cid >= 0x80 {
            lw.push_mac(|w| mac_name(w, cid, uplink)); // proprietary: its length is unknown
            return;
        }
        let d = mac_def(cid);
        let len: i32 = match d {
            None => -1,
            Some(d) => (if uplink { d.up_len } else { d.down_len }) as i32,
        };
        if len < 0 {
            lw.push_mac(|w| {
                mac_name(w, cid, uplink);
                w.str(" (unknown: 1.1 encrypted FOpts?)");
            });
            return;
        }
        let d = d.unwrap_or(&MACS[0]);
        let name = if uplink { d.up } else { d.down };
        if i + 1 + len as usize > n {
            lw.push_mac(|w| {
                w.str(name);
                w.str(" (truncated)");
            });
            return;
        }
        lw.push_mac(|w| w.str(name));
        let a = &p[i + 1..];
        let mut v = S256::default();
        if uplink && cid == 0x06 {
            let margin = if a[1] & 0x20 != 0 { (a[1] & 0x3F) as i32 - 64 } else { (a[1] & 0x3F) as i32 };
            lw.dev_status_battery = a[0] as i32;
            lw.dev_status_margin = margin;
            v.write(|w| {
                if a[0] == 0 {
                    w.str("external power, margin ");
                } else if a[0] == 255 {
                    w.str("battery unknown, margin ");
                } else {
                    w.str("battery ");
                    w.fixed(format_f, 0, a[0] as f64 * 100.0 / 254.0);
                    w.str(" %, margin ");
                }
                w.idec(margin as i64);
                w.str(" dB");
            });
        } else if uplink && matches!(cid, 0x03 | 0x05 | 0x07 | 0x0A) {
            v.write(|w| {
                w.str(name);
                w.str(" 0x");
                w.hex(a[0] as u64, 2, true);
            });
        } else if uplink && cid == 0x01 {
            v.write(|w| {
                w.str("LoRaWAN 1.");
                w.idec((a[0] & 15) as i64);
            });
        } else if !uplink && cid == 0x02 {
            v.write(|w| {
                w.str("margin ");
                w.idec(a[0] as i64);
                w.str(" dB, ");
                w.idec(a[1] as i64);
                w.str(" gateways");
            });
        } else if !uplink && cid == 0x03 {
            v.write(|w| {
                w.str("DR");
                w.idec((a[0] >> 4) as i64);
                w.str(", TX power ");
                w.idec((a[0] & 15) as i64);
                w.str(", channel mask 0x");
                w.hex((a[1] as u64) | (a[2] as u64) << 8, 4, true);
            });
        } else if !uplink && cid == 0x0D {
            v.write(|w| {
                w.str("GPS time ");
                w.udec(le32(a) as u64);
                w.str(" s");
            });
        }
        if v.len != 0 {
            let prev = lw.mac_detail;
            lw.mac_detail.write(|w| {
                w.bytes(prev.as_bytes());
                if prev.len != 0 {
                    w.str(", ");
                }
                w.bytes(v.as_bytes());
            });
        }
        i += 1 + len as usize;
    }
}

/// Decodes a LoRaWAN PHY payload; `ok` stays 0 for anything that is not LoRaWAN R1 (bad length, RFU bits set, wrong major version).
pub fn decode(p: &[u8]) -> Lorawan {
    let mut lw = Lorawan::default();
    let n = p.len();
    if n < 1 {
        lw.note.set(b"empty");
        return lw;
    }
    lw.mtype = (p[0] >> 5) as i32;
    lw.major = (p[0] & 3) as i32;
    if lw.mtype != 7 && ((p[0] & 0x1C) != 0 || lw.major != 0) {
        lw.note.set(b"not LoRaWAN R1 (RFU or major bits set)");
        return lw;
    }
    lw.uplink = matches!(lw.mtype, 0 | 2 | 4 | 6) as u8;
    lw.confirmed = matches!(lw.mtype, 4 | 5) as u8;
    lw.type_name.set(mtype_name(lw.mtype).as_bytes());
    match lw.mtype {
        0 => {
            // MHDR JoinEUI(8) DevEUI(8) DevNonce(2) MIC(4), each little endian
            if n != 23 {
                lw.note_fmt(|w| {
                    w.str("join request of ");
                    w.udec(n as u64);
                    w.str(" bytes (23 expected)");
                });
                return lw;
            }
            lw.join_eui = le64(&p[1..]);
            lw.dev_eui = le64(&p[9..]);
            lw.dev_nonce = u16::from_le_bytes([p[17], p[18]]);
            lw.mic = be32(&p[19..]);
            lw.vendor.set(vendor(lw.dev_eui).as_bytes());
            lw.from.write(|w| eui_text(w, lw.dev_eui));
            lw.to.write(|w| eui_text(w, lw.join_eui));
            let (from, to, vn, nonce, mic) = (lw.from, lw.to, lw.vendor, lw.dev_nonce, lw.mic);
            lw.detail.write(|w| {
                w.str("DevEUI ");
                w.bytes(from.as_bytes());
                if vn.len != 0 {
                    w.str(" (");
                    w.bytes(vn.as_bytes());
                    w.byte(b')');
                }
                w.str(", JoinEUI ");
                w.bytes(to.as_bytes());
                w.str(", DevNonce ");
                w.udec(nonce as u64);
                w.str(", MIC ");
                w.hex(mic as u64, 8, true);
            });
        }
        1 => {
            // encrypted with the device's key: only its length says 1.0 / 1.1 CFList or not
            if n != 17 && n != 33 {
                lw.note_fmt(|w| {
                    w.str("join accept of ");
                    w.udec(n as u64);
                    w.str(" bytes (17 or 33 expected)");
                });
                return lw;
            }
            lw.mic = be32(&p[n - 4..]);
            lw.detail.write(|w| {
                w.udec(n as u64 - 1);
                w.str(" bytes encrypted");
                if n == 33 {
                    w.str(" (with a channel list)");
                }
            });
            lw.note.set(b"encrypted");
        }
        6 => {
            // 1.1: type 0 / 2: NetID(3) DevEUI(8) RJcount0(2); type 1: JoinEUI(8) DevEUI(8) RJcount1(2)
            if n < 2 {
                lw.note.set(b"short rejoin request");
                return lw;
            }
            lw.rejoin_type = p[1] as i32;
            if (lw.rejoin_type == 0 || lw.rejoin_type == 2) && n == 19 {
                lw.rejoin_net_id = p[2] as u32 | (p[3] as u32) << 8 | (p[4] as u32) << 16;
                lw.dev_eui = le64(&p[5..]);
                lw.dev_nonce = u16::from_le_bytes([p[13], p[14]]);
            } else if lw.rejoin_type == 1 && n == 24 {
                lw.join_eui = le64(&p[2..]);
                lw.dev_eui = le64(&p[10..]);
                lw.dev_nonce = u16::from_le_bytes([p[18], p[19]]);
                lw.to.write(|w| eui_text(w, lw.join_eui));
            } else {
                let t = lw.rejoin_type;
                lw.note_fmt(|w| {
                    w.str("rejoin request type ");
                    w.idec(t as i64);
                    w.str(" of ");
                    w.udec(n as u64);
                    w.str(" bytes");
                });
                return lw;
            }
            lw.mic = be32(&p[n - 4..]);
            lw.vendor.set(vendor(lw.dev_eui).as_bytes());
            lw.from.write(|w| eui_text(w, lw.dev_eui));
            let (from, vn, nonce, mic, t) = (lw.from, lw.vendor, lw.dev_nonce, lw.mic, lw.rejoin_type);
            lw.detail.write(|w| {
                w.str("type ");
                w.idec(t as i64);
                w.str(", DevEUI ");
                w.bytes(from.as_bytes());
                if vn.len != 0 {
                    w.str(" (");
                    w.bytes(vn.as_bytes());
                    w.byte(b')');
                }
                w.str(", RJcount ");
                w.udec(nonce as u64);
                w.str(", MIC ");
                w.hex(mic as u64, 8, true);
            });
        }
        7 => {
            // proprietary: the layout after MHDR is the network's own
            lw.detail.write(|w| {
                w.udec(n as u64 - 1);
                w.str(" bytes, layout unknown");
            });
            lw.note.set(b"proprietary");
        }
        _ => {
            if !data_frame(p, &mut lw) {
                return lw;
            }
        }
    }
    lw.ok = 1;
    lw.has_lorawan = 1;
    lw
}

/// A data frame: MHDR DevAddr(4) FCtrl(1) FCnt(2) FOpts(0..15) [FPort(1) FRMPayload] MIC(4). False with a note when it does not fit.
fn data_frame(p: &[u8], lw: &mut Lorawan) -> bool {
    let n = p.len();
    if n < 12 {
        lw.note_fmt(|w| {
            w.str("data frame of ");
            w.udec(n as u64);
            w.str(" bytes (12 at least)");
        });
        return false;
    }
    lw.dev_addr = le32(&p[1..]);
    let fc = p[5];
    lw.adr = (fc & 0x80 != 0) as u8;
    lw.ack = (fc & 0x20 != 0) as u8;
    lw.fopts_len = (fc & 15) as i32;
    if lw.uplink != 0 {
        lw.adr_ack_req = (fc & 0x40 != 0) as u8;
        lw.class_b = (fc & 0x10 != 0) as u8;
    } else {
        lw.fpending = (fc & 0x10 != 0) as u8;
    }
    lw.fcnt = u16::from_le_bytes([p[6], p[7]]);
    let fl = lw.fopts_len as usize;
    if 8 + fl + 4 > n {
        let f = lw.fopts_len;
        lw.note_fmt(|w| {
            w.str("FOptsLen ");
            w.idec(f as i64);
            w.str(" does not fit ");
            w.udec(n as u64);
            w.str(" bytes");
        });
        return false;
    }
    if n > 8 + fl + 4 {
        lw.fport = p[8 + fl] as i32;
        lw.frm_len = (n - (8 + fl + 1 + 4)) as u32;
    }
    lw.mic = be32(&p[n - 4..]);
    let up = lw.uplink != 0;
    mac_commands(&p[8..8 + fl], up, lw);
    let mut nw = S64::default();
    let mut info = Network::default();
    nw.write(|w| info = network(w, lw.dev_addr));
    lw.network = nw;
    lw.net_id_type = info.kind;
    if info.have_ids {
        lw.nwk_id = info.nwk_id;
        lw.net_id = info.net_id;
    }
    lw.from.write(|w| w.hex(lw.dev_addr as u64, 8, true));
    lw.packet_id = lw.fcnt as u32;
    let mut d = S2048::default();
    let mut first = true;
    let mut add = |s: &[u8], d: &mut S2048| {
        let prev = *d;
        d.write(|w| {
            w.bytes(prev.as_bytes());
            if !first {
                w.str(", ");
            }
            w.bytes(s);
        });
        first = false;
    };
    if lw.adr != 0 {
        add(b"ADR", &mut d);
    }
    if lw.adr_ack_req != 0 {
        add(b"ADRACKReq", &mut d);
    }
    if lw.ack != 0 {
        add(b"ACK", &mut d);
    }
    if lw.fpending != 0 {
        add(b"FPending", &mut d);
    }
    if lw.class_b != 0 {
        add(b"class B", &mut d);
    }
    for k in 0..lw.mac_count as usize {
        add(lw.mac_names[k].as_bytes(), &mut d);
    }
    if lw.mac_detail.len != 0 {
        add(lw.mac_detail.as_bytes(), &mut d);
    }
    let mut t = S256::default();
    if lw.fport == 0 {
        t.write(|w| {
            w.str("MAC commands in the payload, ");
            w.udec(lw.frm_len as u64);
            w.str(" bytes encrypted");
        });
        add(t.as_bytes(), &mut d);
    } else if lw.fport > 0 {
        t.write(|w| {
            w.str("FPort ");
            w.idec(lw.fport as i64);
            w.str(", ");
            w.udec(lw.frm_len as u64);
            w.str(" bytes encrypted");
        });
        add(t.as_bytes(), &mut d);
    }
    if lw.fport == 0 && lw.fopts_len > 0 {
        add(b"FOpts with FPort 0 (not allowed)", &mut d);
    }
    t.write(|w| {
        w.str("MIC ");
        w.hex(lw.mic as u64, 8, true);
    });
    add(t.as_bytes(), &mut d);
    lw.detail = d;
    lw.note.set(if lw.fport >= 0 { b"payload encrypted" as &[u8] } else { b"no payload" });
    true
}
