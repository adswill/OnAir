// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! SYNC and SYSINFO broadcast PDUs, BS service text, security class, carrier frequencies, network time and the AACH slot usage
//! (ETSI EN 300 392-2 clauses 18.4, 18.5, 21.4 and 21.5). All of it is broadcast to every terminal.
use crate::abi::{Sync, Sysinfo};
use crate::bits::Reader;

/// Result of a parse that can stop at a header mismatch (nothing written) or at the end of the data (fields written, zeros past the end).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Parsed {
    /// Header mismatch: the output was not touched.
    Rejected,
    /// The output was written but the data was too short (or out of range).
    Partial,
    /// Complete.
    Ok,
}

/// SYNC PDU (BSCH, 60 bits, 21.4.4.2 + D-MLE-SYNC 18.4.2.1). The output is always written; `Parsed::Ok` also needs frame 1-18 and multiframe 1-60.
pub fn parse_sync(b: &[u8], s: &mut Sync) -> Parsed {
    let mut r = Reader::new(b, 0, 60);
    s.system_code = r.get(4) as i32;
    s.colour_code = r.get(6) as i32;
    s.tn = r.get(2) as i32 + 1;
    s.fn_ = r.get(5) as i32;
    s.mn = r.get(6) as i32;
    s.sharing_mode = r.get(2) as i32;
    s.ts_reserved_frames = r.get(3) as i32;
    s.uplane_dtx = r.bit() as i32;
    s.frame18_ext = r.bit() as i32;
    r.get(1);
    s.mcc = r.get(10) as i32;
    s.mnc = r.get(14) as i32;
    s.nbr_broadcast = r.get(2) as i32;
    s.cell_load = r.get(2) as i32;
    s.late_entry = r.bit() as i32;
    if r.ok && s.fn_ >= 1 && s.fn_ <= 18 && s.mn >= 1 && s.mn <= 60 {
        Parsed::Ok
    } else {
        Parsed::Partial
    }
}

/// SYSINFO (SCH/HD on the BNCH, 124 bits, 21.4.4.1 with D-MLE-SYSINFO 18.4.2.2); `b` starts at the MAC PDU type (10 broadcast, 00 SYSINFO).
pub fn parse_sysinfo(b: &[u8], s: &mut Sysinfo) -> Parsed {
    let mut r = Reader::new(b, 0, 124);
    if r.get(2) != 2 || r.get(2) != 0 {
        return Parsed::Rejected;
    }
    s.main_carrier = r.get(12) as i32;
    s.band = r.get(4) as i32;
    s.offset = r.get(2) as i32;
    s.duplex = r.get(3) as i32;
    s.reverse = r.bit() as i32;
    s.num_scch = r.get(2) as i32;
    s.ms_tx_pwr_max = r.get(3) as i32;
    s.rx_lev_access_min = r.get(4) as i32;
    s.access_param = r.get(4) as i32;
    s.dl_timeout = r.get(4) as i32;
    s.cck_flag = r.bit() as i32;
    s.hyperframe_or_cck = r.get(16) as i32;
    s.opt_flag = r.get(2) as i32;
    s.opt_value = r.get(20) as u32;
    s.la = r.get(14) as i32;
    s.subscriber_class = r.get(16) as i32;
    s.bs_service = r.get(12) as i32;
    if r.ok {
        Parsed::Ok
    } else {
        Parsed::Partial
    }
}

/// Text sink over a caller buffer; what does not fit is dropped.
pub struct Buf<'a> {
    pub b: &'a mut [u8],
    pub n: usize,
}

impl<'a> Buf<'a> {
    /// Append bytes.
    pub fn put(&mut self, s: &[u8]) {
        for &c in s {
            if self.n < self.b.len() {
                self.b[self.n] = c;
                self.n += 1;
            }
        }
    }
}

/// BS service details bits (18.5.2), bit 11 first.
pub const SERVICES: [(i32, &[u8]); 11] = [
    (1 << 11, b"registration required"),
    (1 << 10, b"de-registration required"),
    (1 << 9, b"priority cell"),
    (1 << 8, b"never minimum mode"),
    (1 << 7, b"migration"),
    (1 << 6, b"system wide services"),
    (1 << 5, b"voice"),
    (1 << 4, b"circuit data"),
    (1 << 2, b"packet data (SNDCP)"),
    (1 << 1, b"air interface encryption"),
    (1 << 0, b"advanced link"),
];

/// Comma separated names of the BS services set in `v`, or "none"; returns the length written.
pub fn service_text(v: i32, out: &mut [u8]) -> usize {
    let mut w = Buf { b: out, n: 0 };
    let mut first = true;
    for (bit, name) in SERVICES.iter() {
        if v & bit != 0 {
            if !first {
                w.put(b", ");
            }
            first = false;
            w.put(name);
        }
    }
    if first {
        w.put(b"none");
    }
    w.n
}

/// Security information of the extended services broadcast (SYSINFO optional field 3, 21.4.4.1): bit 0 valid, 1 authentication required, 2 class 1, 3 class 2, 4 class 3 (class 2 / 3 only with the cell's air interface encryption service bit).
pub fn security(s: &Sysinfo) -> i32 {
    if s.opt_flag != 3 {
        return 0;
    }
    let sec = ((s.opt_value >> 12) & 0xFF) as i32;
    let auth = (sec >> 7) & 1;
    let c1 = (sec >> 6) & 1;
    // Class 2 / 3 bit (EN 300 392-7 table A.104): only meaningful when the cell's air interface encryption service bit is set.
    let aie = s.bs_service & (1 << 1) != 0;
    let c3 = (aie && (sec >> 5) & 1 != 0) as i32;
    let c2 = (aie && (sec >> 5) & 1 == 0) as i32;
    1 | auth << 1 | c1 << 2 | c2 << 3 | c3 << 4
}

/// Downlink carrier frequency in Hz: band * 100 MHz + carrier * 25 kHz + offset (0, +6.25, -6.25, +12.5 kHz), EN 300 392-2 clause 21.4.4.1.
pub fn dl_carrier_hz(band: i32, carrier: i32, offset: i32) -> f64 {
    const OFF: [f64; 4] = [0.0, 6250.0, -6250.0, 12500.0];
    band as f64 * 100e6 + carrier as f64 * 25e3 + OFF[(offset & 3) as usize]
}

/// Duplex spacing in kHz per band (rows 0-7: frequency band) and duplex field (columns), -1 reserved (TS 100 392-15 table of duplex spacings).
const DUPLEX_KHZ: [[i32; 16]; 8] = [
    [-1, 1600, 10000, 10000, 10000, 10000, 10000, -1, -1, -1, -1, -1, -1, -1, -1, -1],
    [-1, 4500, -1, 36000, 7000, -1, -1, -1, 45000, 45000, -1, -1, -1, -1, -1, -1],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [-1, -1, -1, 8000, 8000, -1, -1, -1, 18000, 18000, -1, -1, -1, -1, -1, -1],
    [-1, -1, -1, 18000, 5000, -1, 30000, 30000, -1, 39000, -1, -1, -1, -1, -1, -1],
    [-1, -1, -1, -1, 9500, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1],
    [-1; 16],
    [-1; 16],
];

/// Duplex spacing in Hz; negative when reserved. Note the table is indexed `[duplex & 7][band & 15]` exactly as the C++ original.
pub fn duplex_spacing_hz(band: i32, duplex: i32) -> f64 {
    let v = DUPLEX_KHZ[(duplex & 7) as usize][(band & 15) as usize];
    if v < 0 {
        -1.0
    } else {
        v as f64 * 1e3
    }
}

/// Uplink carrier frequency in Hz (0 when the duplex spacing is reserved).
pub fn ul_carrier_hz(band: i32, carrier: i32, offset: i32, duplex: i32, reverse: bool) -> f64 {
    let d = duplex_spacing_hz(band, duplex);
    if d < 0.0 {
        return 0.0;
    }
    let dl = dl_carrier_hz(band, carrier, offset);
    if reverse {
        dl + d
    } else {
        dl - d
    }
}

/// TETRA network time (18.5.24) as "YYYY-MM-DD HH:MM:SS UTC, local +HH:MM" written to `out`; returns the length, 0 when the time is not valid.
pub fn network_time_text(t: u64, out: &mut [u8]) -> usize {
    let utc2 = ((t >> 24) & 0xFF_FFFF) as u32;
    let sign = (t >> 23) & 1;
    let offs = ((t >> 17) & 0x3F) as u32;
    let year = 2000 + ((t >> 11) & 0x3F) as u32;
    if utc2 == 0xFF_FFFF {
        return 0;
    }
    let mut sec = utc2 * 2;
    let mut day = sec / 86400;
    sec %= 86400;
    const MD: [u32; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    let leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    let mut mon = 0usize;
    while mon < 12 {
        let d = MD[mon] + (mon == 1 && leap) as u32;
        if day < d {
            break;
        }
        day -= d;
        mon += 1;
    }
    if mon >= 12 {
        return 0;
    }
    let mut w = Buf { b: out, n: 0 };
    w.num2(year as u64, 4);
    w.put(b"-");
    w.num2(mon as u64 + 1, 2);
    w.put(b"-");
    w.num2(day as u64 + 1, 2);
    w.put(b" ");
    w.num2((sec / 3600) as u64, 2);
    w.put(b":");
    w.num2(((sec / 60) % 60) as u64, 2);
    w.put(b":");
    w.num2((sec % 60) as u64, 2);
    w.put(if sign != 0 { b" UTC, local -" } else { b" UTC, local +" });
    w.num2((offs * 15 / 60) as u64, 2);
    w.put(b":");
    w.num2((offs * 15 % 60) as u64, 2);
    w.n
}

impl<'a> Buf<'a> {
    /// Append a number, zero padded to `w` digits.
    pub fn num2(&mut self, v: u64, w: usize) {
        let mut t = [b'0'; 20];
        let mut x = v;
        let mut i = t.len();
        while i > 0 {
            i -= 1;
            t[i] = b'0' + (x % 10) as u8;
            x /= 10;
            if x == 0 {
                break;
            }
        }
        let start = i.min(t.len() - w);
        self.put(&t[start..]);
    }
}

/// ACCESS-ASSIGN (21.4.7): the downlink use of the slot in frames 1-17: -1 not stated, 0 unallocated, 1 assigned control, 2 common control, 3 reserved, 4 and up traffic.
pub fn aach_dl_usage(aa: u16, frame18: bool) -> i32 {
    if frame18 {
        return -1;
    }
    let hdr = (aa >> 12) & 3;
    let f1 = ((aa >> 6) & 63) as i32;
    if hdr == 0 {
        2
    } else {
        f1
    }
}

/// Name of a downlink usage value as a NUL-terminated static string.
pub fn dl_usage_text(u: i32) -> &'static [u8] {
    match u {
        -1 => b"-\0",
        0 => b"unallocated\0",
        1 => b"assigned control\0",
        2 => b"common control\0",
        3 => b"reserved\0",
        _ => b"traffic\0",
    }
}
