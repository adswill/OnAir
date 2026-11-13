// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The 802.15.4-2015 MAC header of a Wi-SUN frame (frame version 2: PAN ID rules of table 7-2, auxiliary security header), its header and payload IEs and the
//! one-line summary. Secured frames keep their payload IEs and payload encrypted (reported as such, never decoded). Wi-SUN FAN TPS 1.0 section 6.3.
use crate::ie::{self, add_ie, add_ie_hex, le16, le24, le32, le64, Schedule};
use crate::names;
use onair_common::text::{Buf, Str};

/// Text with the IEs in order ("UTT BT LBC HT1"): at most about 9 characters per 2 octets of a 2047 octet PSDU.
pub type S8192 = Str<8192>;

/// A parsed MPDU (FCS excluded), `repr(C)`; fields that were not read keep the defaults of the C++ struct.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Frame {
    /// The header could be read.
    pub ok: u8,
    pub secured: u8,
    pub pending: u8,
    pub ack_req: u8,
    pub pan_compress: u8,
    pub seq_suppressed: u8,
    pub ie_list: u8,
    pub has_dest_pan: u8,
    pub has_src_pan: u8,
    pub has_bt: u8,
    pub has_fc: u8,
    pub has_lbc: u8,
    pub has_pan_ie: u8,
    pub use_parent_bs: u8,
    pub rpl_routing: u8,
    pub has_net_name: u8,
    pub has_gtk_hash: u8,
    pub encrypted: u8,
    /// Frame type, version, address modes (0 none, 2 short, 3 extended), sequence number (-1 none).
    pub kind: i32,
    pub version: i32,
    pub dest_mode: i32,
    pub src_mode: i32,
    pub seq: i32,
    pub dest_pan: u16,
    pub src_pan: u16,
    pub dest_short: u16,
    pub src_short: u16,
    pub dest_ext: u64,
    pub src_ext: u64,
    /// Auxiliary security header.
    pub sec_level: i32,
    pub key_id_mode: i32,
    pub key_index: i32,
    pub frame_counter: u32,
    /// Wi-SUN frame type of the UTT-IE (-1 none), unicast fractional sequence interval, broadcast slot number / interval offset, reported RSL (-999 none).
    pub utt: i32,
    pub ufsi: u32,
    pub bsn: i32,
    pub bio: u32,
    pub rsl_dbm: i32,
    pub tx_flow: i32,
    pub rx_flow: i32,
    pub ea_eui: u64,
    pub lfn_bcast_ms: u32,
    pub us: Schedule,
    pub bs: Schedule,
    /// PAN-IE and the other payload IE values (-1: not present).
    pub pan_size: i32,
    pub routing_cost: i32,
    pub tps_version: i32,
    pub pan_version: i32,
    pub lfn_version: i32,
    /// POM-IE: PHY operating mode ids (`phy_mode_count` of them; 2048 is more than the 15 per IE of a 2047 octet PSDU can hold).
    pub phy_mode_count: i32,
    pub phy_modes: [i32; 2048],
    /// MPX IE: multiplex id (-1 none), KMP id, EAPOL packet type, EAP code and type (-1 none).
    pub mpx_id: i32,
    pub kmp_id: i32,
    pub eapol_type: i32,
    pub eap_code: i32,
    pub eap_type: i32,
    /// Where the payload starts in the MPDU and its length.
    pub payload_at: u64,
    pub payload_len: u64,
    pub net_name: Str<64>,
    pub ies: S8192,
    pub info: Str<256>,
}

impl Default for Frame {
    fn default() -> Self {
        Frame {
            ok: 0,
            secured: 0,
            pending: 0,
            ack_req: 0,
            pan_compress: 0,
            seq_suppressed: 0,
            ie_list: 0,
            has_dest_pan: 0,
            has_src_pan: 0,
            has_bt: 0,
            has_fc: 0,
            has_lbc: 0,
            has_pan_ie: 0,
            use_parent_bs: 0,
            rpl_routing: 0,
            has_net_name: 0,
            has_gtk_hash: 0,
            encrypted: 0,
            kind: 0,
            version: 0,
            dest_mode: 0,
            src_mode: 0,
            seq: -1,
            dest_pan: 0,
            src_pan: 0,
            dest_short: 0,
            src_short: 0,
            dest_ext: 0,
            src_ext: 0,
            sec_level: -1,
            key_id_mode: -1,
            key_index: -1,
            frame_counter: 0,
            utt: -1,
            ufsi: 0,
            bsn: 0,
            bio: 0,
            rsl_dbm: -999,
            tx_flow: 0,
            rx_flow: 0,
            ea_eui: 0,
            lfn_bcast_ms: 0,
            us: Schedule::default(),
            bs: Schedule::default(),
            pan_size: -1,
            routing_cost: -1,
            tps_version: -1,
            pan_version: -1,
            lfn_version: -1,
            phy_mode_count: 0,
            phy_modes: [0; 2048],
            mpx_id: -1,
            kmp_id: -1,
            eapol_type: -1,
            eap_code: -1,
            eap_type: -1,
            payload_at: 0,
            payload_len: 0,
            net_name: Str::default(),
            ies: S8192::default(),
            info: Str::default(),
        }
    }
}

/// Appends an IE name to the frame's list.
fn name(f: &mut Frame, n: &str) {
    f.ies.append(|w| add_ie(w, n));
}

/// Appends an IE name made of a prefix and an upper case hex number.
fn name_hex(f: &mut Frame, prefix: &str, v: u32, width: usize) {
    f.ies.append(|w| add_ie_hex(w, prefix, v, width));
}

/// The Wi-SUN header IE (WH-IE, element ID 0x2A): its sub-ID first.
fn read_wh(p: &[u8], f: &mut Frame) {
    if p.is_empty() {
        return;
    }
    let sub = p[0] as u32;
    let p = &p[1..];
    let n = p.len();
    match sub {
        0x01 => {
            if n >= 4 {
                f.utt = (p[0] & 0x0F) as i32;
                f.ufsi = le24(&p[1..]);
            }
            name(f, "UTT");
        }
        0x02 => {
            if n >= 5 {
                f.has_bt = 1;
                f.bsn = le16(p) as i32;
                f.bio = le24(&p[2..]);
            }
            name(f, "BT");
        }
        0x03 => {
            if n >= 2 {
                f.has_fc = 1;
                f.tx_flow = p[0] as i32;
                f.rx_flow = p[1] as i32;
            }
            name(f, "FC");
        }
        0x04 => {
            if n >= 1 {
                f.rsl_dbm = p[0] as i32 - 174;
            }
            name(f, "RSL");
        }
        0x05 => name(f, "MHDS"),
        0x06 => name(f, "VH"),
        0x09 => {
            if n >= 8 {
                f.ea_eui = le64(p);
            }
            name(f, "EA");
        }
        0x0A => name(f, "LUTT"),
        0x0B => name(f, "LBT"),
        0x0C => name(f, "NR"),
        0x0D => name(f, "LUS"),
        0x0E => name(f, "FLUS"),
        0x0F => name(f, "LBS"),
        0x10 => name(f, "LND"),
        0x11 => name(f, "LTO"),
        0x12 => name(f, "PANID"),
        0x80 => {
            if n >= 4 {
                f.has_lbc = 1;
                f.lfn_bcast_ms = le24(p);
            }
            name(f, "LBC");
        }
        _ => name_hex(f, "WH", sub, 2),
    }
}

/// The Wi-SUN payload IE (WP-IE, group 0x4): nested sub-IEs, short (8 bit length, 7 bit sub-ID) or long (11 bit length, 4 bit sub-ID).
fn read_wp(p: &[u8], f: &mut Frame) {
    let n = p.len();
    let mut i = 0usize;
    while i + 2 <= n {
        let d = le16(&p[i..]);
        i += 2;
        let lng = d >> 15 != 0;
        let len = if lng { (d & 0x7FF) as usize } else { (d & 0xFF) as usize };
        let sub = if lng { ((d >> 11) & 0x0F) as u32 } else { ((d >> 8) & 0x7F) as u32 };
        if i + len > n {
            break;
        }
        let q = &p[i..i + len];
        if lng {
            match sub {
                1 => {
                    ie::read_us(q, &mut f.us);
                    name(f, "US");
                }
                2 => {
                    ie::read_bs(q, &mut f.bs);
                    name(f, "BS");
                }
                3 => name(f, "VP"),
                4 => name(f, "LCP"),
                _ => name_hex(f, "WPL", sub, 1),
            }
        } else {
            match sub {
                0x04 => {
                    if len >= 5 {
                        f.has_pan_ie = 1;
                        f.pan_size = le16(q) as i32;
                        f.routing_cost = le16(&q[2..]) as i32;
                        f.use_parent_bs = q[4] & 1;
                        f.rpl_routing = (q[4] >> 1) & 1;
                        f.tps_version = ((q[4] >> 5) & 7) as i32;
                    }
                    name(f, "PAN");
                }
                0x05 => {
                    f.has_net_name = 1;
                    f.net_name.set(&q[..len.min(32)]);
                    name(f, "NETNAME");
                }
                0x06 => {
                    if len >= 2 {
                        f.pan_version = le16(q) as i32;
                    }
                    name(f, "PANVER");
                }
                0x07 => {
                    f.has_gtk_hash = 1;
                    name(f, "GTKHASH");
                }
                0x08 => {
                    if len >= 1 {
                        let c = (q[0] & 0x0F) as usize;
                        let mut k = 0;
                        while k < c && 1 + k < len {
                            if (f.phy_mode_count as usize) < f.phy_modes.len() {
                                f.phy_modes[f.phy_mode_count as usize] = q[1 + k] as i32;
                                f.phy_mode_count += 1;
                            }
                            k += 1;
                        }
                    }
                    name(f, "POM");
                }
                0x40 => {
                    if len >= 2 {
                        f.lfn_version = le16(q) as i32;
                    }
                    name(f, "LFNVER");
                }
                0x41 => name(f, "LGTKHASH"),
                _ => name_hex(f, "WP", sub, 2),
            }
        }
        i += len;
    }
}

/// The MPX IE (group 0x3): transaction control, multiplex id or KMP id; for IEEE 802.1X (KMP 1) the EAPOL type and the EAP code / type.
fn read_mpx(p: &[u8], f: &mut Frame) {
    let n = p.len();
    name(f, "MPX");
    if n < 1 {
        return;
    }
    let transfer = p[0] & 7;
    let mut i = 1usize;
    if transfer == 0 || transfer == 1 {
        // full frame (with multiplex ID), first fragment
        if i + 2 > n {
            return;
        }
        f.mpx_id = le16(&p[i..]) as i32;
        i += 2;
        if f.mpx_id == 0x0001 && i < n {
            f.kmp_id = p[i] as i32; // a KMP: its ID
            i += 1;
        }
    }
    if f.kmp_id == 1 && i + 4 <= n {
        // IEEE 802.1X: an EAPOL frame
        f.eapol_type = p[i + 1] as i32;
        if f.eapol_type == 0 && i + 4 + 4 <= n {
            f.eap_code = p[i + 4] as i32;
            if (f.eap_code == 1 || f.eap_code == 2) && i + 9 <= n {
                f.eap_type = p[i + 8] as i32;
            }
        }
    }
}

/// `%02X:` pairs of an EUI-64, most significant byte first.
pub fn eui_text(w: &mut Buf, e: u64) {
    for k in (0..8).rev() {
        w.hex((e >> (8 * k)) & 0xFF, 2, true);
        if k != 0 {
            w.byte(b':');
        }
    }
}

/// Reads an MPDU (FCS excluded); the header is decoded as far as it goes and the frame says how far (`ok`).
pub fn parse(d: &[u8]) -> Frame {
    let n = d.len();
    let mut f = Frame::default();
    if n < 2 {
        return f;
    }
    let fc = le16(d);
    f.kind = (fc & 7) as i32;
    f.secured = ((fc >> 3) & 1) as u8;
    f.pending = ((fc >> 4) & 1) as u8;
    f.ack_req = ((fc >> 5) & 1) as u8;
    f.pan_compress = ((fc >> 6) & 1) as u8;
    f.seq_suppressed = ((fc >> 8) & 1) as u8;
    f.ie_list = ((fc >> 9) & 1) as u8;
    f.dest_mode = ((fc >> 10) & 3) as i32;
    f.version = ((fc >> 12) & 3) as i32;
    f.src_mode = ((fc >> 14) & 3) as i32;
    let mut i = 2usize;
    if !(f.version == 2 && f.seq_suppressed != 0) {
        if i >= n {
            return f;
        }
        f.seq = d[i] as i32;
        i += 1;
    }
    // which PAN IDs are present (802.15.4-2015 table 7-2 for frame version 2, the 2006 rules before it)
    let d_a = f.dest_mode != 0;
    let s_a = f.src_mode != 0;
    let pc = f.pan_compress != 0;
    if f.version == 2 {
        if !d_a && !s_a {
            f.has_dest_pan = pc as u8;
        } else if d_a && !s_a {
            f.has_dest_pan = !pc as u8;
        } else if !d_a && s_a {
            f.has_src_pan = !pc as u8;
        } else if f.dest_mode == 3 && f.src_mode == 3 {
            f.has_dest_pan = !pc as u8;
        } else {
            f.has_dest_pan = 1;
            f.has_src_pan = !pc as u8;
        }
    } else {
        f.has_dest_pan = d_a as u8;
        f.has_src_pan = (s_a && !(pc && d_a)) as u8;
    }
    let need = |i: usize, k: usize| i + k <= n;
    if f.has_dest_pan != 0 {
        if !need(i, 2) {
            return f;
        }
        f.dest_pan = le16(&d[i..]);
        i += 2;
    }
    if f.dest_mode == 2 {
        if !need(i, 2) {
            return f;
        }
        f.dest_short = le16(&d[i..]);
        i += 2;
    } else if f.dest_mode == 3 {
        if !need(i, 8) {
            return f;
        }
        f.dest_ext = le64(&d[i..]);
        i += 8;
    }
    if f.has_src_pan != 0 {
        if !need(i, 2) {
            return f;
        }
        f.src_pan = le16(&d[i..]);
        i += 2;
    }
    if f.src_mode == 2 {
        if !need(i, 2) {
            return f;
        }
        f.src_short = le16(&d[i..]);
        i += 2;
    } else if f.src_mode == 3 {
        if !need(i, 8) {
            return f;
        }
        f.src_ext = le64(&d[i..]);
        i += 8;
    }
    if f.secured != 0 {
        if !need(i, 5) {
            return f;
        }
        let sc = d[i];
        f.sec_level = (sc & 7) as i32;
        f.key_id_mode = ((sc >> 3) & 3) as i32;
        let no_counter = (sc >> 5) & 1 != 0;
        i += 1;
        if !no_counter {
            f.frame_counter = le32(&d[i..]);
            i += 4;
        }
        const KEY_LEN: [usize; 4] = [0, 1, 5, 9];
        let kl = KEY_LEN[f.key_id_mode as usize];
        if !need(i, kl) {
            return f;
        }
        if f.key_id_mode != 0 {
            f.key_index = d[i + kl - 1] as i32;
        }
        i += kl;
    }
    f.ok = 1;
    // header IEs (always in the clear), then payload IEs and payload (encrypted when secured)
    let mut payload_ies = false;
    if f.ie_list != 0 {
        while i + 2 <= n {
            let ie = le16(&d[i..]);
            let len = (ie & 0x7F) as usize;
            let id = ((ie >> 7) & 0xFF) as u32;
            i += 2;
            if i + len > n {
                f.ok = 0;
                break;
            }
            if id == 0x2A {
                read_wh(&d[i..i + len], &mut f);
            } else if id == 0x7E {
                name(&mut f, "HT1");
                i += len;
                payload_ies = true;
                break;
            } else if id == 0x7F {
                name(&mut f, "HT2");
                i += len;
                break;
            } else {
                name_hex(&mut f, "H", id, 2);
            }
            i += len;
        }
    }
    if f.secured != 0 {
        f.encrypted = 1;
        f.payload_at = i as u64;
        f.payload_len = (n - i) as u64;
    } else {
        if payload_ies {
            while i + 2 <= n {
                let ie = le16(&d[i..]);
                let len = (ie & 0x7FF) as usize;
                let group = ((ie >> 11) & 0x0F) as u32;
                i += 2;
                if i + len > n {
                    break;
                }
                if group == 0x4 {
                    read_wp(&d[i..i + len], &mut f);
                } else if group == 0x3 {
                    read_mpx(&d[i..i + len], &mut f);
                } else if group == 0xF {
                    name(&mut f, "PT");
                    i += len;
                    break;
                } else {
                    name_hex(&mut f, "P", group, 1);
                }
                i += len;
            }
        }
        f.payload_at = i as u64;
        f.payload_len = (n - i) as u64;
    }
    summary(&mut f);
    f
}

/// The one-line summary of what the frame is.
fn summary(f: &mut Frame) {
    let fr = *f;
    f.info.write(|w| {
        w.str(if fr.utt >= 0 { names::utt_name(fr.utt) } else { names::frame_type_name(fr.kind) });
        if fr.has_net_name != 0 {
            w.str(" \"");
            w.bytes(fr.net_name.as_bytes());
            w.byte(b'"');
        }
        if fr.has_pan_ie != 0 {
            w.str(", PAN size ");
            w.idec(fr.pan_size as i64);
            w.str(", routing cost ");
            w.idec(fr.routing_cost as i64);
        }
        if fr.pan_version >= 0 {
            w.str(", PAN version ");
            w.idec(fr.pan_version as i64);
        }
        if fr.eapol_type >= 0 {
            if fr.eapol_type == 0 && fr.eap_code >= 0 {
                w.str(", EAP ");
                w.str(ie::eap_code_name(fr.eap_code));
                let tn = if fr.eap_type >= 0 { ie::eap_type_name(fr.eap_type) } else { "" };
                if fr.eap_type >= 0 && !tn.is_empty() {
                    w.byte(b' ');
                }
                w.str(tn);
            } else if fr.eapol_type == 3 {
                w.str(", EAPOL-Key");
            }
        }
        if fr.ea_eui != 0 {
            w.str(", authenticator ");
            eui_text(w, fr.ea_eui);
        }
        if fr.rsl_dbm > -999 {
            w.str(", RSL ");
            w.idec(fr.rsl_dbm as i64);
            w.str(" dBm");
        }
        if fr.encrypted != 0 {
            w.str(", encrypted (key ");
            w.idec(fr.key_index as i64);
            w.byte(b')');
        }
    });
}
