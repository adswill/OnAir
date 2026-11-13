// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! SigMF meta files (https://sigmf.org, `.sigmf-meta`): reading what OnAir needs to play a recording (data type, sample rate, centre frequency, where the samples
//! start, the end trimmed by `core:trailing_bytes`, annotation count, description, data file name) and the two writers (the recorder's and the test tool's).
//! Finding the data file and its size is file system work and stays in C++ (`core/src/source/iq_file.cpp`).
use crate::cfmt::{put_f1, put_g, Sink};
use crate::json::{self, Kind, Reader, Val};
use crate::Span;

/// Parse result: `status` 0 = ok, 1 = not a valid SigMF meta file, 2 = no `global` object, 3 = no string `core:datatype`, 4 = a data type OnAir cannot play (its text is `datatype`).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct SigmfMeta {
    /// Outcome, see above.
    pub status: i32,
    /// 0 CS8, 1 CU8, 2 CF32, 3 CI16, 4 CU16 (the order of `FileFormat`).
    pub format: i32,
    /// Samples are big endian.
    pub big_endian: i32,
    /// Number of annotations.
    pub annotations: i32,
    /// `core:sample_rate` (global, else the first capture's), 0 when missing.
    pub sample_rate: f64,
    /// First capture's `core:frequency`, else the middle of the first annotation with both edges, else 0.
    pub center_hz: f64,
    /// First capture's `core:sample_start` (0 when missing or not positive).
    pub sample_start: u64,
    /// `core:header_bytes` (0 when missing or negative).
    pub header_bytes: u64,
    /// `core:trailing_bytes`.
    pub trailing_bytes: u64,
    /// Raw (escaped) range of `core:description` in the text; len 0 when absent. Resolve with `onair_files_json_unescape`.
    pub description: Span,
    /// Raw range of the global `core:dataset`; len 0 when absent.
    pub dataset: Span,
    /// Raw range of `core:datatype` (set from status 3 on... when present).
    pub datatype: Span,
}

/// `(uint64_t)max(0, v)` for a double: saturating, NaN is 0.
fn to_u64(v: f64) -> u64 {
    if v > 0.0 {
        if v >= 18446744073709551616.0 {
            u64::MAX
        } else {
            v as u64
        }
    } else {
        0
    }
}

fn num_of(v: &Option<Val>) -> f64 {
    match v {
        Some(x) if x.kind == Kind::Num => x.num,
        _ => 0.0,
    }
}

fn span_of(v: &Option<Val>) -> Span {
    match v {
        Some(x) if x.kind == Kind::Str => Span { off: x.start as u32, len: (x.end - x.start) as u32 },
        _ => Span::default(),
    }
}

/// Splits a SigMF data type ("cf32_le", "ci16_be", "ru8"...) into (format, big endian), or None for an unsupported one. Case is ignored.
pub fn datatype(dt: &[u8]) -> Option<(i32, bool)> {
    let mut buf = [0u8; 16];
    if dt.len() > buf.len() {
        return None;
    }
    for (i, &c) in dt.iter().enumerate() {
        buf[i] = c.to_ascii_lowercase();
    }
    let mut d = &buf[..dt.len()];
    let mut be = false;
    if d.len() > 3 && d.ends_with(b"_be") {
        be = true;
        d = &d[..d.len() - 3];
    } else if d.len() > 3 && d.ends_with(b"_le") {
        d = &d[..d.len() - 3];
    }
    let f = match d {
        b"cf32" => 2,
        b"ci16" | b"ri16" => 3,
        b"cu16" => 4,
        b"ci8" | b"ri8" => 0,
        b"cu8" | b"ru8" => 1,
        _ => return None,
    };
    if f == 0 || f == 1 {
        be = false;
    }
    Some((f, be))
}

/// Reads a `.sigmf-meta` text.
pub fn parse(text: &[u8]) -> SigmfMeta {
    let mut m = SigmfMeta::default();
    let mut rd = Reader::new(text);
    let root = rd.val();
    if rd.bad || root.kind != Kind::Obj {
        m.status = 1;
        return m;
    }
    let g = match json::get(text, &root, b"global") {
        Some(g) if g.kind == Kind::Obj => g,
        _ => {
            m.status = 2;
            return m;
        }
    };
    let dt = json::get(text, &g, b"core:datatype");
    let dts = match dt {
        Some(d) if d.kind == Kind::Str => d,
        _ => {
            m.status = 3;
            return m;
        }
    };
    m.datatype = span_of(&dt);
    let mut raw = [0u8; 40];
    let n = json::unescape(&text[dts.start..dts.end], &mut raw);
    let parsed = if n <= raw.len() { datatype(&raw[..n]) } else { None };
    match parsed {
        Some((f, be)) => {
            m.format = f;
            m.big_endian = be as i32;
        }
        None => {
            m.status = 4;
            return m;
        }
    }
    m.sample_rate = num_of(&json::get(text, &g, b"core:sample_rate"));
    m.description = span_of(&json::get(text, &g, b"core:description"));
    m.dataset = span_of(&json::get(text, &g, b"core:dataset"));
    m.header_bytes = to_u64(num_of(&json::get(text, &g, b"core:header_bytes")));
    m.trailing_bytes = to_u64(num_of(&json::get(text, &g, b"core:trailing_bytes")));
    if let Some(caps) = json::get(text, &root, b"captures") {
        let (first, _) = json::elem(text, &caps, 0);
        if let Some(c) = first {
            if c.kind == Kind::Obj {
                m.center_hz = num_of(&json::get(text, &c, b"core:frequency"));
                m.sample_start = to_u64(num_of(&json::get(text, &c, b"core:sample_start")));
                if m.sample_rate <= 0.0 {
                    m.sample_rate = num_of(&json::get(text, &c, b"core:sample_rate"));
                }
            }
        }
    }
    if let Some(an) = json::get(text, &root, b"annotations") {
        if an.kind == Kind::Arr {
            let need_center = m.center_hz <= 0.0;
            let mut center = m.center_hz;
            let mut found = false;
            let count = json::each_elem(text, &an, |_, a| {
                if need_center && !found {
                    // no capture frequency: the middle of the first annotated band
                    let lo = num_of(&json::get(text, a, b"core:freq_lower_edge"));
                    let hi = num_of(&json::get(text, a, b"core:freq_upper_edge"));
                    if lo > 0.0 && hi > lo {
                        center = (lo + hi) / 2.0;
                        found = true;
                    }
                }
            });
            m.annotations = count as i32;
            m.center_hz = center;
        }
    }
    m
}

fn finite(v: f64) -> f64 {
    if v.is_finite() {
        v
    } else {
        0.0
    }
}

/// The meta file the test tool writes (`writeSigmfMeta`). `description`: quotes and backslashes are escaped, control characters below 32 are dropped. Returns the
/// length of the text; bytes beyond `out.len()` are not written but counted.
pub fn write_tool_meta(datatype: &[u8], rate: f64, center: f64, sample_start: u64, description: &[u8], out: &mut [u8]) -> usize {
    use core::fmt::Write;
    let mut s = Sink::new(out);
    s.put(b"{\n \"global\": {\"core:datatype\": \"");
    s.put(datatype);
    s.put(b"\", \"core:sample_rate\": ");
    put_g(&mut s, finite(rate), 9);
    s.put(b", \"core:version\": \"1.0.0\", \"core:description\": \"");
    for &c in description {
        if c == b'"' || c == b'\\' {
            s.put(b"\\");
        }
        if c >= 32 {
            s.put(&[c]);
        }
    }
    s.put(b"\", \"core:recorder\": \"OnAir\"},\n \"captures\": [{\"core:sample_start\": ");
    let _ = write!(s, "{}", sample_start);
    s.put(b", \"core:frequency\": ");
    put_g(&mut s, finite(center), 9);
    s.put(b"}],\n \"annotations\": []\n}\n");
    s.len
}

/// A JSON string literal: quotes and backslashes escaped, control characters as `\u00xx`, other bytes as they are.
fn put_json_str(s: &mut Sink, v: &[u8]) {
    use core::fmt::Write;
    s.put(b"\"");
    for &c in v {
        if c == b'"' || c == b'\\' {
            s.put(b"\\");
            s.put(&[c]);
        } else if c < 0x20 {
            let _ = write!(s, "\\u{:04x}", c);
        } else {
            s.put(&[c]);
        }
    }
    s.put(b"\"");
}

#[allow(clippy::too_many_arguments)]
/// The meta file the recorder writes (`sigmfMetaJson`). Empty `dataset`, `datetime` and `description` and a zero `sample_count` are left out, so is a centre
/// frequency that is not positive or is 1e18 or more. Returns the text length (counted beyond `out.len()` too).
pub fn write_recorder_meta(
    datatype: &[u8],
    rate: f64,
    center: f64,
    datetime: &[u8],
    dataset: &[u8],
    sample_count: u64,
    description: &[u8],
    out: &mut [u8],
) -> usize {
    use core::fmt::Write;
    let mut s = Sink::new(out);
    s.put(b"{\n  \"global\": {\n    \"core:datatype\": \"");
    s.put(datatype);
    s.put(b"\",\n    \"core:sample_rate\": ");
    put_g(&mut s, finite(rate), 10);
    s.put(b",\n    \"core:version\": \"1.0.0\",\n");
    if sample_count != 0 {
        let _ = writeln!(s, "    \"core:sample_count\": {},", sample_count);
    }
    if !dataset.is_empty() {
        s.put(b"    \"core:dataset\": ");
        put_json_str(&mut s, dataset);
        s.put(b",\n");
    }
    if !description.is_empty() {
        s.put(b"    \"core:description\": ");
        put_json_str(&mut s, description);
        s.put(b",\n");
    }
    s.put(b"    \"core:recorder\": \"OnAir\"\n  },\n  \"captures\": [\n    {\n      \"core:sample_start\": 0");
    let c = finite(center);
    if c > 0.0 && c < 1e18 {
        s.put(b",\n      \"core:frequency\": ");
        put_f1(&mut s, c);
    }
    if !datetime.is_empty() {
        s.put(b",\n      \"core:datetime\": ");
        put_json_str(&mut s, datetime);
    }
    s.put(b"\n    }\n  ],\n  \"annotations\": []\n}\n");
    s.len
}
