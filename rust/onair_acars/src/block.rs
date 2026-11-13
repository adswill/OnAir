// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Field parsing of a good ACARS block (ARINC 618 section 3; libacars acars.c for the layout and the H1 sublabel / MFI rule).
//!
//! Layout after SOH: mode (1), address (7), technical ack (1), label (2), block id (1), then STX and the text, then the suffix ETX (0x03) or
//! ETB (0x17). A downlink (block id is a digit) starts its text with the message number (3), the block sequence letter (1) and the flight id (6).
use crate::label;
use crate::text::Buf;

/// ASCII STX, the start of the text.
pub const STX: u8 = 0x02;
/// Longest text kept (characters).
pub const TEXT_CAP: usize = 220;
/// Room for the short reading of a text of [`TEXT_CAP`] characters (the media advisory lists up to 210 links of at most 13 characters).
pub const DECODED_CAP: usize = 3328;

/// How far [`parse`] got; each stage keeps what the stage before set (the C++ parser leaves a message half filled on an error).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    /// Too short or no ETX / ETB suffix: nothing set.
    Rejected = 0,
    /// The header was read but the text does not start with STX: mode .. downlink are set.
    Header = 1,
    /// An acknowledgement without text: header set, text empty.
    NoText = 2,
    /// Complete.
    Full = 3,
}

/// The fields of a block. Strings are fixed arrays with a length.
pub struct Fields {
    pub stage: Stage,
    pub final_block: bool,
    pub mode: u8,
    pub reg: [u8; 7],
    pub reg_len: usize,
    pub ack: u8,
    pub label: [u8; 2],
    pub block_id: u8,
    pub downlink: bool,
    /// Downlink with room for the message number and flight id.
    pub has_msg_num: bool,
    pub msg_num: [u8; 3],
    pub msg_seq: u8,
    pub flight_id: [u8; 6],
    pub sublabel: [u8; 2],
    pub sublabel_len: usize,
    pub mfi: [u8; 2],
    pub mfi_len: usize,
    pub text: [u8; TEXT_CAP],
    pub text_len: usize,
}

/// Printable reading of the text: line breaks stay (CR LF counts once), control characters and DEL become '.', at most `cap` characters.
fn printable(p: &[u8], out: &mut [u8], cap: usize) -> usize {
    let mut n = 0;
    for (i, &c) in p.iter().enumerate() {
        if n >= cap {
            break;
        }
        let ch = match c {
            b'\n' => b'\n',
            b'\r' => {
                if i + 1 >= p.len() || p[i + 1] != b'\n' {
                    b'\n'
                } else {
                    continue;
                }
            }
            c if c < 0x20 || c == 0x7f => b'.',
            c => c,
        };
        out[n] = ch;
        n += 1;
    }
    n
}

fn dots(b: &mut [u8]) {
    for c in b {
        if *c < 0x20 {
            *c = b'.';
        }
    }
}

/// Parses a good block. `t`: the bytes after SOH up to and including the suffix, parity bits removed. `decoded` receives the short reading of the text
/// (written only at [`Stage::Full`]); its length is returned beside the fields.
pub fn parse(t: &[u8], decoded: &mut [u8]) -> (Fields, usize) {
    let mut f = Fields {
        stage: Stage::Rejected,
        final_block: false,
        mode: 0,
        reg: [0; 7],
        reg_len: 0,
        ack: 0,
        label: [b' '; 2],
        block_id: 0,
        downlink: false,
        has_msg_num: false,
        msg_num: [0; 3],
        msg_seq: 0,
        flight_id: [0; 6],
        sublabel: [0; 2],
        sublabel_len: 0,
        mfi: [0; 2],
        mfi_len: 0,
        text: [0; TEXT_CAP],
        text_len: 0,
    };
    let len = t.len();
    // mode, 7 address, ack, 2 label, block id, then STX and text, then the suffix: 13 bytes at least
    if len < 13 {
        return (f, 0);
    }
    let suffix = t[len - 1];
    if suffix != 0x03 && suffix != 0x17 {
        return (f, 0);
    }
    f.final_block = suffix == 0x03;
    let n = len - 1;
    f.mode = t[0];
    // the address without its leading dots, control characters shown as dots
    let reg = &t[1..8];
    let lead = reg.iter().take_while(|&&c| c == b'.').count();
    f.reg_len = 7 - lead;
    f.reg[..f.reg_len].copy_from_slice(&reg[lead..]);
    dots(&mut f.reg[..f.reg_len]);
    f.ack = match t[8] {
        0x15 => b'!', // NAK: nothing acknowledged
        0x06 => b'^', // ACK
        c => c,
    };
    f.label = [t[9], t[10]];
    if f.label[1] == 0x7f {
        f.label[1] = b'd'; // "_<DEL>"
    }
    for c in &mut f.label {
        if *c < 0x20 || *c > 0x7e {
            *c = b'.';
        }
    }
    f.block_id = if t[11] == 0 { b' ' } else { t[11] };
    f.downlink = f.block_id.is_ascii_digit();
    f.stage = Stage::Header;
    let mut i = 12;
    if i >= n {
        f.stage = Stage::NoText; // an acknowledgement without text
        return (f, 0);
    }
    if t[i] != STX {
        return (f, 0);
    }
    i += 1;
    let mut p = &t[i..n];
    if f.downlink && p.len() >= 10 {
        f.has_msg_num = true;
        f.msg_num.copy_from_slice(&p[..3]);
        f.msg_seq = p[3];
        f.flight_id.copy_from_slice(&p[4..10]);
        dots(&mut f.msg_num);
        dots(&mut f.flight_id);
        p = &p[10..];
    }
    f.text_len = printable(p, &mut f.text, TEXT_CAP);
    let text = &f.text[..f.text_len];
    if f.label == *b"H1" {
        // sublabel and MFI, libacars acars.c: la_acars_extract_sublabel_and_mfi
        let r = text.len();
        let mut off = 0;
        if f.downlink {
            if r >= 4 && text[0] == b'#' && text[3] == b'B' {
                f.sublabel = [text[1], text[2]];
                f.sublabel_len = 2;
                off = 4;
            }
        } else if r >= 5 && text.starts_with(b"- #") {
            f.sublabel = [text[3], text[4]];
            f.sublabel_len = 2;
            off = 5;
        }
        if off != 0 && r - off >= 4 && text[off] == b'/' && text[off + 3] == b' ' {
            f.mfi = [text[off + 1], text[off + 2]];
            f.mfi_len = 2;
        }
    }
    let mut b = Buf::new(decoded);
    label::decode_text(&f.label, &f.text[..f.text_len], &mut b);
    let dl = b.len();
    f.stage = Stage::Full;
    (f, dl)
}
