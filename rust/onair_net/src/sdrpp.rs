// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The SDR++ server protocol (SDR++'s `server` module and its SmGui draw list): packets are an 8 byte header (type, total size, little endian) and a body; commands carry a
//! 32-bit command id; the server's menu comes as a draw list of typed elements (step, bool, int, float, string) that this module turns into a list of widgets.
//! Nothing is allocated: the widgets, their combo items and the generated labels go to arrays the caller owns, text is given as ranges of the input or of the label arena.
use crate::Span;

/// Packet type: command.
pub const PKT_COMMAND: u32 = 0;
/// Packet type: acknowledgement.
pub const PKT_ACK: u32 = 1;
/// Packet type: baseband samples.
pub const PKT_BASEBAND: u32 = 2;
/// Packet type: compressed baseband samples.
pub const PKT_BASEBAND_ZSTD: u32 = 3;
/// Packet type: VFO samples.
pub const PKT_VFO: u32 = 4;
/// Packet type: FFT.
pub const PKT_FFT: u32 = 5;
/// Packet type: error.
pub const PKT_ERROR: u32 = 6;
/// Largest packet the client accepts.
pub const MAX_PACKET: u32 = 32 << 20;

const EL_STEP: u8 = 0;
const EL_BOOL: u8 = 1;
const EL_INT: u8 = 2;
const EL_FLOAT: u8 = 3;
const EL_STRING: u8 = 4;

const ST_FILL_WIDTH: u8 = 0x00;
const ST_SAME_LINE: u8 = 0x01;
const ST_BEGIN_DISABLED: u8 = 0x02;
const ST_END_DISABLED: u8 = 0x03;
const ST_COMBO: u8 = 0x80;
const ST_RADIO: u8 = 0x84;
const ST_LEFT_LABEL: u8 = 0x87;
const ST_SLIDER_INT: u8 = 0x88;
const ST_SLIDER_FLOAT_STEPS: u8 = 0x89;
const ST_INPUT_INT: u8 = 0x8a;
const ST_CHECKBOX: u8 = 0x8b;
const ST_SLIDER_FLOAT: u8 = 0x8c;
const ST_TEXT: u8 = 0x8e;
const ST_TEXT_COLORED: u8 = 0x8f;

/// Widget kinds.
pub const COMBO: u32 = 0;
/// A check box.
pub const CHECKBOX: u32 = 1;
/// A radio button.
pub const RADIO: u32 = 2;
/// An integer slider.
pub const SLIDER_INT: u32 = 3;
/// A float slider.
pub const SLIDER_FLOAT: u32 = 4;
/// An integer input.
pub const INPUT_INT: u32 = 5;
/// A line of text.
pub const TEXT: u32 = 6;

/// One control of the server's menu. Ranges are bytes of the draw list (`id`, `label` unless `label_in_arena`, combo items) or of the label arena.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Widget {
    /// Kind (`COMBO` ...).
    pub kind: u32,
    /// Greyed out in the server's menu.
    pub disabled: u32,
    /// Check box: on; radio: this one is chosen.
    pub b: u32,
    /// Combo: the entry chosen.
    pub i: i32,
    /// First combo item in the caller's item array.
    pub items_first: u32,
    /// Number of combo items.
    pub items_count: u32,
    /// 1 when `label` is a range of the label arena.
    pub label_in_arena: u32,
    /// Spare (keeps the doubles aligned).
    pub reserved: u32,
    /// Sliders and the integer input: value, minimum, maximum, step.
    pub value: f64,
    /// Minimum.
    pub min_v: f64,
    /// Maximum.
    pub max_v: f64,
    /// Step (0 for a float slider without steps).
    pub step: f64,
    /// The ImGui label the server knows the control by.
    pub id: Span,
    /// What the control is called.
    pub label: Span,
}

/// Counts of what `parse_ui` wrote.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct UiCounts {
    /// 1 when the draw list was read (0: cut short or an unknown element type).
    pub ok: u32,
    /// Widgets written.
    pub widgets: u32,
    /// Combo items written.
    pub items: u32,
    /// Bytes used of the label arena.
    pub arena: u32,
}

fn le16(p: &[u8]) -> u32 {
    p[0] as u32 | (p[1] as u32) << 8
}
fn le32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

#[derive(Clone, Copy)]
struct El {
    ty: u8,
    step: u8,
    b: bool,
    i: i32,
    f: f32,
    s: Span,
}

/// The element at byte `k` of the draw list as SmGui::DrawList::loadItem reads it, and the offset after it; None when it is cut short or of an unknown type.
fn element(p: &[u8], k: usize) -> Option<(El, usize)> {
    let ty = *p.get(k)?;
    let mut e = El { ty, step: 0, b: false, i: 0, f: 0.0, s: Span::default() };
    let k = k + 1;
    let n = p.len();
    match ty {
        EL_STEP => {
            if k + 2 > n {
                return None;
            }
            e.step = p[k];
            Some((e, k + 2))
        }
        EL_BOOL => {
            if k + 1 > n {
                return None;
            }
            e.b = p[k] != 0;
            Some((e, k + 1))
        }
        EL_INT => {
            if k + 4 > n {
                return None;
            }
            e.i = le32(&p[k..]) as i32;
            Some((e, k + 4))
        }
        EL_FLOAT => {
            if k + 4 > n {
                return None;
            }
            e.f = f32::from_bits(le32(&p[k..]));
            Some((e, k + 4))
        }
        EL_STRING => {
            if k + 2 > n {
                return None;
            }
            let len = le16(&p[k..]) as usize;
            let k = k + 2;
            if k + len > n {
                return None;
            }
            e.s = Span { off: k as u32, len: len as u32 };
            Some((e, k + len))
        }
        _ => None,
    }
}

fn text(p: &[u8], s: Span) -> &[u8] {
    &p[s.off as usize..(s.off + s.len) as usize]
}

/// The part of an id after the first "##" (empty without one): returns the range inside `id`.
fn hidden_part(id: &[u8]) -> (usize, usize) {
    match id.windows(2).position(|w| w == b"##") {
        Some(p) => (p + 2, id.len()),
        None => (0, 0),
    }
}

/// The part of an id before the first "##".
fn visible_len(id: &[u8]) -> usize {
    id.windows(2).position(|w| w == b"##").unwrap_or(id.len())
}

/// The server-source combo's id.
const SERVER_SOURCE_ID: &[u8] = b"##sdrpp_server_src_sel";

/// The k-th word (range of `hidden`) of the hidden id split at `_`, empty parts dropped.
fn nth_word(hidden: &[u8], k: usize) -> Option<(usize, usize)> {
    let mut seen = 0usize;
    let mut cur = 0usize;
    for i in 0..=hidden.len() {
        if i == hidden.len() || hidden[i] == b'_' {
            if i > cur {
                if seen == k {
                    return Some((cur, i));
                }
                seen += 1;
            }
            cur = i + 1;
        }
    }
    None
}

/// The words of a widget's hidden id as the C++ `idWords` keeps them: the range [start, end) of word numbers left after dropping the source's instance name at the end
/// (a word with a space or a capital letter) and the module's name at the front ("_airspy_lna_gain_Airspy Source" keeps lna, gain).
fn kept_range(hidden: &[u8]) -> (usize, usize) {
    let mut n = 0usize;
    while nth_word(hidden, n).is_some() {
        n += 1;
    }
    let mut end = n;
    if let Some((a, b)) = n.checked_sub(1).and_then(|l| nth_word(hidden, l)) {
        let last = &hidden[a..b];
        if last.contains(&b' ') || last.iter().any(u8::is_ascii_uppercase) {
            end -= 1;
        }
    }
    (if end > 1 { 1 } else { 0 }, end)
}

const WORDS: &[(&[u8], &[u8])] = &[
    (b"dev", b"Device"),
    (b"sr", b"Sample rate"),
    (b"bw", b"Bandwidth"),
    (b"ant", b"Antenna"),
    (b"gm", b"Gain mode"),
    (b"clk", b"Clock"),
    (b"ch", b"Channel"),
    (b"chan", b"Channel"),
    (b"ds", b"Direct sampling"),
    (b"agc", b"AGC"),
    (b"port", b"Port"),
    (b"lna", b"LNA"),
    (b"vga", b"VGA"),
    (b"mix", b"Mixer"),
    (b"att", b"Attenuator"),
    (b"atten", b"Attenuator"),
    (b"attn", b"Attenuator"),
    (b"ppm", b"PPM"),
    (b"biast", b"Bias tee"),
    (b"ofs", b"Offset tuning"),
    (b"rtl", b"RTL"),
    (b"lin", b"Linear"),
    (b"sens", b"Sensitive"),
    (b"refr", b"Refresh"),
    (b"ref", b"Reference"),
];

fn eq_lower(w: &[u8], lower: &[u8]) -> bool {
    w.len() == lower.len() && w.iter().zip(lower).all(|(a, b)| a.to_ascii_lowercase() == *b)
}

/// Word flags of `id_flags`.
pub const W_SR: u32 = 1;
/// "bw"
pub const W_BW: u32 = 2;
/// "lna"
pub const W_LNA: u32 = 4;
/// "vga"
pub const W_VGA: u32 = 8;
/// "mix"
pub const W_MIX: u32 = 16;
/// "mixer"
pub const W_MIXER: u32 = 32;
/// "if"
pub const W_IF: u32 = 64;
/// "lin"
pub const W_LIN: u32 = 128;
/// "sens"
pub const W_SENS: u32 = 256;

/// Which of the words sr, bw, lna, vga, mix, mixer, if, lin, sens a control's id holds (see `kept_range` for the words that count), as `W_*` bits.
pub fn id_flags(id: &[u8]) -> u32 {
    let (h0, h1) = hidden_part(id);
    let hidden = &id[h0..h1];
    let (s, e) = kept_range(hidden);
    let names: [(&[u8], u32); 9] = [
        (b"sr", W_SR),
        (b"bw", W_BW),
        (b"lna", W_LNA),
        (b"vga", W_VGA),
        (b"mix", W_MIX),
        (b"mixer", W_MIXER),
        (b"if", W_IF),
        (b"lin", W_LIN),
        (b"sens", W_SENS),
    ];
    let mut f = 0;
    for k in s..e {
        if let Some((a, b)) = nth_word(hidden, k) {
            for (n, bit) in names {
                if eq_lower(&hidden[a..b], n) {
                    f |= bit;
                }
            }
        }
    }
    f
}

/// A readable name for a control that has no label of its own, written to `out`; returns its length, or `usize::MAX` when `out` is too short.
pub fn label_from_id(id: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0usize;
    let put = |b: &[u8], n: &mut usize, out: &mut [u8]| -> bool {
        if *n + b.len() > out.len() {
            return false;
        }
        out[*n..*n + b.len()].copy_from_slice(b);
        *n += b.len();
        true
    };
    if id == SERVER_SOURCE_ID {
        return if put(b"Server source", &mut n, out) { n } else { usize::MAX };
    }
    let (h0, h1) = hidden_part(id);
    let hidden = &id[h0..h1];
    let (s, e) = kept_range(hidden);
    for k in s..e {
        let Some((a, b)) = nth_word(hidden, k) else { break };
        let w = &hidden[a..b];
        if eq_lower(w, b"sel") || eq_lower(w, b"select") {
            continue;
        }
        if n > 0 && !put(b" ", &mut n, out) {
            return usize::MAX;
        }
        match WORDS.iter().find(|(key, _)| eq_lower(w, key)) {
            Some((_, v)) => {
                if !put(v, &mut n, out) {
                    return usize::MAX;
                }
            }
            None => {
                for &c in w {
                    if !put(&[c.to_ascii_lowercase()], &mut n, out) {
                        return usize::MAX;
                    }
                }
            }
        }
    }
    if n == 0 && !put(hidden, &mut n, out) {
        return usize::MAX;
    }
    if n > 0 {
        out[0] = out[0].to_ascii_uppercase();
    }
    n
}

/// The factor of a rate text's unit ("hz" 1, "k" 1e3, "m" 1e6, "g" 1e9, "sps", "s/s") after the number; 0 when the unit is not one of them.
pub fn rate_unit(unit: &[u8]) -> f64 {
    let mut buf = [0u8; 8];
    let mut n = 0usize;
    for &c in unit {
        if c == b' ' {
            continue;
        }
        if n >= buf.len() {
            return 0.0; // none of the accepted units is this long
        }
        buf[n] = c.to_ascii_lowercase();
        n += 1;
    }
    let mut u = &buf[..n];
    let mut mul = 1.0;
    if let Some(&c) = u.first() {
        if matches!(c, b'k' | b'm' | b'g') {
            mul = if c == b'k' {
                1e3
            } else if c == b'm' {
                1e6
            } else {
                1e9
            };
            u = &u[1..];
        }
    }
    if u.is_empty() || u == b"hz" || u == b"sps" || u == b"s/s" {
        mul
    } else {
        0.0
    }
}

/// Writes the 8 byte packet header and body of a command (id and argument bytes) to `out`; returns its length or `usize::MAX` when `out` is too short.
pub fn command(cmd: u32, arg: &[u8], out: &mut [u8]) -> usize {
    let t = 12 + arg.len();
    if out.len() < t || t > u32::MAX as usize {
        return usize::MAX;
    }
    out[..4].copy_from_slice(&PKT_COMMAND.to_le_bytes());
    out[4..8].copy_from_slice(&(t as u32).to_le_bytes());
    out[8..12].copy_from_slice(&cmd.to_le_bytes());
    out[12..t].copy_from_slice(arg);
    t
}

/// The argument of a UI_ACTION command: the sync flag, the control's id as a string element and the value (type 1 bool, 2 int, other: float `f`). The id is cut to 65535
/// bytes. Returns its length or `usize::MAX` when `out` is too short.
pub fn ui_action_arg(id: &[u8], ty: u8, i: i32, f: f32, b: bool, sync: bool, out: &mut [u8]) -> usize {
    let id = &id[..id.len().min(0xffff)];
    let t = 1 + 1 + 2 + id.len() + 1 + if ty == EL_BOOL { 1 } else { 4 };
    if out.len() < t {
        return usize::MAX;
    }
    out[0] = sync as u8;
    out[1] = EL_STRING;
    out[2] = id.len() as u8;
    out[3] = (id.len() >> 8) as u8;
    out[4..4 + id.len()].copy_from_slice(id);
    let mut k = 4 + id.len();
    out[k] = ty;
    k += 1;
    if ty == EL_BOOL {
        out[k] = b as u8;
    } else if ty == EL_INT {
        out[k..k + 4].copy_from_slice(&(i as u32).to_le_bytes());
    } else {
        out[k..k + 4].copy_from_slice(&f.to_bits().to_le_bytes());
    }
    t
}

/// Reads a packet header: (type, body size); None when it is short, the size is under 8 or over `MAX_PACKET`, or the type is over `PKT_ERROR`. The size counts the header.
pub fn packet_header(h: &[u8]) -> Option<(u32, u32)> {
    if h.len() < 8 {
        return None;
    }
    let (ty, size) = (le32(h), le32(&h[4..]));
    if !(8..=MAX_PACKET).contains(&size) || ty > PKT_ERROR {
        return None;
    }
    Some((ty, size - 8))
}

/// Text of the error packet's code.
pub fn error_text(c: u8) -> Option<&'static str> {
    match c {
        0 => Some("none"),
        1 => Some("invalid packet"),
        2 => Some("invalid command"),
        3 => Some("invalid argument"),
        _ => None,
    }
}

/// Turns the draw list `p` (the answer to GET_UI / UI_ACTION without its command id) into widgets. `widgets`, `items` (the combo entries, ranges of `p`) and `arena`
/// (generated labels) are the caller's; `UiCounts::ok` is 0 for a list that is cut short or holds an unknown element type, and the counts then tell nothing. When an array
/// is too small `ok` is 0 as well and `arena` is `u32::MAX`; sizes that always suffice: widgets `p.len() / 3 + 1`, items `p.len() + 1`, arena `9 * p.len() + 64`.
pub fn parse_ui(p: &[u8], widgets: &mut [Widget], items: &mut [Span], arena: &mut [u8]) -> UiCounts {
    let bad = UiCounts::default();
    let too_small = UiCounts { arena: u32::MAX, ..UiCounts::default() };
    // pass 1: the whole list must read
    let mut k = 0usize;
    while k < p.len() {
        match element(p, k) {
            Some((_, nk)) => k = nk,
            None => return bad,
        }
    }
    let (mut nw, mut ni, mut na) = (0usize, 0usize, 0usize);
    let mut disabled = 0i32;
    let mut left_label = Span::default();
    let mut k = 0usize;
    while k < p.len() {
        let Some((first, mut next)) = element(p, k) else { return bad };
        if first.ty != EL_STEP {
            return bad; // every step is followed by its arguments, none of which is a step
        }
        let st = first.step;
        let a0 = next;
        let mut args = [El { ty: 0, step: 0, b: false, i: 0, f: 0.0, s: Span::default() }; 6];
        let mut na_args = 0usize;
        while next < p.len() {
            let Some((e, nk)) = element(p, next) else { return bad };
            if e.ty == EL_STEP {
                break;
            }
            if na_args < args.len() {
                args[na_args] = e;
            }
            na_args += 1;
            next = nk;
        }
        k = next;
        let is = |j: usize, t: u8| j < na_args && j < args.len() && args[j].ty == t;
        let mut w = Widget::default();
        let mut widget = false;
        match st {
            ST_BEGIN_DISABLED => disabled += 1,
            ST_END_DISABLED => disabled = (disabled - 1).max(0),
            ST_LEFT_LABEL => {
                if is(0, EL_STRING) {
                    left_label = args[0].s;
                }
            }
            ST_COMBO => {
                if is(0, EL_STRING) && is(1, EL_INT) && is(2, EL_STRING) {
                    w.kind = COMBO;
                    w.id = args[0].s;
                    w.i = args[1].i;
                    w.items_first = ni as u32;
                    // ImGui's "a\0b\0c\0" list
                    let list = text(p, args[2].s);
                    let base = args[2].s.off as usize;
                    let mut q = 0usize;
                    while q < list.len() {
                        let z = list[q..].iter().position(|&c| c == 0).map(|z| z + q);
                        let end = z.unwrap_or(list.len());
                        if ni >= items.len() {
                            return too_small;
                        }
                        items[ni] = Span { off: (base + q) as u32, len: (end - q) as u32 };
                        ni += 1;
                        if z.is_none() {
                            break;
                        }
                        q = end + 1;
                    }
                    w.items_count = ni as u32 - w.items_first;
                    widget = true;
                }
            }
            ST_CHECKBOX | ST_RADIO => {
                if is(0, EL_STRING) && is(1, EL_BOOL) {
                    w.kind = if st == ST_CHECKBOX { CHECKBOX } else { RADIO };
                    w.id = args[0].s;
                    w.b = args[1].b as u32;
                    widget = true;
                }
            }
            ST_SLIDER_INT => {
                if is(0, EL_STRING) && is(1, EL_INT) && is(2, EL_INT) && is(3, EL_INT) {
                    w.kind = SLIDER_INT;
                    w.id = args[0].s;
                    w.value = args[1].i as f64;
                    w.min_v = args[2].i as f64;
                    w.max_v = args[3].i as f64;
                    w.step = 1.0;
                    widget = true;
                }
            }
            ST_SLIDER_FLOAT_STEPS => {
                if is(0, EL_STRING) && is(1, EL_FLOAT) && is(2, EL_FLOAT) && is(3, EL_FLOAT) && is(4, EL_FLOAT) {
                    w.kind = SLIDER_FLOAT;
                    w.id = args[0].s;
                    w.value = args[1].f as f64;
                    w.min_v = args[2].f as f64;
                    w.max_v = args[3].f as f64;
                    w.step = args[4].f as f64;
                    widget = true;
                }
            }
            ST_SLIDER_FLOAT => {
                if is(0, EL_STRING) && is(1, EL_FLOAT) && is(2, EL_FLOAT) && is(3, EL_FLOAT) {
                    w.kind = SLIDER_FLOAT;
                    w.id = args[0].s;
                    w.value = args[1].f as f64;
                    w.min_v = args[2].f as f64;
                    w.max_v = args[3].f as f64;
                    w.step = 0.0;
                    widget = true;
                }
            }
            ST_INPUT_INT => {
                if is(0, EL_STRING) && is(1, EL_INT) && is(2, EL_INT) {
                    w.kind = INPUT_INT;
                    w.id = args[0].s;
                    w.value = args[1].i as f64;
                    w.step = args[2].i.max(1) as f64;
                    widget = true;
                }
            }
            ST_TEXT | ST_TEXT_COLORED => {
                // the last non-empty string among all the arguments
                let mut q = a0;
                while q < k {
                    let Some((e, nq)) = element(p, q) else { break };
                    if e.ty == EL_STRING && e.s.len > 0 {
                        w.kind = TEXT;
                        w.label = e.s;
                        widget = true;
                    }
                    q = nq;
                }
            }
            _ => {} // layout steps (columns, groups, tables, popups), buttons, text inputs: nothing to drive
        }
        if widget {
            w.disabled = (disabled > 0) as u32;
            if w.kind != TEXT {
                let id = text(p, w.id);
                let vis = visible_len(id);
                if left_label.len > 0 {
                    w.label = left_label;
                } else if vis > 0 && w.kind != RADIO {
                    w.label = Span { off: w.id.off, len: vis as u32 };
                } else {
                    let m = label_from_id(id, &mut arena[na..]);
                    if m == usize::MAX {
                        return too_small;
                    }
                    w.label = Span { off: na as u32, len: m as u32 };
                    w.label_in_arena = 1;
                    na += m;
                }
            }
            if nw >= widgets.len() {
                return too_small;
            }
            widgets[nw] = w;
            nw += 1;
        }
        if st != ST_LEFT_LABEL && st != ST_FILL_WIDTH && st != ST_SAME_LINE && st != ST_BEGIN_DISABLED && st != ST_END_DISABLED {
            left_label = Span::default();
        }
    }
    UiCounts { ok: 1, widgets: nw as u32, items: ni as u32, arena: na as u32 }
}
