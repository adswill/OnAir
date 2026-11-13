// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DAB programme associated data (ETSI EN 300 401 clause 7.4 and 5.3.3, ETSI TS 102 563 clause 5.2, ETSI TS 102 980 DL Plus): the PAD of a DAB+ access unit
//! (data stream element) and of an MP2 frame, the F-PAD / X-PAD split into application subfields, the data group length indicator, the dynamic label segment
//! framing and the DL Plus tag command. The data group, dynamic label and slide show assembly state stays in C++.
use crate::crc::{crc16, tail16};

/// X-PAD subfield length by length index (EN 300 401 table 30).
pub const SUBFIELD_LEN: [i32; 8] = [4, 6, 8, 12, 16, 24, 32, 48];

/// The PAD of one audio frame: the X-PAD bytes in the order they were sent and the F-PAD bytes.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PadBytes {
    /// Number of X-PAD bytes written.
    pub xlen: u32,
    /// The two F-PAD bytes.
    pub fpad: [u8; 2],
}

/// The PAD of a DAB+ access unit: the data stream element (id_syn_ele 4) that comes first when there is PAD, with the X-PAD bytes reversed into the order they
/// were sent (up to 508 bytes needed in `xpad`). `None` when the unit carries no PAD.
pub fn pad_from_au(au: &[u8], xpad: &mut [u8]) -> Option<PadBytes> {
    let n = au.len();
    if n < 4 || (au[0] >> 5) != 4 {
        return None;
    }
    let (mut pos, mut cnt) = (2usize, au[1] as usize);
    if cnt == 255 {
        cnt += au[2] as usize;
        pos = 3;
    }
    if cnt < 2 || pos + cnt > n {
        return None;
    }
    let d = &au[pos..pos + cnt];
    for k in 0..cnt - 2 {
        if k < xpad.len() {
            xpad[k] = d[cnt - 3 - k];
        }
    }
    Some(PadBytes { xlen: (cnt - 2) as u32, fpad: [d[cnt - 2], d[cnt - 1]] })
}

/// The PAD of an MP2 frame (its last bytes: F-PAD last, X-PAD in front). `last_cont` is the length of the last data subfield, kept between frames for the
/// variable size X-PAD without content indicators. `None` when the frame has no usable PAD. `xpad` needs 4 + 4 * 48 bytes.
pub fn pad_from_mp2(f: &[u8], last_cont: &mut i32, xpad: &mut [u8]) -> Option<PadBytes> {
    let len = f.len();
    if len < 8 {
        return None;
    }
    let fpad = [f[len - 2], f[len - 1]];
    if (fpad[0] >> 6) != 0 {
        return None;
    }
    let ind = (fpad[0] >> 4) & 3;
    let ci = fpad[1] & 2 != 0;
    let mut xlen = 0i32;
    if ind == 1 {
        xlen = 4;
        if ci {
            *last_cont = 3;
        }
    } else if ind == 2 {
        if ci {
            let (mut i, mut tot) = (0usize, 0i32);
            while i < 4 && i < len - 3 {
                let t = f[len - 3 - i] & 0x1F;
                if t == 0 {
                    i += 1;
                    break;
                }
                *last_cont = SUBFIELD_LEN[(f[len - 3 - i] >> 5) as usize];
                tot += *last_cont;
                i += 1;
            }
            xlen = i as i32 + tot;
        } else {
            xlen = *last_cont;
        }
    }
    xlen = xlen.min(len as i32 - 6);
    if xlen <= 0 {
        return None;
    }
    for k in 0..xlen as usize {
        if k < xpad.len() {
            xpad[k] = f[len - 3 - k];
        }
    }
    Some(PadBytes { xlen: xlen as u32, fpad })
}

/// One application subfield of an X-PAD: application type and the byte range in the X-PAD.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PadSub {
    /// X-PAD application type (1 data group length indicator, 2 / 3 dynamic label, 12 / 13 data group).
    pub typ: i32,
    /// First byte in the X-PAD.
    pub off: u32,
    /// Length (clamped to the X-PAD).
    pub len: u32,
}

/// Split one X-PAD into its subfields (EN 300 401 clause 7.4.2). `last_type` is the application type of the last content indicator, kept between frames for
/// X-PAD without one. At most 5 subfields are written to `out`; returns the number.
pub fn pad_split(xp: &[u8], fpad: [u8; 2], last_type: &mut i32, out: &mut [PadSub]) -> usize {
    let mut xlen = xp.len() as i32;
    if (fpad[0] >> 6) != 0 {
        return 0;
    }
    let ind = (fpad[0] >> 4) & 3;
    let ci = fpad[1] & 2 != 0;
    if (ind != 1 && ind != 2) || xlen <= 0 {
        return 0;
    }
    if ind == 1 {
        xlen = xlen.min(4);
    }
    let mut subs = [(0i32, 0i32); 5];
    let mut ns = 0usize;
    let mut pos = 0i32;
    if ci {
        if ind == 1 {
            subs[0] = ((xp[0] & 0x1F) as i32, 3);
            ns = 1;
            pos = 1;
        } else {
            let mut i = 0usize;
            while i < 4 && (i as i32) < xlen {
                let t = (xp[i] & 0x1F) as i32;
                if t == 0 {
                    i += 1;
                    break;
                }
                subs[ns] = (t, SUBFIELD_LEN[(xp[i] >> 5) as usize]);
                ns += 1;
                i += 1;
            }
            pos = i as i32;
        }
        if ns > 0 {
            *last_type = subs[ns - 1].0;
        }
    } else {
        let t = if *last_type == 12 {
            13
        } else if *last_type == 2 {
            3
        } else {
            *last_type
        };
        if t == 0 {
            return 0;
        }
        subs[0] = (t, xlen);
        ns = 1;
    }
    let mut c = 0usize;
    for &(typ, l) in subs.iter().take(ns) {
        let len = l.min(xlen - pos);
        if len <= 0 {
            break;
        }
        if c < out.len() {
            out[c] = PadSub { typ, off: pos as u32, len: len as u32 };
        }
        c += 1;
        pos += len;
    }
    c
}

/// The data group length indicator (X-PAD application type 1): 0 = too short (ignored), 1 = bad CRC, 2 = good; `len` receives the 14 bit length.
pub fn dg_length_indicator(d: &[u8], len: &mut i32) -> u8 {
    if d.len() < 2 {
        return 0;
    }
    if d.len() >= 4 && crc16(&d[..2]) != ((d[2] as u16) << 8 | d[3] as u16) {
        return 1;
    }
    *len = (((d[0] & 0x3F) as i32) << 8) | d[1] as i32;
    2
}

/// What a dynamic label subfield collector should do next (TS 102 980 / EN 300 401 clause 7.4.5.2).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DlsStep {
    /// 0 keep collecting, 1 stop (no segment), 2 stop with a segment of `seg_len` bytes (CRC removed).
    pub action: u8,
    /// Bytes of the complete segment without its CRC.
    pub seg_len: u32,
}

/// Look at the bytes of a dynamic label segment collected so far: a label segment has a 2 byte prefix, 1 to 16 characters and a CRC-16; a command has no data
/// (clear display) or a few bytes (DL Plus), its length being the one the CRC agrees with.
pub fn dls_step(dls: &[u8]) -> DlsStep {
    let n = dls.len();
    if n < 2 {
        return DlsStep::default();
    }
    let crc_ok = |total: usize| n >= total && crc16(&dls[..total - 2]) == tail16(&dls[..total]);
    if dls[0] & 0x10 == 0 {
        let total = 2 + (dls[0] & 0x0F) as usize + 1 + 2;
        if n >= total {
            return if crc_ok(total) { DlsStep { action: 2, seg_len: (total - 2) as u32 } } else { DlsStep { action: 1, seg_len: 0 } };
        }
        return DlsStep::default();
    }
    let most = if dls[0] & 0x0F == 1 { 0 } else { 16 };
    for l in 0..=most {
        let total = 2 + l + 2;
        if crc_ok(total) {
            return DlsStep { action: 2, seg_len: (total - 2) as u32 };
        }
    }
    if n >= 2 + most + 2 {
        return DlsStep { action: 1, seg_len: 0 };
    }
    DlsStep::default()
}

/// A dynamic label segment without its CRC: prefix flags and the second byte field.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DlSegment {
    /// Toggle bit.
    pub toggle: i32,
    /// First segment flag.
    pub first: u8,
    /// Last segment flag.
    pub last: u8,
    /// Command flag.
    pub cmd: u8,
    /// Command code (low 4 bits of the first byte).
    pub code: i32,
    /// Segment number (0 for the first segment).
    pub seg_no: i32,
    /// Character set (first segment; else the same nibble of byte 1).
    pub charset: i32,
}

/// Decode the 2 byte prefix of a label segment.
pub fn dl_segment(d: &[u8]) -> DlSegment {
    let b0 = d.first().copied().unwrap_or(0);
    let b1 = d.get(1).copied().unwrap_or(0);
    let first = b0 & 0x40 != 0;
    DlSegment {
        toggle: (b0 >> 7) as i32,
        first: first as u8,
        last: (b0 & 0x20 != 0) as u8,
        cmd: (b0 & 0x10 != 0) as u8,
        code: (b0 & 0x0F) as i32,
        seg_no: if first { 0 } else { ((b1 >> 4) & 7) as i32 },
        charset: (b1 >> 4) as i32,
    }
}

/// One DL Plus tag: content type, start and length in characters of the label.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DlpTag {
    /// Content type (TS 102 980 table 2).
    pub typ: i32,
    /// First character.
    pub start: i32,
    /// Number of characters.
    pub len: i32,
}

/// The DL Plus tags command (TS 102 980 clause 5.3): `d` follows the 2 byte prefix. Returns (item running, number of tags written); `None` when too short.
pub fn dlplus_tags(d: &[u8], out: &mut [DlpTag; 4]) -> Option<(u8, usize)> {
    let n = d.len();
    if n < 1 {
        return None;
    }
    let nt = ((d[0] & 3) + 1) as usize;
    if n < 1 + 3 * nt {
        return None;
    }
    for i in 0..nt {
        out[i] = DlpTag { typ: (d[1 + 3 * i] & 0x7F) as i32, start: (d[2 + 3 * i] & 0x7F) as i32, len: (d[3 + 3 * i] & 0x7F) as i32 + 1 };
    }
    Some(((d[0] >> 2) & 1, nt))
}
