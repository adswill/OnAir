// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! APRS information field parser, after APRS 1.0.1 (APRS101.pdf) chapters 5 to 12 and the 1.2 addenda: uncompressed and compressed positions, Mic-E,
//! objects, items, status, messages (with ack / reject and message numbers), weather, telemetry, raw GPS, capabilities, queries, third-party traffic.
//! The Mic-E longitude and speed rules were cross-checked against the description in Dire Wolf's decode_aprs.c - no code was copied.
//! Text results (names, comment, summary...) go into a caller buffer through `Arena` and are referenced by `Span`.
use crate::aprs_pos::{mic_e, parse_pos};
use crate::aprs_wx::{parse_weather, put_summary, Weather};
use crate::arena::{Arena, Span};

/// Packet type ids (index into `TYPE_NAMES`; 0 = not set).
pub const T_NONE: i32 = 0;
/// Position report.
pub const T_POSITION: i32 = 1;
/// Mic-E.
pub const T_MIC_E: i32 = 2;
/// Object.
pub const T_OBJECT: i32 = 3;
/// Item.
pub const T_ITEM: i32 = 4;
/// Status.
pub const T_STATUS: i32 = 5;
/// Message.
pub const T_MESSAGE: i32 = 6;
/// Weather.
pub const T_WEATHER: i32 = 7;
/// Telemetry.
pub const T_TELEMETRY: i32 = 8;
/// Raw GPS.
pub const T_RAW_GPS: i32 = 9;
/// Capabilities.
pub const T_CAPABILITIES: i32 = 10;
/// Query.
pub const T_QUERY: i32 = 11;
/// Third-party.
pub const T_THIRD_PARTY: i32 = 12;
/// Other.
pub const T_OTHER: i32 = 13;

/// Names of the packet types as the table shows them.
pub const TYPE_NAMES: [&str; 14] = [
    "",
    "Position",
    "Mic-E",
    "Object",
    "Item",
    "Status",
    "Message",
    "Weather",
    "Telemetry",
    "Raw GPS",
    "Capabilities",
    "Query",
    "Third-party",
    "Other",
];

/// The parse result (C layout, mirrored by the C++ `OnairAprsInfo`); strings are spans of the arena.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Info {
    /// Packet type id (`T_*`).
    pub kind: i32,
    /// Course in degrees (valid with `has_course`).
    pub course_deg: i32,
    /// Latitude, degrees, north positive.
    pub lat: f64,
    /// Longitude, degrees, east positive.
    pub lon: f64,
    /// Altitude, metres.
    pub alt_m: f64,
    /// Speed, knots.
    pub speed_knots: f64,
    /// Weather values.
    pub wx: Weather,
    /// Object or item name; the addressee of a message.
    pub name: Span,
    /// What follows the position.
    pub comment: Span,
    /// Message or status text.
    pub text: Span,
    /// Message number after the `{`.
    pub msg_no: Span,
    /// Mic-E message name.
    pub mic_status: Span,
    /// One line for the table.
    pub summary: Span,
    /// A position was found.
    pub has_pos: bool,
    /// Altitude present.
    pub has_alt: bool,
    /// Course present.
    pub has_course: bool,
    /// Speed present.
    pub has_speed: bool,
    /// Objects and items: false when killed.
    pub live: bool,
    /// Message is an ack.
    pub is_ack: bool,
    /// Message is a reject.
    pub is_rej: bool,
    /// Weather values present.
    pub has_wx: bool,
    /// Symbol table character.
    pub sym_table: u8,
    /// Symbol code character.
    pub sym_code: u8,
}

impl Default for Info {
    fn default() -> Self {
        Info {
            kind: T_NONE,
            course_deg: 0,
            lat: 0.0,
            lon: 0.0,
            alt_m: 0.0,
            speed_knots: 0.0,
            wx: Weather::default(),
            name: Span::default(),
            comment: Span::default(),
            text: Span::default(),
            msg_no: Span::default(),
            mic_status: Span::default(),
            summary: Span::default(),
            has_pos: false,
            has_alt: false,
            has_course: false,
            has_speed: false,
            live: true,
            is_ack: false,
            is_rej: false,
            has_wx: false,
            sym_table: 0,
            sym_code: 0,
        }
    }
}

/// `s` without trailing spaces, CR and LF.
fn trim_right(s: &[u8]) -> &[u8] {
    let mut n = s.len();
    while n > 0 && matches!(s[n - 1], b' ' | b'\r' | b'\n') {
        n -= 1;
    }
    &s[..n]
}

fn all_digits(s: &[u8], a: usize, n: usize) -> bool {
    a + n <= s.len() && s[a..a + n].iter().all(|c| c.is_ascii_digit())
}

/// Atoi of a digit string (digits already checked, optional leading minus).
fn atoi_simple(s: &[u8]) -> i32 {
    let (neg, d) = if s.first() == Some(&b'-') { (true, &s[1..]) } else { (false, s) };
    let mut v = 0i32;
    for &c in d {
        v = v * 10 + i32::from(c - b'0');
    }
    if neg {
        -v
    } else {
        v
    }
}

/// Course / speed `DDD/SSS` at the start of a comment (not for weather stations) and `/A=aaaaaa` (feet) anywhere. Returns the comment without
/// the course / speed prefix.
fn comment_extras<'a>(o: &mut Info, comment: &'a [u8]) -> &'a [u8] {
    let mut c = comment;
    if c.len() >= 7 && c[3] == b'/' && all_digits(c, 0, 3) && all_digits(c, 4, 3) && o.sym_code != b'_' {
        o.has_course = true;
        o.course_deg = atoi_simple(&c[..3]);
        o.has_speed = true;
        o.speed_knots = f64::from(atoi_simple(&c[4..7]));
        c = &c[7..];
    }
    if let Some(a) = c.windows(3).position(|w| w == b"/A=") {
        if a + 9 <= c.len() {
            let v = &c[a + 3..a + 9];
            let ok = v.iter().enumerate().all(|(i, &ch)| ch.is_ascii_digit() || (i == 0 && ch == b'-'));
            if ok {
                o.has_alt = true;
                o.alt_m = f64::from(atoi_simple(v)) * 0.3048;
            }
        }
    }
    c
}

/// PHG / RNG data extension at the start of the comment (APRS101 ch. 7) and !DAO! precision anywhere in it (WB4APR datum note): the text of the extension
/// goes in front of the comment that remains; the DAO adds its digits to `lat` and `lon`.
fn comment_data_ext(o: &mut Info, comment: &[u8], arena: &mut Arena) -> Span {
    let mut before: &[u8] = comment;
    let mut after: &[u8] = &[];
    if let Some(p) = comment.windows(5).position(|w| w[0] == b'!' && w[4] == b'!' && w[1].is_ascii_alphabetic() && w[2..4].iter().all(|b| b.is_ascii_graphic() || *b == b' ')) {
        let (d, a, b) = (comment[p + 1], comment[p + 2], comment[p + 3]);
        if o.has_pos {
            let (da, db) = if d.is_ascii_uppercase() {
                (dao_upper(a), dao_upper(b))
            } else {
                (dao_lower(a), dao_lower(b))
            };
            o.lat += if o.lat < 0.0 { -da } else { da };
            o.lon += if o.lon < 0.0 { -db } else { db };
        }
        before = &comment[..p];
        after = &comment[p + 5..];
    }
    let start = arena.mark();
    let mut head: &[u8] = before;
    if head.len() >= 7 && head[0..3] == *b"PHG" && all_digits(head, 3, 4) {
        let p = u32::from(head[3] - b'0');
        let h = 10u32 << u32::from(head[4] - b'0');
        let g = head[5] - b'0';
        let d = head[6] - b'0';
        arena.put(b"PHG ");
        arena.fixed(0, f64::from(p * p));
        arena.put(b" W, ");
        arena.fixed(0, f64::from(h));
        arena.put(b" ft HAAT, ");
        arena.fixed(0, f64::from(g));
        arena.put(b" dB, ");
        if d == 0 {
            arena.put(b"omni");
        } else {
            arena.put(b"directional to ");
            arena.fixed(0, f64::from(d.min(8)) * 45.0);
        }
        head = &head[7..];
        if !head.is_empty() || !after.is_empty() {
            arena.put(b", ");
        }
    } else if head.len() >= 7 && head[0..3] == *b"RNG" && all_digits(head, 3, 4) {
        arena.put(b"range ");
        arena.fixed(0, f64::from(atoi_simple(&head[3..7])));
        arena.put(b" mi");
        head = &head[7..];
        if !head.is_empty() || !after.is_empty() {
            arena.put(b", ");
        }
    }
    arena.put(head);
    arena.put(after);
    arena.since(start)
}

fn dao_upper(c: u8) -> f64 {
    if c.is_ascii_digit() {
        f64::from(c - b'0') * 0.001 / 60.0
    } else {
        0.0
    }
}

fn dao_lower(c: u8) -> f64 {
    if (33..=123).contains(&c) {
        f64::from(c - 33) * 1.1 * 0.0001 / 60.0
    } else {
        0.0
    }
}

/// ` [sym]` text of a position: "25.2000N 55.3000E [/>] 12 kn 270 deg 100 m".
fn put_pos_text(a: &mut Arena, o: &Info) {
    put_lat(a, o.lat);
    a.byte(b' ');
    put_lon(a, o.lon);
    if o.sym_table != 0 && o.sym_code != 0 {
        a.put(b" [");
        a.byte(o.sym_table);
        a.byte(o.sym_code);
        a.byte(b']');
    }
    if o.has_speed && o.speed_knots >= 0.5 {
        a.byte(b' ');
        a.fixed(0, o.speed_knots);
        a.put(b" kn");
    }
    if o.has_course && o.has_speed && o.speed_knots >= 0.5 {
        a.byte(b' ');
        a.fixed(0, f64::from(o.course_deg));
        a.put(b" deg");
    }
    if o.has_alt {
        a.byte(b' ');
        a.fixed(0, o.alt_m);
        a.put(b" m");
    }
}

/// "%.4f" of |v| followed by the hemisphere letter.
fn put_deg(a: &mut Arena, v: f64, neg: u8, pos: u8) {
    a.fixed(4, f64::from_bits(v.to_bits() & !(1u64 << 63)));
    a.byte(if v < 0.0 { neg } else { pos });
}

/// Latitude as "25.2000N".
pub fn put_lat(a: &mut Arena, v: f64) {
    put_deg(a, v, b'S', b'N');
}

/// Longitude as "55.3000E".
pub fn put_lon(a: &mut Arena, v: f64) {
    put_deg(a, v, b'W', b'E');
}

/// Stores the comment (after the position), takes course / speed / altitude out of it and, for the weather symbol `_`, the weather values.
fn finish_position(o: &mut Info, rest: &[u8], arena: &mut Arena) {
    let c = comment_extras(o, rest);
    o.comment = comment_data_ext(o, c, arena);
    if o.sym_code == b'_' {
        let mut w = Weather::default();
        if parse_weather(c, &mut w) {
            o.wx = w;
            o.has_wx = true;
        }
    }
}

/// Starts a summary: the text up to the current end becomes `o.summary` with `end_summary`.
fn put_name_pos(a: &mut Arena, o: &Info, prefix: &[u8], name: Span) {
    a.put(prefix);
    a.put_span(name);
    a.put(if o.live { b" " } else { b" (killed) " });
    put_pos_text(a, o);
    if o.comment.len > 0 {
        a.byte(b' ');
        a.put_span(o.comment);
    }
}

/// Parses an APRS information field. `dest` is the destination callsign of the frame (Mic-E carries the latitude in it), `info` the information
/// field. False when the field is empty or the type byte is not understood; `o` is filled as far as the parser got in either case.
pub fn parse(dest: &[u8], info: &[u8], o: &mut Info, arena: &mut Arena) -> bool {
    *o = Info::default();
    if info.is_empty() {
        return false;
    }
    let dti = info[0];
    match dti {
        b'!' | b'=' | b'/' | b'@' => {
            if dti == b'!' && info.len() > 1 && info[1] == b'!' {
                o.kind = T_OTHER;
                o.text = arena.push(info);
                o.summary = arena.push(b"Ultimeter weather station data");
                return true;
            }
            let skip = if dti == b'/' || dti == b'@' { 8 } else { 1 };
            if info.len() <= skip {
                return false;
            }
            let used = parse_pos(&info[skip..], o);
            if used == 0 {
                o.kind = T_OTHER;
                o.text = arena.push(info);
                o.summary = arena.push(b"position report that could not be read");
                return true;
            }
            finish_position(o, &info[skip + used..], arena);
            o.kind = if o.has_wx { T_WEATHER } else { T_POSITION };
            let m = arena.mark();
            arena.put(if o.has_wx { b"Weather " } else { b"Position " });
            put_pos_text(arena, o);
            if o.has_wx {
                arena.put(b": ");
                put_summary(arena, &o.wx);
            }
            if o.comment.len > 0 && !o.has_wx {
                arena.byte(b' ');
                arena.put_span(o.comment);
            }
            o.summary = arena.since(m);
            true
        }
        0x60 | b'\'' | 0x1c | 0x1d => {
            if !mic_e(dest, info, o, arena) {
                return false;
            }
            o.kind = T_MIC_E;
            let m = arena.mark();
            arena.put(b"Mic-E ");
            put_pos_text(arena, o);
            if o.mic_status.len > 0 {
                arena.put(b", ");
                arena.put_span(o.mic_status);
            }
            if o.comment.len > 0 {
                arena.byte(b' ');
                arena.put_span(o.comment);
            }
            o.summary = arena.since(m);
            true
        }
        b':' => {
            if info.len() < 11 || info[10] != b':' {
                return false;
            }
            o.kind = T_MESSAGE;
            o.name = arena.push(trim_right(&info[1..10]));
            let mut t = &info[11..];
            let br = t.iter().rposition(|&c| c == b'{');
            let mut empty = false;
            // the message number ends at a '}' (the reply-ack form "{MM}AA" of APRS 1.1 carries the acknowledged number after it) or a CR / LF
            let num = |s: &[u8]| -> usize { s.iter().position(|&c| c == b'}' || c == b'\r' || c == b'\n').unwrap_or(s.len()) };
            if t.starts_with(b"ack") && t.len() > 3 {
                o.is_ack = true;
                o.msg_no = arena.push(&t[3..3 + num(&t[3..])]);
                empty = true;
            } else if t.starts_with(b"rej") && t.len() > 3 {
                o.is_rej = true;
                o.msg_no = arena.push(&t[3..3 + num(&t[3..])]);
                empty = true;
            } else if let Some(b) = br.filter(|&b| t.len() - b <= 7) {
                o.msg_no = arena.push(&t[b + 1..b + 1 + num(&t[b + 1..])]);
                t = &t[..b];
            }
            if empty {
                t = &[];
            }
            o.text = arena.push(t);
            let m = arena.mark();
            if o.is_ack {
                arena.put(b"Ack ");
                arena.put_span(o.msg_no);
                arena.put(b" to ");
                arena.put_span(o.name);
            } else if o.is_rej {
                arena.put(b"Reject ");
                arena.put_span(o.msg_no);
                arena.put(b" to ");
                arena.put_span(o.name);
            } else {
                arena.put(b"Message to ");
                arena.put_span(o.name);
                arena.put(b": ");
                arena.put_span(o.text);
                if o.msg_no.len > 0 {
                    arena.put(b" {");
                    arena.put_span(o.msg_no);
                    arena.byte(b'}');
                }
            }
            o.summary = arena.since(m);
            true
        }
        b';' => {
            if info.len() < 18 || (info[10] != b'*' && info[10] != b'_') {
                return false;
            }
            o.kind = T_OBJECT;
            o.name = arena.push(trim_right(&info[1..10]));
            o.live = info[10] == b'*';
            let used = parse_pos(&info[18..], o);
            if used == 0 {
                return false;
            }
            finish_position(o, &info[18 + used..], arena);
            let m = arena.mark();
            put_name_pos(arena, o, b"Object ", o.name);
            o.summary = arena.since(m);
            true
        }
        b')' => {
            let mut e = 1;
            while e < info.len() && e < 11 && info[e] != b'!' && info[e] != b'_' {
                e += 1;
            }
            if e >= info.len() || e < 4 || (info[e] != b'!' && info[e] != b'_') {
                return false;
            }
            o.kind = T_ITEM;
            o.name = arena.push(&info[1..e]);
            o.live = info[e] == b'!';
            let used = parse_pos(&info[e + 1..], o);
            if used == 0 {
                return false;
            }
            finish_position(o, &info[e + 1 + used..], arena);
            let m = arena.mark();
            put_name_pos(arena, o, b"Item ", o.name);
            o.summary = arena.since(m);
            true
        }
        b'>' => {
            o.kind = T_STATUS;
            let mut t = trim_right(&info[1..]);
            if t.len() > 7 && all_digits(t, 0, 6) && t[6] == b'z' {
                t = &t[7..]; // a time stamp in front
            }
            o.text = arena.push(t);
            let m = arena.mark();
            arena.put(b"Status: ");
            arena.put_span(o.text);
            o.summary = arena.since(m);
            true
        }
        b'_' => {
            if info.len() < 9 {
                return false;
            }
            let mut w = Weather::default();
            if !parse_weather(&info[9..], &mut w) {
                return false;
            }
            o.kind = T_WEATHER;
            o.has_wx = true;
            o.wx = w;
            let m = arena.mark();
            arena.put(b"Weather: ");
            put_summary(arena, &w);
            o.summary = arena.since(m);
            true
        }
        _ => {
            if dti == b'T' && info.len() > 2 && info[1] == b'#' {
                return text_type(o, arena, T_TELEMETRY, b"Telemetry ", &info[2..]);
            }
            match dti {
                b'$' => {
                    o.kind = T_RAW_GPS;
                    o.text = arena.push(trim_right(info));
                    o.summary = arena.push_span(o.text);
                    true
                }
                b'<' => text_type(o, arena, T_CAPABILITIES, b"Capabilities ", &info[1..]),
                b'?' => text_type(o, arena, T_QUERY, b"Query ", &info[1..]),
                b'}' => text_type(o, arena, T_THIRD_PARTY, b"Third-party ", &info[1..]),
                _ => {
                    o.kind = T_OTHER;
                    o.text = arena.push(trim_right(info));
                    o.summary = arena.push_span(o.text);
                    true
                }
            }
        }
    }
}

/// A packet type whose text is the rest of the field: summary = prefix + text.
fn text_type(o: &mut Info, arena: &mut Arena, kind: i32, prefix: &[u8], text: &[u8]) -> bool {
    o.kind = kind;
    o.text = arena.push(text);
    let m = arena.mark();
    arena.put(prefix);
    arena.put_span(o.text);
    o.summary = arena.since(m);
    true
}

impl Arena<'_> {
    /// Appends a copy of an earlier string as a string of its own.
    pub fn push_span(&mut self, s: Span) -> Span {
        let m = self.mark();
        self.put_span(s);
        self.since(m)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn run(dest: &[u8], info: &[u8]) -> (bool, Info, [u8; 1024]) {
        let mut buf = [0u8; 1024];
        let mut o = Info::default();
        let ok = {
            let mut a = Arena::new(&mut buf);
            parse(dest, info, &mut o, &mut a)
        };
        (ok, o, buf)
    }
    fn s(b: &[u8], sp: Span) -> &str {
        core::str::from_utf8(&b[sp.off as usize..(sp.off + sp.len) as usize]).unwrap()
    }
    #[test]
    fn phg_rng_dao() {
        let (ok, o, b) = run(b"APRS", b"!4903.50N/07201.75W#PHG5132Weather igate");
        assert!(ok);
        assert_eq!(s(&b, o.comment), "PHG 25 W, 20 ft HAAT, 3 dB, directional to 90, Weather igate");
        let (_, o, b) = run(b"APRS", b"!4903.50N/07201.75W#RNG0050 digi");
        assert_eq!(s(&b, o.comment), "range 50 mi,  digi");
        // DAO, human readable: 1 thousandth of a minute of latitude, 8 of longitude; north positive, west negative
        let (_, base, _) = run(b"APRS", b"!4903.50N/07201.75W-hello");
        let (_, o, b) = run(b"APRS", b"!4903.50N/07201.75W-hello!W18!");
        assert!((o.lat - base.lat - 0.001 / 60.0).abs() < 1e-9);
        assert!((o.lon - base.lon + 0.008 / 60.0).abs() < 1e-9);
        assert_eq!(s(&b, o.comment), "hello");
        // DAO, base 91: value of 'A' (32) x 1.1 hundredths of a thousandth
        let (_, o, _) = run(b"APRS", b"!4903.50N/07201.75W-x!wAA!");
        assert!((o.lat - base.lat - f64::from(32u8) * 1.1 * 0.0001 / 60.0).abs() < 1e-9);
        // a datum note only, spaces in place of digits, changes nothing
        let (_, o, _) = run(b"APRS", b"!4903.50N/07201.75W-!W  !");
        assert!((o.lat - base.lat).abs() < 1e-12);
    }

    #[test]
    fn position_and_message() {
        let (ok, o, b) = run(b"APRS", b"!4903.50N/07201.75W-Test 001234");
        assert!(ok && o.kind == T_POSITION);
        assert!(s(&b, o.summary).starts_with("Position 49.0583N 72.0292W [/-]"), "{}", s(&b, o.summary));
        let (ok, o, b) = run(b"APRS", b":N0CALL-9 :hello{12");
        assert!(ok && o.kind == T_MESSAGE);
        assert_eq!(s(&b, o.summary), "Message to N0CALL-9: hello {12}");
        let (ok, o, b) = run(b"APRS", b":N0CALL-9 :ack12");
        assert!(ok && o.is_ack);
        assert_eq!(s(&b, o.summary), "Ack 12 to N0CALL-9");
        // the reply-ack form: the message number stops at the '}'
        let (ok, o, b) = run(b"APRS", b":WU2Z     :Testing{AB}CD");
        assert!(ok && s(&b, o.msg_no) == "AB" && s(&b, o.text) == "Testing");
        let (ok, o, b) = run(b"APRS", b":WU2Z     :ackAB}CD");
        assert!(ok && o.is_ack && s(&b, o.msg_no) == "AB");
    }
    #[test]
    fn mic_e_and_weather() {
        // the APRS101 Mic-E example: destination S32U6T, 33 deg 25.64 N, 112 deg 07.74 W, 20 kn, 251 deg
        let (ok, o, _) = run(b"S32U6T", b"`(_f\"Oj/>");
        assert!(ok && o.kind == T_MIC_E && o.has_pos);
        assert!((o.lat - (33.0 + 2564.0 / 6000.0)).abs() < 1e-9);
        let (ok, o, b) = run(b"APRS", b"_10090556c220s004g005t077r000p000P000h50b09900wRSW");
        assert!(ok && o.has_wx);
        assert!(s(&b, o.summary).starts_with("Weather: 77 F, wind 4 mph from 220 deg"));
    }
    #[test]
    fn rejects() {
        assert!(!run(b"APRS", b"").0);
        assert!(!run(b"APRS", b":short").0);
    }
}
