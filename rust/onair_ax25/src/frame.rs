// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! AX.25 v2.2 frame parsing and the text forms of a frame (callsign with SSID, digipeater path, TNC-2 monitor line, frame type name) and the FCS.
//! Frames come without HDLC flags; the FCS is CRC-16/X-25 sent low byte first (AX.25 v2.2 section 3.8). Building frames stays in C++ (transmitter side).
use crate::arena::Arena;
use onair_common::crc::crc16_x25;

/// The AX.25 frame check sequence of `d` (CRC-16/X-25: reflected, start 0xFFFF, result inverted).
pub fn fcs(d: &[u8]) -> u16 {
    crc16_x25(d)
}

/// True when the last two bytes of `d` are the FCS of the bytes before them.
pub fn fcs_ok(d: &[u8]) -> bool {
    let n = d.len();
    if n < 3 {
        return false;
    }
    let c = fcs(&d[..n - 2]);
    d[n - 2] == (c & 0xFF) as u8 && d[n - 1] == (c >> 8) as u8
}

/// An address as parsed from a frame: callsign (up to 6 characters, spaces removed), SSID and the H / command-response bit.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Addr {
    /// Callsign characters.
    pub call: [u8; 8],
    /// Number of callsign characters (1 to 6).
    pub call_len: u8,
    /// "Has been repeated" bit of a digipeater, command/response bit of source and destination.
    pub h: bool,
    /// SSID 0 to 15.
    pub ssid: i32,
}

/// Longest digipeater path of a frame (AX.25 v2.2 allows eight).
pub const MAX_PATH: usize = 8;

/// A parsed frame; `info_off` is where the information field starts in the input bytes.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Frame {
    /// Destination address.
    pub to: Addr,
    /// Source address.
    pub from: Addr,
    /// Digipeaters.
    pub path: [Addr; MAX_PATH],
    /// Number of digipeaters.
    pub npath: u32,
    /// Control field.
    pub control: i32,
    /// Protocol id, -1 when the frame type carries none.
    pub pid: i32,
    /// Offset of the information field (it runs to the end of the input).
    pub info_off: usize,
}

/// Parses one address field of 7 bytes (AX.25 v2.2 section 3.12). Returns the address and whether it is the last one of the address field.
fn parse_addr(p: &[u8]) -> Option<(Addr, bool)> {
    let mut a = Addr::default();
    let mut n = 0;
    for &b in &p[..6] {
        let ch = b >> 1;
        if b & 1 != 0 {
            return None; // only the last byte of the last address ends the field
        }
        if ch == b' ' {
            continue;
        }
        if !(ch.is_ascii_digit() || ch.is_ascii_uppercase()) {
            return None;
        }
        a.call[n] = ch;
        n += 1;
    }
    if n == 0 {
        return None;
    }
    a.call_len = n as u8;
    a.ssid = i32::from((p[6] >> 1) & 0x0F);
    a.h = p[6] & 0x80 != 0;
    Some((a, p[6] & 1 != 0))
}

/// Parses a frame without its FCS. None when the addresses are malformed or the frame is too short.
pub fn parse_frame(d: &[u8]) -> Option<Frame> {
    let n = d.len();
    let mut f = Frame::default();
    if n < 15 {
        return None;
    }
    let (to, last) = parse_addr(&d[..7])?;
    if last {
        return None;
    }
    f.to = to;
    let (from, mut last) = parse_addr(&d[7..14])?;
    f.from = from;
    let mut pos = 14;
    while !last {
        if f.npath as usize >= MAX_PATH || pos + 7 > n {
            return None;
        }
        let (a, l) = parse_addr(&d[pos..pos + 7])?;
        last = l;
        f.path[f.npath as usize] = a;
        f.npath += 1;
        pos += 7;
    }
    if pos >= n {
        return None;
    }
    f.control = i32::from(d[pos]);
    pos += 1;
    f.pid = -1;
    let c = f.control;
    let has_pid = c & 1 == 0 || (c & 0xEF) == 0x03; // I frames and UI frames
    if has_pid {
        if pos >= n {
            return None;
        }
        f.pid = i32::from(d[pos]);
        pos += 1;
    }
    f.info_off = pos;
    Some(f)
}

/// An address for the text builders: callsign bytes of any length, SSID and the H bit.
#[derive(Clone, Copy)]
pub struct AddrRef<'a> {
    /// Callsign without SSID.
    pub call: &'a [u8],
    /// SSID.
    pub ssid: i32,
    /// H bit.
    pub h: bool,
}

/// `CALL` or `CALL-SSID` (SSID 0 is left out).
pub fn put_addr(o: &mut Arena, a: &AddrRef) {
    o.put(a.call);
    if a.ssid != 0 {
        o.byte(b'-');
        put_int(o, a.ssid);
    }
}

/// Appends a decimal integer.
fn put_int(o: &mut Arena, v: i32) {
    let mut tmp = [0u8; 12];
    let mut i = tmp.len();
    let mut u = i64::from(v).unsigned_abs();
    loop {
        i -= 1;
        tmp[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if v < 0 {
        o.byte(b'-');
    }
    o.put(&tmp[i..]);
}

/// "WIDE1-1*,WIDE2-1": the digipeaters, a star marks the last one that has repeated the frame.
pub fn put_path(o: &mut Arena, path: &[AddrRef]) {
    let last_h = path.iter().rposition(|a| a.h);
    for (i, a) in path.iter().enumerate() {
        if i > 0 {
            o.byte(b',');
        }
        put_addr(o, a);
        if Some(i) == last_h {
            o.byte(b'*');
        }
    }
}

/// The TNC-2 monitor line `FROM>TO,PATH:info`.
pub fn put_tnc2(o: &mut Arena, from: &AddrRef, to: &AddrRef, path: &[AddrRef], info: &[u8]) {
    put_addr(o, from);
    o.byte(b'>');
    put_addr(o, to);
    let m = o.mark();
    o.byte(b',');
    put_path(o, path);
    if o.mark() == m + 1 {
        o.truncate(m); // an empty path text (digipeaters with empty callsigns) adds nothing, like the reference
    }
    o.byte(b':');
    o.put(info);
}

/// Frame type name from the control field: "I", "RR", "RNR", "REJ", "SREJ", "UI", "SABM", "SABME", "DISC", "DM", "UA", "FRMR", "XID", "TEST", "U"
/// (AX.25 v2.2 section 4.3).
pub fn type_name(control: u8) -> &'static str {
    if control & 1 == 0 {
        return "I";
    }
    if control & 3 == 1 {
        return ["RR", "RNR", "REJ", "SREJ"][usize::from((control >> 2) & 3)];
    }
    match control & 0xEF {
        0x03 => "UI",
        0x2F => "SABM",
        0x6F => "SABME",
        0x43 => "DISC",
        0x0F => "DM",
        0x63 => "UA",
        0x87 => "FRMR",
        0xAF => "XID",
        0xE3 => "TEST",
        _ => "U",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn addr(call: &[u8; 6], ssid: u8, last: bool) -> [u8; 7] {
        let mut o = [0u8; 7];
        for i in 0..6 {
            o[i] = call[i] << 1;
        }
        o[6] = 0x60 | (ssid << 1) | u8::from(last);
        o
    }
    #[test]
    fn parses_ui_frame() {
        let mut d = [0u8; 40];
        d[..7].copy_from_slice(&addr(b"APRS  ", 0, false));
        d[7..14].copy_from_slice(&addr(b"N0CALL", 9, false));
        d[14..21].copy_from_slice(&addr(b"WIDE1 ", 1, true));
        d[21] = 0x03;
        d[22] = 0xF0;
        d[23..27].copy_from_slice(b">hi!");
        let f = parse_frame(&d[..27]).unwrap();
        assert_eq!(&f.from.call[..6], b"N0CALL");
        assert_eq!((f.from.ssid, f.npath, f.pid, f.info_off), (9, 1, 0xF0, 23));
        assert_eq!(type_name(f.control as u8), "UI");
        let mut b = [0u8; 64];
        let mut o = Arena::new(&mut b);
        let fr = AddrRef { call: b"N0CALL", ssid: 9, h: false };
        let to = AddrRef { call: b"APRS", ssid: 0, h: false };
        let p = [AddrRef { call: b"WIDE1", ssid: 1, h: true }];
        put_tnc2(&mut o, &fr, &to, &p, b">hi!");
        let s = o.since(0);
        assert_eq!(&b[..s.len as usize], b"N0CALL-9>APRS,WIDE1-1*:>hi!");
    }
    #[test]
    fn fcs_vector() {
        let mut d = *b"123456789\0\0";
        let c = fcs(&d[..9]);
        d[9] = (c & 0xFF) as u8;
        d[10] = (c >> 8) as u8;
        assert_eq!(c, 0x906E);
        assert!(fcs_ok(&d));
        assert!(!fcs_ok(&d[..2]));
    }
}
