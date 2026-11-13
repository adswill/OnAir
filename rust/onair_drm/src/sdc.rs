// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The service description channel (ES 201 980 clause 6.4): the data field of entities (multiplex description, label, application information, time and
//! date, audio information, language and country) and the CRC-16 check of a whole SDC block. The parser works in place on a record that holds the previous
//! state, like the C++ `SdcInfo` it mirrors: entities only touch what they carry, so a second SDC of the same multiplex refines the first.
use crate::bits::BitReader;

/// Bytes of the string / byte fields of the records (an entity body is at most 127 bytes).
pub const FIELD: usize = 128;

/// Bytes of one logical frame of a stream in part A and part B.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdcStream {
    /// Part A bytes.
    pub len_a: i32,
    /// Part B bytes.
    pub len_b: i32,
}

/// Multiplex description entity (type 0).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdcMux {
    /// Present.
    pub present: u8,
    /// Protection level of part A.
    pub prot_a: i32,
    /// Protection level of part B.
    pub prot_b: i32,
    /// Number of streams.
    pub n_streams: i32,
    /// The streams.
    pub stream: [SdcStream; 4],
}

/// Audio information entity (type 9).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct SdcAudio {
    /// Present.
    pub present: u8,
    /// Text flag.
    pub text: u8,
    /// Enhancement flag.
    pub enhancement: u8,
    /// Short id.
    pub short_id: i32,
    /// Stream id.
    pub stream_id: i32,
    /// Audio coding (0 AAC, 3 xHE-AAC).
    pub coding: i32,
    /// SBR flag (AAC only).
    pub sbr: i32,
    /// Audio mode.
    pub mode: i32,
    /// Sampling rate code.
    pub rate_code: i32,
    /// MPEG Surround mode.
    pub mps: i32,
    /// Bytes of `config` used.
    pub n_config: u32,
    /// xHE-AAC static configuration.
    pub config: [u8; FIELD],
}

impl Default for SdcAudio {
    fn default() -> Self {
        SdcAudio {
            present: 0,
            text: 0,
            enhancement: 0,
            short_id: 0,
            stream_id: 0,
            coding: 0,
            sbr: 0,
            mode: 0,
            rate_code: 0,
            mps: 0,
            n_config: 0,
            config: [0; FIELD],
        }
    }
}

/// Label entity (type 1).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct SdcLabel {
    /// Present.
    pub present: u8,
    /// Short id.
    pub short_id: i32,
    /// Text control byte (0 = none).
    pub text_control: i32,
    /// Bytes of `text` used.
    pub n_text: u32,
    /// The label text as sent.
    pub text: [u8; FIELD],
}

impl Default for SdcLabel {
    fn default() -> Self {
        SdcLabel { present: 0, short_id: 0, text_control: 0, n_text: 0, text: [0; FIELD] }
    }
}

/// Language and country entity (type 12).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdcLang {
    /// Present.
    pub present: u8,
    /// Bytes of `language` used (0 or 3).
    pub n_language: u8,
    /// Bytes of `country` used (0 or 2).
    pub n_country: u8,
    /// Short id.
    pub short_id: i32,
    /// ISO 639-2 code.
    pub language: [u8; 3],
    /// ISO 3166 code.
    pub country: [u8; 2],
}

/// Time and date entity (type 8).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdcTime {
    /// Present.
    pub present: u8,
    /// The local time offset is present.
    pub has_offset: u8,
    /// Modified Julian Date.
    pub mjd: i32,
    /// Hours.
    pub hour: i32,
    /// Minutes.
    pub minute: i32,
    /// Offset in half hours.
    pub offset_half_hours: i32,
}

/// Application information entity (type 5).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct SdcApp {
    /// Present.
    pub present: u8,
    /// Packet mode.
    pub packet: u8,
    /// Packet mode: data units (not single packets).
    pub data_unit: u8,
    /// Short id.
    pub short_id: i32,
    /// Stream id.
    pub stream_id: i32,
    /// Application domain.
    pub domain: i32,
    /// Packet id.
    pub packet_id: i32,
    /// Packet data field length.
    pub packet_len: i32,
    /// Bytes of `app_data` used.
    pub n_app: u32,
    /// The application data bytes.
    pub app_data: [u8; FIELD],
}

impl Default for SdcApp {
    fn default() -> Self {
        SdcApp {
            present: 0,
            packet: 0,
            data_unit: 0,
            short_id: 0,
            stream_id: 0,
            domain: 0,
            packet_id: 0,
            packet_len: 0,
            n_app: 0,
            app_data: [0; FIELD],
        }
    }
}

/// Everything an SDC can say, in and out of `sdc_parse`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdcInfo {
    /// Entities read by the last call.
    pub entities: i32,
    /// Entities of unknown type appended to the `other` array by the last call.
    pub n_other: u32,
    /// Current multiplex description.
    pub mux: SdcMux,
    /// Announced (next) multiplex description.
    pub mux_next: SdcMux,
    /// Time and date.
    pub time: SdcTime,
    /// Audio by short id.
    pub audio: [SdcAudio; 4],
    /// Labels by short id.
    pub label: [SdcLabel; 4],
    /// Languages by short id.
    pub lang: [SdcLang; 4],
    /// Applications by short id.
    pub app: [SdcApp; 4],
}

/// Parse the data field of an SDC (`d`): returns the number of entities read. Entity types that are not interpreted are appended to `other` (as many as fit;
/// `info.n_other` counts the ones stored, so at most `other.len()`).
pub fn sdc_parse(d: &[u8], info: &mut SdcInfo, other: &mut [u8]) -> i32 {
    let bytes = d.len();
    let (mut pos, mut count) = (0usize, 0i32);
    let mut no = 0usize;
    while pos + 2 <= bytes {
        let len = (d[pos] >> 1) as usize;
        let ver = d[pos] & 1;
        let typ = d[pos + 1] >> 4;
        let nib = (d[pos + 1] & 15) as i32;
        if len == 0 && ver == 0 && typ == 0 && nib == 0 {
            break;
        }
        if pos + 2 + len > bytes {
            break;
        }
        let b = &d[pos + 2..pos + 2 + len];
        count += 1;
        if typ == 0 && len % 3 == 0 && (3..=12).contains(&len) {
            let mut m = SdcMux { present: 1, prot_a: (nib >> 2) & 3, prot_b: nib & 3, n_streams: (len / 3) as i32, ..Default::default() };
            for i in 0..len / 3 {
                m.stream[i].len_a = ((b[i * 3] as i32) << 4) | (b[i * 3 + 1] >> 4) as i32;
                m.stream[i].len_b = (((b[i * 3 + 1] & 15) as i32) << 8) | b[i * 3 + 2] as i32;
            }
            if ver != 0 {
                info.mux_next = m;
            } else {
                info.mux = m;
            }
        } else if typ == 1 {
            let id = ((nib >> 2) & 3) as usize;
            let l = &mut info.label[id];
            l.present = 1;
            l.short_id = id as i32;
            l.text_control = 0;
            l.n_text = 0;
            let mut start = 0;
            if len > 0 && (1..=15).contains(&b[0]) {
                l.text_control = b[0] as i32;
                start = 1;
            }
            for &c in &b[start..len] {
                if (l.n_text as usize) < FIELD {
                    l.text[l.n_text as usize] = c;
                    l.n_text += 1;
                }
            }
        } else if typ == 5 && len >= 1 && ver == 0 {
            let a = &mut info.app[((nib >> 2) & 3) as usize];
            a.present = 1;
            a.short_id = (nib >> 2) & 3;
            a.stream_id = nib & 3;
            a.packet = (b[0] & 0x80 != 0) as u8;
            a.domain = (b[0] & 7) as i32;
            let mut at = 1;
            if a.packet != 0 {
                a.data_unit = (b[0] & 0x40 != 0) as u8;
                a.packet_id = ((b[0] >> 4) & 3) as i32;
                a.packet_len = if len >= 2 { b[1] as i32 } else { 0 };
                at = 2;
            }
            a.n_app = 0;
            for &c in b.iter().take(len).skip(at) {
                if (a.n_app as usize) < FIELD {
                    a.app_data[a.n_app as usize] = c;
                    a.n_app += 1;
                }
            }
        } else if typ == 8 && len >= 3 {
            // the 4 bit nibble of the entity header, then up to 4 bytes: MJD (17), hours (5), minutes (6), and with a fourth byte the offset (2 + 1 + 5)
            let mut bits = [0u8; 40];
            let mut nb = 0;
            for k in (0..4).rev() {
                bits[nb] = ((nib >> k) & 1) as u8;
                nb += 1;
            }
            for &c in b.iter().take(4) {
                for k in (0..8).rev() {
                    bits[nb] = (c >> k) & 1;
                    nb += 1;
                }
            }
            let mut r = BitReader::new(&bits);
            let t = &mut info.time;
            t.present = 1;
            t.mjd = r.get(17) as i32;
            t.hour = r.get(5) as i32;
            t.minute = r.get(6) as i32;
            t.has_offset = (len >= 4) as u8;
            if t.has_offset != 0 {
                r.get(2);
                let sense = r.get(1);
                let v = r.get(5) as i32;
                t.offset_half_hours = if sense != 0 { -v } else { v };
            }
        } else if typ == 9 && len >= 2 && ver == 0 {
            let mut a = SdcAudio {
                present: 1,
                short_id: (nib >> 2) & 3,
                stream_id: nib & 3,
                coding: (b[0] >> 6) as i32,
                sbr: ((b[0] >> 5) & 1) as i32,
                mode: ((b[0] >> 3) & 3) as i32,
                rate_code: (b[0] & 7) as i32,
                text: (b[1] & 0x80 != 0) as u8,
                enhancement: (b[1] & 0x40 != 0) as u8,
                mps: ((b[1] >> 3) & 7) as i32,
                ..Default::default()
            };
            if a.coding == 3 {
                a.sbr = 0;
            }
            for &c in &b[2..len] {
                if (a.n_config as usize) < FIELD {
                    a.config[a.n_config as usize] = c;
                    a.n_config += 1;
                }
            }
            info.audio[a.short_id as usize] = a;
        } else if typ == 12 && len >= 5 && ver == 0 {
            let l = &mut info.lang[((nib >> 2) & 3) as usize];
            l.present = 1;
            l.short_id = (nib >> 2) & 3;
            l.language.copy_from_slice(&b[..3]);
            l.n_language = 3;
            l.country.copy_from_slice(&b[3..5]);
            l.n_country = 2;
        } else {
            if no < other.len() {
                other[no] = typ;
                no += 1;
            }
        }
        pos += 2 + len;
    }
    info.entities = count;
    info.n_other = no as u32;
    count
}

/// CRC check of a decoded SDC block (`bits`, one bit per byte, at least 4 + 8 * `field_bytes` + 16 of them): the CRC-16 covers the AFS index (as 8 bits) and the
/// data field. Returns the AFS index (the first 4 bits) and whether the CRC is right.
pub fn sdc_check_bits(bits: &[u8], field_bytes: usize) -> (i32, bool) {
    let bit = |i: usize| (bits.get(i).copied().unwrap_or(0) & 1) as u32;
    let mut afs = 0i32;
    for i in 0..4 {
        afs = (afs << 1) | bit(i) as i32;
    }
    let n = 4 + 8 * field_bytes;
    let mut r: u32 = 0xFFFF;
    let mut feed = |x: u32| {
        let fb = ((r >> 15) & 1) ^ x;
        r = (r << 1) & 0xFFFF;
        if fb != 0 {
            r ^= 0x1021;
        }
    };
    for _ in 0..4 {
        feed(0);
    }
    for i in 0..n {
        feed(bit(i));
    }
    let mut rx = 0u32;
    for j in 0..16 {
        rx = (rx << 1) | bit(n + j);
    }
    (afs, (r ^ 0xFFFF) & 0xFFFF == rx)
}
