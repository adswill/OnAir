// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! MOT (Multimedia Object Transfer, ETSI EN 301 234) parsing shared by DAB and DRM: MOT time (clause 6.2.4) and its text, the header core and header extension
//! parameters (clause 6.2, with the slide show parameters of ETSI TS 101 499), the MSC data group framing (ETSI EN 300 401 clause 5.3.3.1), the directory
//! object (clause 7.1) and the content type names. Segment reassembly into objects and directories stays in C++.
use crate::crc::{crc16, tail16};
use onair_common::text::Buf;
use onair_dvbsi::charset;

/// MOT time (EN 301 234 clause 6.2.4).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct MotTime {
    /// Validity flag set.
    pub valid: u8,
    /// Validity flag clear: the time is "now".
    pub now: u8,
    /// Long form (seconds and milliseconds follow).
    pub long_form: u8,
    /// 17 bit Modified Julian Date.
    pub mjd: i32,
    /// Hours.
    pub hour: i32,
    /// Minutes.
    pub minute: i32,
    /// Seconds (long form).
    pub second: i32,
    /// Milliseconds (long form).
    pub ms: i32,
}

/// Parse a MOT time from `d`: returns the bytes used (4 short, 6 long, 0 when `d` is too short; the record then holds what could be read, like the reference).
pub fn parse_time(d: &[u8], t: &mut MotTime) -> usize {
    *t = MotTime::default();
    if d.len() < 4 {
        return 0;
    }
    let v = u32::from_be_bytes([d[0], d[1], d[2], d[3]]);
    t.valid = ((v >> 31) & 1) as u8;
    if t.valid == 0 {
        t.now = 1;
        return 4;
    }
    t.mjd = ((v >> 14) & 0x1FFFF) as i32;
    t.long_form = ((v >> 11) & 1) as u8;
    t.hour = ((v >> 6) & 0x1F) as i32;
    t.minute = (v & 0x3F) as i32;
    if t.long_form == 0 {
        return 4;
    }
    if d.len() < 6 {
        return 0;
    }
    let w = ((d[4] as u32) << 8) | d[5] as u32;
    t.second = ((w >> 10) & 0x3F) as i32;
    t.ms = (w & 0x3FF) as i32;
    6
}

/// MJD to the calendar (EN 300 401 annex B).
fn mjd_date(mjd: i32) -> (i32, i32, i32) {
    let j = mjd as i64 + 2400001 + 68569;
    let c = 4 * j / 146097;
    let mut e = j - (146097 * c + 3) / 4;
    let yy = 4000 * (e + 1) / 1461001;
    e = e - 1461 * yy / 4 + 31;
    let mm = 80 * e / 2447;
    let d = (e - 2447 * mm / 80) as i32;
    let k = mm / 11;
    ((100 * (c - 49) + yy + k) as i32, (mm + 2 - 12 * k) as i32, d)
}

/// `%0<w>d` of `v`.
fn dec_pad(o: &mut Buf, v: i32, w: usize) {
    let mut tmp = [0u8; 12];
    let mut t = 0;
    let mut a = (v as i64).unsigned_abs();
    loop {
        tmp[t] = b'0' + (a % 10) as u8;
        t += 1;
        a /= 10;
        if a == 0 {
            break;
        }
    }
    if v < 0 {
        o.byte(b'-');
    }
    let digits_w = if v < 0 { w.saturating_sub(1) } else { w };
    for _ in t..digits_w {
        o.byte(b'0');
    }
    while t > 0 {
        t -= 1;
        o.byte(tmp[t]);
    }
}

/// "now", "-" or "YYYY-MM-DD hh:mm[:ss]" into `out` (at most 64 bytes in the reference); returns the length.
pub fn time_text(t: &MotTime, out: &mut [u8]) -> usize {
    let mut o = Buf::new(out);
    if t.now != 0 {
        o.str("now");
    } else if t.valid == 0 {
        o.str("-");
    } else {
        let (y, m, d) = mjd_date(t.mjd);
        dec_pad(&mut o, y, 4);
        o.byte(b'-');
        dec_pad(&mut o, m, 2);
        o.byte(b'-');
        dec_pad(&mut o, d, 2);
        o.byte(b' ');
        dec_pad(&mut o, t.hour, 2);
        o.byte(b':');
        dec_pad(&mut o, t.minute, 2);
        if t.long_form != 0 {
            o.byte(b':');
            dec_pad(&mut o, t.second, 2);
        }
    }
    o.len()
}

/// A byte range of the header.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Range {
    /// First byte.
    pub off: u32,
    /// Number of bytes.
    pub len: u32,
}

/// The parsed header core and the parameters the receiver uses (strings are byte ranges of the header, the caption is UTF-8 in the caller's buffer).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct MotHeader {
    /// Body size from the header core, 0 = not stated.
    pub body_size: u64,
    /// Header size from the header core.
    pub header_size: u64,
    /// Content type.
    pub content_type: i32,
    /// Content subtype.
    pub subtype: i32,
    /// Number of parameters found (stored up to the array capacity).
    pub n_params: u32,
    /// ContentName (0x0C) present; `name` excludes the character set byte.
    pub has_name: u8,
    /// CategoryID / SlideID (0x25) present.
    pub has_slide: u8,
    /// TriggerTime (0x05) parsed.
    pub has_trigger: u8,
    /// ExpireTime (0x04) parsed.
    pub has_expire: u8,
    /// CategoryID.
    pub category_id: i32,
    /// SlideID.
    pub slide_id: i32,
    /// ContentName text.
    pub name: Range,
    /// CategoryTitle bytes.
    pub category: Range,
    /// ClickThroughURL bytes.
    pub click_url: Range,
    /// AlternativeLocationURL bytes.
    pub alt_url: Range,
    /// Bytes of the cleaned caption written to the caption buffer.
    pub caption_len: u32,
    /// TriggerTime.
    pub trigger: MotTime,
    /// ExpireTime.
    pub expire: MotTime,
}

/// One header extension parameter: id and data range.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct MotParam {
    /// ParamId.
    pub id: i32,
    /// Data bytes.
    pub data: Range,
}

/// Parse a MOT header (core of 7 bytes and extension); the header size field is honoured. `None` when shorter than 7 bytes. `caption` receives the cleaned
/// ContentDescription (3 bytes per input byte at most). Parameters past `params.len()` are counted.
pub fn parse_header(h: &[u8], params: &mut [MotParam], caption: &mut [u8]) -> Option<MotHeader> {
    let n = h.len();
    if n < 7 {
        return None;
    }
    let mut o = MotHeader {
        body_size: ((h[0] as u64) << 20) | ((h[1] as u64) << 12) | ((h[2] as u64) << 4) | (h[3] >> 4) as u64,
        header_size: (((h[3] & 0x0F) as u64) << 9) | ((h[4] as u64) << 1) | (h[5] >> 7) as u64,
        content_type: ((h[5] >> 1) & 0x3F) as i32,
        subtype: (((h[5] & 1) as i32) << 8) | h[6] as i32,
        category_id: -1,
        slide_id: -1,
        ..Default::default()
    };
    let end = if o.header_size >= 7 { (o.header_size.min(n as u64)) as usize } else { 7.min(n) };
    let mut p = 7usize;
    let mut np = 0usize;
    while p < end {
        let pli = h[p] >> 6;
        let id = (h[p] & 0x3F) as i32;
        p += 1;
        let mut len = match pli {
            1 => 1usize,
            2 => 4,
            _ => 0,
        };
        if pli == 3 {
            if p >= end {
                break;
            }
            if h[p] & 0x80 != 0 {
                if p + 1 >= end {
                    break;
                }
                len = (((h[p] & 0x7F) as usize) << 8) | h[p + 1] as usize;
                p += 2;
            } else {
                len = (h[p] & 0x7F) as usize;
                p += 1;
            }
        }
        if p + len > end {
            break;
        }
        if np < params.len() {
            params[np] = MotParam { id, data: Range { off: p as u32, len: len as u32 } };
        }
        np += 1;
        let d = &h[p..p + len];
        let rg = Range { off: p as u32, len: len as u32 };
        match id {
            0x0C if len >= 1 => {
                o.has_name = 1;
                o.name = Range { off: p as u32 + 1, len: len as u32 - 1 };
            }
            0x17 if len >= 1 => {
                let mut b = onair_dvbsi::charset::Out::new(caption);
                charset::dab_charset((d[0] >> 4) as i32, &d[1..], &mut b);
                let l = b.len();
                let buf = b.into_inner();
                o.caption_len = charset::clean_in_place(buf, l) as u32;
            }
            0x25 if len >= 2 => {
                o.has_slide = 1;
                o.category_id = d[0] as i32;
                o.slide_id = d[1] as i32;
            }
            0x26 => o.category = rg,
            0x27 => o.click_url = rg,
            0x28 => o.alt_url = rg,
            0x05 if len >= 4 => o.has_trigger = (parse_time(d, &mut o.trigger) > 0) as u8,
            0x04 if len >= 4 => o.has_expire = (parse_time(d, &mut o.expire) > 0) as u8,
            _ => {}
        }
        p += len;
    }
    o.n_params = np as u32;
    Some(o)
}

/// Content type name ("image/JPEG", "text/HTML", "general data" ...) of a type and subtype (EN 301 234 table 17).
pub fn content_type_name(typ: i32, sub: i32) -> &'static [u8] {
    match typ {
        0 => {
            if sub == 1 {
                b"MIME / HTTP header"
            } else {
                b"general data"
            }
        }
        1 => match sub {
            0 => b"text/ASCII",
            1 => b"text/ISO-8859-1",
            2 => b"text/HTML",
            _ => b"text",
        },
        2 => match sub {
            0 => b"image/GIF",
            1 => b"image/JPEG",
            2 => b"image/BMP",
            3 => b"image/PNG",
            _ => b"image",
        },
        3 => b"audio",
        4 => b"video",
        5 => b"MOT transport",
        6 => match sub {
            0 => b"MHEG",
            1 => b"Java",
            _ => b"system",
        },
        7 => b"application",
        8 => b"proprietary",
        _ => b"unknown",
    }
}

/// The group classes `parse_group` reports.
pub const GROUP_IGNORED: u8 = 0;
/// A group with a bad CRC or a bad structure.
pub const GROUP_BAD: u8 = 1;
/// A scrambled body group.
pub const GROUP_SCRAMBLED: u8 = 2;
/// A compressed directory group.
pub const GROUP_COMPRESSED: u8 = 3;
/// A header, body or directory segment.
pub const GROUP_SEGMENT: u8 = 4;

/// An MSC data group of the MOT types, reduced to what the reassembly needs.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct MotGroup {
    /// `GROUP_*`.
    pub status: u8,
    /// Last segment flag.
    pub last: u8,
    /// Data group type (3 header, 4 body, 6 directory).
    pub typ: i32,
    /// Segment number.
    pub seg_no: i32,
    /// Transport id.
    pub tid: i32,
    /// First byte of the segment data.
    pub off: u32,
    /// Length of the segment data.
    pub len: u32,
}

/// Parse an MSC data group (EN 300 401 clause 5.3.3.1): header, CRC check, session header (transport id) and the MOT segmentation header.
pub fn parse_group(d: &[u8]) -> MotGroup {
    let mut g = MotGroup::default();
    let mut n = d.len();
    if n < 3 {
        return g;
    }
    let (ext, crc, seg, ua) = (d[0] & 0x80 != 0, d[0] & 0x40 != 0, d[0] & 0x20 != 0, d[0] & 0x10 != 0);
    let typ = (d[0] & 0x0F) as i32;
    if !(3..=7).contains(&typ) {
        return g;
    }
    g.status = GROUP_BAD;
    if crc {
        if n < 5 || crc16(&d[..n - 2]) != tail16(&d[..n]) {
            return g;
        }
        n -= 2;
    }
    let mut p = 2 + if ext { 2 } else { 0 };
    let mut last = true;
    let mut seg_no = 0i32;
    if seg {
        if p + 2 > n {
            return g;
        }
        last = d[p] & 0x80 != 0;
        seg_no = (((d[p] & 0x7F) as i32) << 8) | d[p + 1] as i32;
        p += 2;
    }
    let mut tid = 0i32;
    if ua {
        if p + 1 > n {
            return g;
        }
        let has_tid = d[p] & 0x10 != 0;
        let li = (d[p] & 0x0F) as usize;
        p += 1;
        if p + li > n {
            return g;
        }
        if has_tid && li >= 2 {
            tid = ((d[p] as i32) << 8) | d[p + 1] as i32;
        }
        p += li;
    }
    if typ == 5 {
        g.status = GROUP_SCRAMBLED;
        return g;
    }
    if typ == 7 {
        g.status = GROUP_COMPRESSED;
        return g;
    }
    if p + 2 > n {
        return g;
    }
    let size = (((d[p] & 0x1F) as usize) << 8) | d[p + 1] as usize;
    p += 2;
    g.status = GROUP_SEGMENT;
    g.last = last as u8;
    g.typ = typ;
    g.seg_no = seg_no;
    g.tid = tid;
    g.off = p as u32;
    g.len = size.min(n - p) as u32;
    g
}

/// One object announced by a MOT directory: its transport id and where its header (core and extension) is.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DirEntry {
    /// Transport id.
    pub tid: i32,
    /// First byte of the header.
    pub off: u32,
    /// Header size.
    pub size: u32,
}

/// Walk a directory object (EN 301 234 clause 7.1): 32 bits compression flag and size, 16 bits number of objects, 24 bits carousel period, 16 bits segment
/// size, 16 bits extension length, the extension, then per object its transport id and header. Returns the number of entries found (`None` when the
/// directory is shorter than 13 bytes); entries past `out.len()` are counted, not stored.
pub fn parse_directory(dir: &[u8], out: &mut [DirEntry]) -> Option<usize> {
    let n = dir.len();
    if n < 13 {
        return None;
    }
    let count = ((dir[4] as usize) << 8) | dir[5] as usize;
    let ext_len = ((dir[11] as usize) << 8) | dir[12] as usize;
    let mut p = 13 + ext_len;
    let mut c = 0usize;
    let mut i = 0;
    while i < count && p + 2 + 7 <= n {
        let tid = ((dir[p] as i32) << 8) | dir[p + 1] as i32;
        let h = &dir[p + 2..];
        let hs = (((h[3] & 0x0F) as usize) << 9) | ((h[4] as usize) << 1) | (h[5] >> 7) as usize;
        if hs < 7 || p + 2 + hs > n {
            break;
        }
        if c < out.len() {
            out[c] = DirEntry { tid, off: (p + 2) as u32, size: hs as u32 };
        }
        c += 1;
        p += 2 + hs;
        i += 1;
    }
    Some(c)
}
