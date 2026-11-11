//! PSI / SI section parsers: PAT, PMT (ISO/IEC 13818-1 section 2.4.4), SDT and NIT (ETSI EN 300 468 sections 5.2.3 and 5.2.1), the ATSC terrestrial /
//! cable virtual channel table (A/65 section 6.3) and the TDT (EN 300 468 section 5.2.5). Every parser takes the whole section (header and CRC included,
//! the CRC already checked by the caller) and writes plain records; text fields are `Span`s into the section. Reads past the end give 0.
use crate::{bcd, rd, rd16, Span};

/// One PAT program entry.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PatEntry {
    /// program_number (never 0: the network PID entry is skipped).
    pub prog: i32,
    /// program_map_PID.
    pub pid: i32,
}

/// Parse a PAT section (`s.len() >= 8`); returns the transport_stream_id and the number of entries written (those past `out.len()` are dropped).
pub fn parse_pat(s: &[u8], out: &mut [PatEntry]) -> Option<(i32, usize)> {
    let len = s.len();
    if len < 8 {
        return None;
    }
    let mut n = 0;
    let mut i = 8;
    while i + 4 <= len.saturating_sub(4) {
        let prog = rd16(s, i);
        let pid = ((rd(s, i + 2) & 0x1F) << 8) | rd(s, i + 3);
        i += 4;
        if prog == 0 {
            continue;
        }
        if n < out.len() {
            out[n] = PatEntry { prog, pid };
            n += 1;
        }
    }
    Some((rd16(s, 3), n))
}

/// Stream kind codes of `PmtStream::kind`.
pub const KIND_DATA: u8 = 0;
/// Video.
pub const KIND_VIDEO: u8 = 1;
/// Audio.
pub const KIND_AUDIO: u8 = 2;
/// DVB subtitles.
pub const KIND_SUBTITLES: u8 = 3;
/// Teletext.
pub const KIND_TELETEXT: u8 = 4;

/// One elementary stream of a PMT.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PmtStream {
    /// stream_type.
    pub stream_type: i32,
    /// elementary_PID.
    pub pid: i32,
    /// `KIND_*`.
    pub kind: u8,
    /// Number of valid bytes of `codec`.
    pub codec_len: u8,
    /// 3 language bytes are valid.
    pub has_lang: u8,
    /// ISO 639 language code (raw bytes).
    pub lang: [u8; 3],
    /// Codec name, ASCII ("H.264", "type 0x06"...).
    pub codec: [u8; 24],
    /// Index of its first entry in the sub array.
    pub sub_first: u32,
    /// Number of sub entries.
    pub sub_count: u32,
}

/// One language / page entry of a subtitling (0x59) or teletext (0x56) descriptor.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PmtSub {
    /// 1 for teletext, 0 for DVB subtitles.
    pub teletext: u8,
    /// Language code.
    pub lang: [u8; 3],
    /// subtitling_type, or the teletext_type (descriptor byte >> 3).
    pub typ: i32,
    /// composition_page_id, or the teletext page 100..899.
    pub page: i32,
    /// ancillary_page_id (DVB only).
    pub anc_page: i32,
}

/// Header fields of a parsed PMT.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PmtInfo {
    /// program_number.
    pub prog: i32,
    /// PCR_PID.
    pub pcr_pid: i32,
    /// A CA descriptor (0x09) was seen at program or stream level.
    pub ca: u8,
    /// Streams written.
    pub n_streams: u32,
    /// Subs written.
    pub n_subs: u32,
}

/// Codec name and kind of a stream_type (ISO/IEC 13818-1 table 2-34 plus the Chinese AVS family, GB/T 17975.1).
fn stream_kind(st: i32) -> (&'static [u8], u8) {
    match st {
        0x01 => (b"MPEG-1 video", KIND_VIDEO),
        0x02 => (b"MPEG-2", KIND_VIDEO),
        0x03 => (b"MPEG-1 audio", KIND_AUDIO),
        0x04 => (b"MPEG audio", KIND_AUDIO),
        0x0F => (b"AAC", KIND_AUDIO),
        0x11 => (b"HE-AAC", KIND_AUDIO),
        0x1B => (b"H.264", KIND_VIDEO),
        0x24 => (b"HEVC", KIND_VIDEO),
        0x81 => (b"AC-3", KIND_AUDIO),
        0x87 => (b"E-AC-3", KIND_AUDIO),
        // DRA sound (GB/T 22726) in Chinese streams; the registration descriptor "DRA1" (below) names it too. Stream type from memory of GY/T 220.1, unchecked.
        0x14 => (b"DRA", KIND_AUDIO),
        0x42 => (b"AVS", KIND_VIDEO),
        0xD2 => (b"AVS2", KIND_VIDEO),
        0xD4 => (b"AVS3", KIND_VIDEO),
        _ => (b"", KIND_DATA),
    }
}

/// Set the codec name of a stream.
fn set_codec(st: &mut PmtStream, name: &[u8]) {
    st.codec = [0; 24];
    st.codec[..name.len()].copy_from_slice(name);
    st.codec_len = name.len() as u8;
}

/// "type 0x%02X" for a stream type with no known codec.
fn type_name(st: &mut PmtStream) {
    const H: &[u8; 16] = b"0123456789ABCDEF";
    let t = st.stream_type as u8;
    set_codec(st, &[b't', b'y', b'p', b'e', b' ', b'0', b'x', H[(t >> 4) as usize], H[(t & 15) as usize]]);
}

/// Parse a PMT section (`s.len() >= 12`): streams with their codec, kind, language and the subtitling / teletext page lists. Streams past the array
/// capacity are dropped; `n_subs` counts every sub entry (overlapping descriptors of corrupt sections can exceed the section length / 5), only the first
/// `subs.len()` are stored: call again with a bigger array when `n_subs > subs.len()`.
pub fn parse_pmt(s: &[u8], streams: &mut [PmtStream], subs: &mut [PmtSub]) -> Option<PmtInfo> {
    let len = s.len();
    if len < 12 {
        return None;
    }
    let mut info = PmtInfo { prog: rd16(s, 3), pcr_pid: ((rd(s, 8) & 0x1F) << 8) | rd(s, 9), ..Default::default() };
    let pil = (((rd(s, 10) & 0x0F) << 8) | rd(s, 11)) as usize;
    let mut ca = false;
    let mut i = 12;
    while i + 2 <= 12 + pil && i + 2 <= len {
        let tag = rd(s, i);
        let l = rd(s, i + 1) as usize;
        if tag == 0x09 {
            ca = true;
        }
        i += 2 + l;
    }
    let mut i = 12 + pil;
    let (mut ns, mut nb) = (0usize, 0usize);
    while i + 5 <= len.saturating_sub(4) {
        let mut st = PmtStream {
            stream_type: rd(s, i),
            pid: ((rd(s, i + 1) & 0x1F) << 8) | rd(s, i + 2),
            sub_first: nb as u32,
            ..Default::default()
        };
        let esl = (((rd(s, i + 3) & 0x0F) << 8) | rd(s, i + 4)) as usize;
        let (name, kind) = stream_kind(st.stream_type);
        set_codec(&mut st, name);
        st.kind = kind;
        let mut j = i + 5;
        while j + 2 <= i + 5 + esl && j + 2 <= len {
            let tag = rd(s, j);
            let l = rd(s, j + 1) as usize;
            if j + 2 + l > len {
                break;
            }
            let d = &s[j + 2..j + 2 + l];
            let lang3 = |st: &mut PmtStream| {
                if l >= 3 {
                    st.lang.copy_from_slice(&d[..3]);
                    st.has_lang = 1;
                }
            };
            match tag {
                0x09 => ca = true,
                0x05 => {
                    if l >= 4 && &d[..4] == b"DRA1" {
                        set_codec(&mut st, b"DRA");
                        st.kind = KIND_AUDIO;
                    }
                }
                0x0A => lang3(&mut st),
                0x6A => {
                    set_codec(&mut st, b"AC-3");
                    st.kind = KIND_AUDIO;
                }
                0x7A => {
                    set_codec(&mut st, b"E-AC-3");
                    st.kind = KIND_AUDIO;
                }
                0x7B => {
                    set_codec(&mut st, b"DTS");
                    st.kind = KIND_AUDIO;
                }
                0x7C => {
                    set_codec(&mut st, b"AAC");
                    st.kind = KIND_AUDIO;
                }
                // extension descriptor: AC-4 (descriptor_tag_extension 0x15, EN 300 468 annex D.7)
                0x7F if l >= 1 && d[0] == 0x15 => {
                    set_codec(&mut st, b"AC-4");
                    st.kind = KIND_AUDIO;
                }
                0x59 => {
                    set_codec(&mut st, b"DVB subtitles");
                    st.kind = KIND_SUBTITLES;
                    lang3(&mut st);
                    let mut k = 0;
                    while k + 8 <= l {
                        if nb < subs.len() {
                            subs[nb] = PmtSub {
                                teletext: 0,
                                lang: [d[k], d[k + 1], d[k + 2]],
                                typ: d[k + 3] as i32,
                                page: rd16(d, k + 4),
                                anc_page: rd16(d, k + 6),
                            };
                        }
                        nb += 1;
                        k += 8;
                    }
                }
                0x56 => {
                    set_codec(&mut st, b"Teletext");
                    st.kind = KIND_TELETEXT;
                    lang3(&mut st);
                    let mut k = 0;
                    while k + 5 <= l {
                        let mag = if d[k + 3] & 7 != 0 { (d[k + 3] & 7) as i32 } else { 8 };
                        let b = d[k + 4] as i32;
                        k += 5;
                        if (b >> 4) > 9 || (b & 15) > 9 {
                            continue;
                        }
                        if nb < subs.len() {
                            subs[nb] = PmtSub {
                                teletext: 1,
                                lang: [d[k - 5], d[k - 4], d[k - 3]],
                                typ: (d[k - 2] >> 3) as i32,
                                page: mag * 100 + (b >> 4) * 10 + (b & 15),
                                anc_page: 0,
                            };
                        }
                        nb += 1;
                    }
                }
                0x46 => {
                    set_codec(&mut st, b"VBI teletext");
                    st.kind = KIND_TELETEXT;
                }
                _ => {}
            }
            j += 2 + l;
        }
        if st.codec_len == 0 {
            type_name(&mut st);
        }
        st.sub_count = (nb as u32) - st.sub_first;
        if ns < streams.len() {
            streams[ns] = st;
            ns += 1;
        }
        i += 5 + esl;
    }
    info.ca = ca as u8;
    info.n_streams = ns as u32;
    info.n_subs = nb as u32;
    Some(info)
}

/// One SDT service entry.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SdtService {
    /// service_id.
    pub sid: i32,
    /// free_CA_mode.
    pub free_ca: u8,
    /// A service descriptor (0x48) gave the type.
    pub has_type: u8,
    /// The provider name is present.
    pub has_provider: u8,
    /// The service name is present.
    pub has_name: u8,
    /// service_type.
    pub typ: i32,
    /// service_provider_name bytes.
    pub provider: Span,
    /// service_name bytes.
    pub name: Span,
}

/// Parse an SDT section (`s.len() >= 11`): the transport_stream_id, original_network_id and the services (service descriptor 0x48 only).
pub fn parse_sdt(s: &[u8], out: &mut [SdtService]) -> Option<(i32, i32, usize)> {
    let len = s.len();
    if len < 11 {
        return None;
    }
    let mut n = 0;
    let mut i = 11;
    while i + 5 <= len.saturating_sub(4) {
        let dl = (((rd(s, i + 3) & 0x0F) << 8) | rd(s, i + 4)) as usize;
        let mut sv = SdtService { sid: rd16(s, i), free_ca: ((rd(s, i + 3) & 0x10) != 0) as u8, ..Default::default() };
        let mut j = i + 5;
        while j + 2 <= i + 5 + dl && j + 2 <= len {
            let tag = rd(s, j);
            let l = rd(s, j + 1) as usize;
            if j + 2 + l > len {
                break;
            }
            if tag == 0x48 && l >= 3 {
                let d = j + 2;
                sv.has_type = 1;
                sv.typ = rd(s, d);
                let pl = rd(s, d + 1) as usize;
                if 2 + pl < l {
                    sv.has_provider = 1;
                    sv.provider = Span { off: (d + 2) as u32, len: pl as u32 };
                    let nl = rd(s, d + 2 + pl) as usize;
                    if 3 + pl + nl <= l {
                        sv.has_name = 1;
                        sv.name = Span { off: (d + 3 + pl) as u32, len: nl as u32 };
                    }
                }
            }
            j += 2 + l;
        }
        if n < out.len() {
            out[n] = sv;
            n += 1;
        }
        i += 5 + dl;
    }
    Some((rd16(s, 3), rd16(s, 8), n))
}

/// Logical channel number entry (descriptor 0x83, EACEM / UK): service and number.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct NitLcn {
    /// service_id.
    pub sid: i32,
    /// logical_channel_number (10 bits).
    pub lcn: i32,
}

/// Result of an NIT parse besides the LCN list.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct NitInfo {
    /// A network name descriptor (0x40) was found.
    pub has_name: u8,
    /// network_name bytes of the last one.
    pub name: Span,
    /// LCN entries written.
    pub n_lcn: u32,
}

/// Parse an NIT section (`s.len() >= 10`): the network name and the logical channel numbers of the transport stream loop. Descriptors that run past the
/// end of the section are cut at it. Corrupt length fields make descriptors overlap, so there can be more entries than the section length suggests:
/// `n_lcn` counts all of them, only the first `out.len()` are stored (call again with a bigger array when `n_lcn > out.len()`).
pub fn parse_nit(s: &[u8], out: &mut [NitLcn]) -> Option<NitInfo> {
    let len = s.len();
    if len < 10 {
        return None;
    }
    let mut info = NitInfo::default();
    let ndl = (((rd(s, 8) & 0x0F) << 8) | rd(s, 9)) as usize;
    let mut j = 10;
    while j + 2 <= 10 + ndl && j + 2 <= len {
        let tag = rd(s, j);
        let l = rd(s, j + 1) as usize;
        if tag == 0x40 {
            info.has_name = 1;
            info.name = Span { off: (j + 2) as u32, len: l.min(len - j - 2) as u32 };
        }
        j += 2 + l;
    }
    let mut n = 0usize;
    let mut i = 10 + ndl;
    if i + 2 <= len {
        let tsl = (((rd(s, i) & 0x0F) << 8) | rd(s, i + 1)) as usize;
        i += 2;
        let end = len.saturating_sub(4).min(i + tsl);
        while i + 6 <= end {
            let dl = (((rd(s, i + 4) & 0x0F) << 8) | rd(s, i + 5)) as usize;
            let mut j = i + 6;
            while j + 2 <= i + 6 + dl && j + 2 <= len {
                let tag = rd(s, j);
                let l = (rd(s, j + 1) as usize).min(len - j - 2);
                if tag == 0x83 {
                    let mut k = 0;
                    while k + 4 <= l {
                        if n < out.len() {
                            out[n] = NitLcn { sid: rd16(s, j + 2 + k), lcn: ((rd(s, j + 4 + k) & 0x03) << 8) | rd(s, j + 5 + k) };
                        }
                        n += 1;
                        k += 4;
                    }
                }
                j += 2 + rd(s, j + 1) as usize;
            }
            i += 6 + dl;
        }
    }
    info.n_lcn = n as u32;
    Some(info)
}

/// One ATSC virtual channel.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Tvct {
    /// 0 .. 7 name characters, ASCII; anything else became '?'.
    pub name: [u8; 8],
    /// Valid bytes of `name`.
    pub name_len: u8,
    /// major_channel_number.
    pub major: i32,
    /// minor_channel_number.
    pub minor: i32,
    /// program_number.
    pub prog: i32,
    /// service_type (2 digital TV, 3 audio, 4 data).
    pub stype: i32,
}

/// Parse a TVCT / CVCT section (`s.len() >= 14`): returns the transport_stream_id and the channels that map to a program (program_number not 0 or 0xFFFF).
pub fn parse_tvct(s: &[u8], out: &mut [Tvct]) -> Option<(i32, usize)> {
    let len = s.len();
    if len < 14 {
        return None;
    }
    let cnt = rd(s, 9);
    let mut i = 10usize;
    let mut n = 0;
    for _ in 0..cnt {
        if i + 32 > len.saturating_sub(4) {
            break;
        }
        let mut c = Tvct::default();
        for k in 0..7 {
            let (hi, lo) = (rd(s, i + 2 * k), rd(s, i + 2 * k + 1));
            if hi == 0 && lo == 0 {
                break;
            }
            c.name[c.name_len as usize] = if hi == 0 && (0x20..0x7F).contains(&lo) { lo as u8 } else { b'?' };
            c.name_len += 1;
        }
        c.major = ((rd(s, i + 14) & 0x0F) << 6) | (rd(s, i + 15) >> 2);
        c.minor = ((rd(s, i + 15) & 0x03) << 8) | rd(s, i + 16);
        c.prog = rd16(s, i + 24);
        c.stype = rd(s, i + 27) & 0x3F;
        let dl = (((rd(s, i + 30) & 0x03) << 8) | rd(s, i + 31)) as usize;
        if c.prog != 0 && c.prog != 0xFFFF && n < out.len() {
            out[n] = c;
            n += 1;
        }
        i += 32 + dl;
    }
    Some((rd16(s, 3), n))
}

/// Result of a TDT / TOT parse.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Tdt {
    /// Seconds since 1970 (UTC).
    pub unix: i64,
    /// Valid bytes of `text`.
    pub text_len: u8,
    /// "YYYY-MM-DD hh:mm:ss UTC".
    pub text: [u8; 32],
}

/// Append a byte unless the buffer is full.
fn push(out: &mut [u8; 32], n: &mut usize, b: u8) {
    if *n < out.len() {
        out[*n] = b;
        *n += 1;
    }
}

/// Write `v` like printf `%0<w>d` into `out`.
fn put_int(out: &mut [u8; 32], n: &mut usize, v: i32, w: usize) {
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
        push(out, n, b'-');
    }
    let digits_w = if v < 0 { w.saturating_sub(1) } else { w };
    for _ in t..digits_w {
        push(out, n, b'0');
    }
    while t > 0 {
        t -= 1;
        push(out, n, tmp[t]);
    }
}

/// Parse a TDT / TOT (`s.len() >= 8`): the UTC time as text and Unix seconds. The date is the MJD conversion of EN 300 468 Annex C.
pub fn parse_tdt(s: &[u8]) -> Option<Tdt> {
    if s.len() < 8 {
        return None;
    }
    let mjd = rd16(s, 3);
    let (hh, mm, ss) = (bcd(rd(s, 5)), bcd(rd(s, 6)), bcd(rd(s, 7)));
    let mut y = ((mjd as f64 - 15078.2) / 365.25) as i32;
    let mut m = ((mjd as f64 - 14956.1 - ((y as f64 * 365.25) as i32) as f64) / 30.6001) as i32;
    let d = mjd - 14956 - (y as f64 * 365.25) as i32 - (m as f64 * 30.6001) as i32;
    let k = if m == 14 || m == 15 { 1 } else { 0 };
    y += k;
    m = m - 1 - k * 12;
    let mut t = Tdt { unix: (mjd as i64 - 40587) * 86400 + (hh * 3600 + mm * 60 + ss) as i64, ..Default::default() };
    let mut n = 0usize;
    put_int(&mut t.text, &mut n, 1900 + y, 4);
    push(&mut t.text, &mut n, b'-');
    put_int(&mut t.text, &mut n, m, 2);
    push(&mut t.text, &mut n, b'-');
    put_int(&mut t.text, &mut n, d, 2);
    push(&mut t.text, &mut n, b' ');
    put_int(&mut t.text, &mut n, hh, 2);
    push(&mut t.text, &mut n, b':');
    put_int(&mut t.text, &mut n, mm, 2);
    push(&mut t.text, &mut n, b':');
    put_int(&mut t.text, &mut n, ss, 2);
    for &c in b" UTC" {
        push(&mut t.text, &mut n, c);
    }
    t.text_len = n as u8;
    Some(t)
}
