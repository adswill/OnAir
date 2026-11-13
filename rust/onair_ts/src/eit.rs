// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! EIT section parser (ETSI EN 300 468 section 5.2.4): the present / following event name of the service list and every event of the guide with its
//! short event (0x4D), extended event (0x4E) and content (0x54) descriptors. Text fields are `Span`s into the section.
use crate::{bcd, rd, rd16, Span};

/// One programme.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EitEvent {
    /// event_id.
    pub event_id: i32,
    /// Start, seconds since 1970 UTC.
    pub start: i64,
    /// Duration in seconds.
    pub duration: i32,
    /// running_status.
    pub running: i32,
    /// First content nibble, -1 when there is no content descriptor.
    pub genre: i32,
    /// ISO 639 language of the short event descriptor.
    pub lang: [u8; 3],
    /// `lang` is set.
    pub has_lang: u8,
    /// `title` is set.
    pub has_title: u8,
    /// `text` is set.
    pub has_text: u8,
    /// Bit n set: `ext[n]` is present.
    pub ext_mask: u16,
    /// event_name bytes.
    pub title: Span,
    /// short event text bytes.
    pub text: Span,
    /// Pieces of the extended text by descriptor_number (later descriptors with the same number replace earlier ones).
    pub ext: [Span; 16],
}

/// Header-level result of an EIT parse.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EitInfo {
    /// service_id.
    pub sid: i32,
    /// section_number.
    pub sec_num: i32,
    /// The present / following block applies (table 0x4E, section 0 or 1).
    pub legacy: u8,
    /// The legacy block found an event name (`now_next_name` is valid; otherwise the name is empty).
    pub has_name: u8,
    /// event_name bytes of the present / following event.
    pub now_next_name: Span,
    /// Events written.
    pub n_events: u32,
}

/// Parse an EIT section (`s.len() >= 18`): the present / following name and the events (those with no start time or a duration of 0 are skipped).
/// Descriptors that run past the section are ignored.
pub fn parse_eit(s: &[u8], out: &mut [EitEvent]) -> Option<EitInfo> {
    let len = s.len();
    if len < 18 {
        return None;
    }
    let tid = rd(s, 0);
    let mut info = EitInfo { sid: rd16(s, 3), sec_num: rd(s, 6), ..Default::default() };
    if tid == 0x4E && info.sec_num <= 1 {
        info.legacy = 1;
        let i = 14usize;
        if i + 12 <= len - 4 {
            let dl = (((rd(s, i + 10) & 0x0F) << 8) | rd(s, i + 11)) as usize;
            let mut j = i + 12;
            while j + 2 <= i + 12 + dl && j + 2 <= len {
                let tag = rd(s, j);
                let l = rd(s, j + 1) as usize;
                if j + 2 + l > len {
                    break;
                }
                if tag == 0x4D && l >= 5 {
                    let nl = rd(s, j + 5) as usize;
                    if 4 + nl <= l {
                        info.has_name = 1;
                        info.now_next_name = Span { off: (j + 6) as u32, len: nl as u32 };
                    }
                }
                j += 2 + l;
            }
        }
    }
    let end = len - 4;
    let mut i = 14usize;
    let mut n = 0usize;
    while i + 12 <= end {
        let mut ev = EitEvent { event_id: rd16(s, i), genre: -1, ..Default::default() };
        let mjd = rd16(s, i + 2) as i64;
        let undefined = rd(s, i + 2) == 0xFF && rd(s, i + 3) == 0xFF && rd(s, i + 4) == 0xFF;
        ev.start = if undefined {
            0
        } else {
            (mjd - 40587) * 86400 + (bcd(rd(s, i + 4)) * 3600 + bcd(rd(s, i + 5)) * 60 + bcd(rd(s, i + 6))) as i64
        };
        ev.duration = bcd(rd(s, i + 7)) * 3600 + bcd(rd(s, i + 8)) * 60 + bcd(rd(s, i + 9));
        ev.running = rd(s, i + 10) >> 5;
        let dl = (((rd(s, i + 10) & 0x0F) << 8) | rd(s, i + 11)) as usize;
        let dend = end.min(i + 12 + dl);
        let mut j = i + 12;
        while j + 2 <= dend {
            let tag = rd(s, j);
            let l = rd(s, j + 1) as usize;
            if j + 2 + l > dend {
                break;
            }
            let d = j + 2;
            if tag == 0x4D && l >= 5 {
                ev.lang.copy_from_slice(&s[d..d + 3]);
                ev.has_lang = 1;
                let nl = rd(s, d + 3) as usize;
                if 4 + nl < l {
                    ev.has_title = 1;
                    ev.title = Span { off: (d + 4) as u32, len: nl as u32 };
                    let tl = rd(s, d + 4 + nl) as usize;
                    if 5 + nl + tl <= l {
                        ev.has_text = 1;
                        ev.text = Span { off: (d + 5 + nl) as u32, len: tl as u32 };
                    }
                }
            } else if tag == 0x4E && l >= 6 {
                let num = (rd(s, d) >> 4) as usize;
                let items = rd(s, d + 4) as usize;
                if 5 + items < l {
                    let tl = rd(s, d + 5 + items) as usize;
                    if 6 + items + tl <= l {
                        ev.ext[num] = Span { off: (d + 6 + items) as u32, len: tl as u32 };
                        ev.ext_mask |= 1 << num;
                    }
                }
            } else if tag == 0x54 && l >= 2 {
                ev.genre = rd(s, d) >> 4;
            }
            j += 2 + l;
        }
        i = dend;
        if ev.start == 0 || ev.duration <= 0 {
            continue;
        }
        if n < out.len() {
            out[n] = ev;
            n += 1;
        }
    }
    info.n_events = n as u32;
    Some(info)
}
