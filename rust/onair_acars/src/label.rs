// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Label names and the short reading of fixed-format text. Names are those the decoders' own message formats support (acarsdec label.c reads fields
//! from these labels) plus the service labels of ARINC 618; the full ARINC 620 list is not public and is not reproduced.
use crate::text::Buf;

/// Meaning of a label (two characters, the DEL of "_<DEL>" written as 'd'); "" when the label is not in the list. The string is NUL-terminated
/// (so the C ABI can hand out the pointer): [`text::strip_nul`](crate::text::strip_nul) gives the plain text.
pub fn label_text(a: u8, b: u8) -> &'static str {
    LABELS.iter().find(|l| l.0 == a && l.1 == b).map_or("\0", |l| l.2)
}

static LABELS: &[(u8, u8, &str)] = &[
    (b'_', b'd', "Command or response\0"),
    (b'Q', b'0', "Link test\0"),
    (b'S', b'Q', "Ground station squitter\0"),
    (b'S', b'A', "Media advisory\0"),
    (b'H', b'1', "Terminal message\0"),
    (b'5', b'Z', "Airline defined downlink\0"),
    (b'Q', b'1', "OOOI report (out, off, on, in)\0"),
    (b'Q', b'2', "ETA report\0"),
    (b'Q', b'A', "Gate out\0"),
    (b'Q', b'B', "Wheels off\0"),
    (b'Q', b'C', "Wheels on\0"),
    (b'Q', b'D', "Gate in\0"),
    (b'Q', b'E', "Gate out, destination\0"),
    (b'Q', b'F', "Wheels off, destination\0"),
    (b'Q', b'G', "Gate out and in\0"),
    (b'Q', b'H', "Gate out\0"),
    (b'Q', b'K', "Wheels on, destination\0"),
    (b'Q', b'L', "Gate in, origin and destination\0"),
    (b'Q', b'M', "Origin and destination\0"),
    (b'Q', b'N', "Destination and ETA\0"),
    (b'Q', b'P', "Gate out, origin and destination\0"),
    (b'Q', b'Q', "Wheels off, origin and destination\0"),
    (b'Q', b'R', "Wheels on, origin and destination\0"),
    (b'Q', b'S', "Gate in, origin and destination\0"),
    (b'Q', b'T', "Gate out and in, origin and destination\0"),
    (b'1', b'0', "Arrival report\0"),
    (b'1', b'5', "Flight status report\0"),
    (b'1', b'7', "ETA report\0"),
    (b'2', b'N', "Takeoff report\0"),
    (b'4', b'4', "Position or ETA report\0"),
];

/// Field layout of the Q labels (acarsdec label.c): which 4 characters of the text hold what.
/// 'o' origin, 'd' destination, 'O' out, 'F' off, 'N' on, 'I' in, 'E' ETA.
static Q_LAYOUT: &[(u8, &[(u8, usize)])] = &[
    (b'1', &[(b'o', 0), (b'O', 4), (b'F', 8), (b'N', 12), (b'I', 16), (b'd', 24)]),
    (b'2', &[(b'o', 0), (b'E', 4)]),
    (b'A', &[(b'o', 0), (b'O', 4)]),
    (b'B', &[(b'o', 0), (b'F', 4)]),
    (b'C', &[(b'o', 0), (b'N', 4)]),
    (b'D', &[(b'o', 0), (b'I', 4)]),
    (b'E', &[(b'o', 0), (b'O', 4), (b'd', 8)]),
    (b'F', &[(b'o', 0), (b'F', 4), (b'd', 8)]),
    (b'G', &[(b'o', 0), (b'O', 4), (b'I', 8)]),
    (b'H', &[(b'o', 0), (b'O', 4)]),
    (b'K', &[(b'o', 0), (b'N', 4), (b'd', 8)]),
    (b'L', &[(b'd', 0), (b'I', 8), (b'o', 13)]),
    (b'M', &[(b'd', 0), (b'o', 8)]),
    (b'N', &[(b'd', 4), (b'E', 8)]),
    (b'P', &[(b'o', 0), (b'd', 4), (b'O', 8)]),
    (b'Q', &[(b'o', 0), (b'd', 4), (b'F', 8)]),
    (b'R', &[(b'o', 0), (b'd', 4), (b'N', 8)]),
    (b'S', &[(b'o', 0), (b'd', 4), (b'I', 8)]),
    (b'T', &[(b'o', 0), (b'd', 4), (b'O', 8), (b'I', 12)]),
];

fn is_time(s: &[u8], o: usize) -> bool {
    if o + 4 > s.len() || !s[o..o + 4].iter().all(u8::is_ascii_digit) {
        return false;
    }
    // hhmm: hours below 24, tens of minutes below 6
    (s[o] - b'0') * 10 + (s[o + 1] - b'0') < 24 && (s[o + 2] - b'0') < 6
}

fn is_airport(s: &[u8], o: usize) -> bool {
    o + 4 <= s.len() && s[o..o + 4].iter().all(u8::is_ascii_uppercase)
}

/// Media advisory link name (libacars media-adv.c).
fn link_name(c: u8) -> Option<&'static str> {
    Some(match c {
        b'V' => "VHF ACARS",
        b'S' => "default SATCOM",
        b'H' => "HF",
        b'G' => "Globalstar",
        b'C' => "ICO SATCOM",
        b'2' => "VDL2",
        b'X' => "Inmarsat Aero",
        b'I' => "Iridium",
        _ => return None,
    })
}

/// Media advisory (label SA), libacars media-adv.c: version 0, state E or L, current link, hhmmss, available links, optional "/text".
fn media_advisory(t: &[u8], out: &mut Buf) {
    if t.len() < 10 || t[0] != b'0' || (t[1] != b'E' && t[1] != b'L') {
        return;
    }
    let Some(cur) = link_name(t[2]) else { return };
    if !t[3..9].iter().all(u8::is_ascii_digit) {
        return;
    }
    let two = |i: usize| ((t[i] - b'0') * 10 + (t[i + 1] - b'0')) as u32;
    let (h, m, s) = (two(3), two(5), two(7));
    if h > 23 || m > 59 || s > 59 {
        return;
    }
    // every link before the '/' has to be known
    let end = t[9..].iter().position(|&c| c == b'/').map_or(t.len(), |p| p + 9);
    if t[9..end].iter().any(|&c| link_name(c).is_none()) {
        return;
    }
    out.str(if t[1] == b'E' { "link established: " } else { "link lost: " });
    out.str(cur);
    out.str(" at ");
    out.dec2(h);
    out.byte(b':');
    out.dec2(m);
    out.byte(b':');
    out.dec2(s);
    out.str(", available: ");
    for (k, &c) in t[9..end].iter().enumerate() {
        if k > 0 {
            out.str(", ");
        }
        out.str(link_name(c).unwrap_or(""));
    }
}

/// Short reading of fixed-format text: the OOOI times and airports of the Q labels and the media advisory (SA); nothing written when the label
/// or the text does not fit.
pub fn decode_text(label: &[u8], text: &[u8], out: &mut Buf) {
    if label == b"SA" {
        return media_advisory(text, out);
    }
    if label.len() != 2 || label[0] != b'Q' {
        return;
    }
    let Some((_, fields)) = Q_LAYOUT.iter().find(|q| q.0 == label[1]) else { return };
    // check every field first: a text that does not fit gives nothing, not half a reading
    for &(kind, off) in fields.iter() {
        let ok = if kind == b'o' || kind == b'd' { is_airport(text, off) } else { is_time(text, off) };
        if !ok {
            return;
        }
    }
    for (n, &(kind, off)) in fields.iter().enumerate() {
        let name = match kind {
            b'o' => "from",
            b'd' => "to",
            b'O' => "out",
            b'F' => "off",
            b'N' => "on",
            b'I' => "in",
            _ => "eta",
        };
        if n > 0 {
            out.byte(b' ');
        }
        out.str(name);
        out.byte(b' ');
        out.bytes(&text[off..off + 4]);
    }
}
