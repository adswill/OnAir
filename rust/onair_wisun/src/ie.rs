// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Wi-SUN FAN 1.0 information elements: the unicast / broadcast schedule IEs (US-IE, BS-IE) with their channel information, the header IEs (WH-IE sub-IDs:
//! UTT, BT, FC, RSL, EA, LBC, ...), the payload IEs (WP-IE sub-IEs: PAN, NETNAME, PANVER, GTKHASH, POM, ...) and the MPX IE with its EAPOL summary.
//! Wi-SUN FAN TPS 1.0 section 6.3 (frame types and IEs), IEEE 802.15.4-2015 section 7.4 (IE format).
use onair_common::text::Buf;

/// A unicast (US-IE) or broadcast (BS-IE) schedule, `repr(C)`; fields not in the IE keep the -1 / 0 defaults.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Schedule {
    pub present: u8,
    pub bcast_interval_ms: u32,
    /// BS-IE: broadcast schedule identifier (-1 none).
    pub bsi: i32,
    pub dwell_ms: i32,
    pub clock_drift: i32,
    pub timing_accuracy: i32,
    /// 0 regulatory domain and operating class, 1 explicit, 2 regulatory domain and channel plan ID.
    pub ch_plan: i32,
    /// 0 fixed, 1 TR51CF, 2 DH1CF, 3 vendor.
    pub ch_function: i32,
    /// 0 none, 1 ranges, 2 bit mask.
    pub excluded: i32,
    pub reg_domain: i32,
    pub op_class: i32,
    pub ch_plan_id: i32,
    /// Explicit plan: first channel in MHz.
    pub ch0_mhz: f64,
    /// Explicit plan: 0 200 kHz, 1 400 kHz, 2 600 kHz, 3 100 kHz (-1 none).
    pub spacing_code: i32,
    pub num_channels: i32,
    pub fixed_channel: i32,
    /// Channels excluded (ranges: their size; mask: bits set).
    pub excluded_count: i32,
}

impl Default for Schedule {
    fn default() -> Self {
        Schedule {
            present: 0,
            bcast_interval_ms: 0,
            bsi: -1,
            dwell_ms: 0,
            clock_drift: 0,
            timing_accuracy: 0,
            ch_plan: 0,
            ch_function: 0,
            excluded: 0,
            reg_domain: -1,
            op_class: -1,
            ch_plan_id: -1,
            ch0_mhz: 0.0,
            spacing_code: -1,
            num_channels: 0,
            fixed_channel: -1,
            excluded_count: 0,
        }
    }
}

pub(crate) fn le16(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}
pub(crate) fn le24(p: &[u8]) -> u32 {
    p[0] as u32 | (p[1] as u32) << 8 | (p[2] as u32) << 16
}
pub(crate) fn le32(p: &[u8]) -> u32 {
    le24(p) | (p[3] as u32) << 24
}
pub(crate) fn le64(p: &[u8]) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&p[..8]);
    u64::from_le_bytes(b)
}

/// The channel information shared by the US-IE and the BS-IE (channel plan, function, excluded channels).
fn read_channels(p: &[u8], s: &mut Schedule) {
    let n = p.len();
    if n < 1 {
        return;
    }
    let c = p[0];
    s.ch_plan = (c & 7) as i32;
    s.ch_function = ((c >> 3) & 7) as i32;
    s.excluded = ((c >> 6) & 3) as i32;
    let mut i = 1usize;
    if s.ch_plan == 0 {
        if i + 2 > n {
            return;
        }
        s.reg_domain = p[i] as i32;
        s.op_class = p[i + 1] as i32;
        i += 2;
    } else if s.ch_plan == 1 {
        if i + 6 > n {
            return;
        }
        s.ch0_mhz = le24(&p[i..]) as f64 / 1000.0;
        s.spacing_code = (p[i + 3] & 0x0F) as i32;
        s.num_channels = le16(&p[i + 4..]) as i32;
        i += 6;
    } else if s.ch_plan == 2 {
        if i + 2 > n {
            return;
        }
        s.reg_domain = p[i] as i32;
        s.ch_plan_id = p[i + 1] as i32;
        i += 2;
    }
    if s.ch_function == 0 {
        if i + 2 > n {
            return;
        }
        s.fixed_channel = le16(&p[i..]) as i32;
        i += 2;
    }
    if s.excluded == 1 && i < n {
        let ranges = p[i] as usize;
        i += 1;
        let mut r = 0;
        while r < ranges && i + 4 <= n {
            s.excluded_count += le16(&p[i + 2..]) as i32 - le16(&p[i..]) as i32 + 1;
            r += 1;
            i += 4;
        }
    } else if s.excluded == 2 {
        s.excluded_count += p[i..].iter().map(|b| b.count_ones() as i32).sum::<i32>();
    }
}

/// US-IE (unicast schedule): dwell interval, clock drift, timing accuracy, channels.
pub fn read_us(p: &[u8], s: &mut Schedule) {
    if p.len() < 4 {
        return;
    }
    s.present = 1;
    s.dwell_ms = p[0] as i32;
    s.clock_drift = p[1] as i32;
    s.timing_accuracy = p[2] as i32;
    read_channels(&p[3..], s);
}

/// BS-IE (broadcast schedule): interval, schedule id, dwell interval, clock drift, timing accuracy, channels.
pub fn read_bs(p: &[u8], s: &mut Schedule) {
    if p.len() < 10 {
        return;
    }
    s.present = 1;
    s.bcast_interval_ms = le32(p);
    s.bsi = le16(&p[4..]) as i32;
    s.dwell_ms = p[6] as i32;
    s.clock_drift = p[7] as i32;
    s.timing_accuracy = p[8] as i32;
    read_channels(&p[9..], s);
}

/// Appends an IE name to the list, separated by blanks.
pub fn add_ie(w: &mut Buf, name: &str) {
    if !w.is_empty() {
        w.byte(b' ');
    }
    w.str(name);
}

/// Same with a name made of a prefix and a hex number (`WH%02X`, `H%02X`, `WPL%X`, `P%X`).
pub fn add_ie_hex(w: &mut Buf, prefix: &str, v: u32, width: usize) {
    if !w.is_empty() {
        w.byte(b' ');
    }
    w.str(prefix);
    w.hex(v as u64, width, true);
}

/// EAP code name (RFC 3748 4).
pub fn eap_code_name(c: i32) -> &'static str {
    match c {
        1 => "request",
        2 => "response",
        3 => "success",
        4 => "failure",
        _ => "?",
    }
}

/// EAP type name (RFC 3748 5): identity (1) and TLS (13); empty otherwise.
pub fn eap_type_name(t: i32) -> &'static str {
    match t {
        1 => "identity",
        13 => "TLS",
        _ => "",
    }
}
