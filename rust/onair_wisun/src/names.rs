// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Names of the Wi-SUN FAN / 802.15.4 enumerations used in frame text: UTT frame types (Wi-SUN FAN TPS 1.0 table 6-2), 802.15.4 frame types, channel
//! functions, regulatory domains (Wi-SUN PHY specification frequency band table), and the schedule text. The `*_z` forms end in a NUL byte so the C ABI can
//! hand out pointers that stay valid forever.
use crate::ie::Schedule;
use onair_common::text::Buf;

fn strip(s: &'static str) -> &'static str {
    &s[..s.len() - 1]
}

/// UTT frame type name with a trailing NUL: "PAN Advert", ... "Extended Data", "?" outside 0..11.
pub fn utt_name_z(t: i32) -> &'static str {
    match t {
        0 => "PAN Advert\0",
        1 => "PAN Advert Solicit\0",
        2 => "PAN Config\0",
        3 => "PAN Config Solicit\0",
        4 => "Data\0",
        5 => "Ack\0",
        6 => "EAPOL\0",
        7 => "LFN PAN Advert\0",
        8 => "LFN PAN Advert Solicit\0",
        9 => "LFN PAN Config\0",
        10 => "LFN PAN Config Solicit\0",
        11 => "Extended Data\0",
        _ => "?\0",
    }
}
/// UTT frame type name.
pub fn utt_name(t: i32) -> &'static str {
    strip(utt_name_z(t))
}

/// Short UTT name with a trailing NUL ("PA", "PAS", ... "ExtData").
pub fn utt_short_z(t: i32) -> &'static str {
    const N: [&str; 12] =
        ["PA\0", "PAS\0", "PC\0", "PCS\0", "Data\0", "Ack\0", "EAPOL\0", "LPA\0", "LPAS\0", "LPC\0", "LPCS\0", "ExtData\0"];
    if (0..12).contains(&t) {
        N[t as usize]
    } else {
        "?\0"
    }
}

/// Channel function name with a trailing NUL (0 fixed, 1 TR51CF, 2 DH1CF, 3 vendor).
pub fn ch_function_z(f: i32) -> &'static str {
    match f {
        0 => "fixed\0",
        1 => "TR51CF\0",
        2 => "DH1CF\0",
        3 => "vendor\0",
        _ => "?\0",
    }
}
/// Channel function name.
pub fn ch_function(f: i32) -> &'static str {
    strip(ch_function_z(f))
}

/// Regulatory domain name with a trailing NUL (Wi-SUN regulatory domain codes: 0x08 Australia / NZ).
pub fn reg_domain_z(c: i32) -> &'static str {
    match c {
        0x00 => "World\0",
        0x01 => "North America\0",
        0x02 => "Japan\0",
        0x03 => "Europe\0",
        0x04 => "China\0",
        0x05 => "India\0",
        0x06 => "Mexico\0",
        0x07 => "Brazil\0",
        0x08 => "Australia / NZ\0",
        0x09 => "Korea\0",
        0x0A => "Philippines\0",
        0x0B => "Malaysia\0",
        0x0C => "Hong Kong\0",
        0x0D => "Singapore\0",
        0x0E => "Thailand\0",
        0x0F => "Vietnam\0",
        0x10 => "Singapore (866 MHz)\0",
        _ => "?\0",
    }
}
/// Regulatory domain name.
pub fn reg_domain(c: i32) -> &'static str {
    strip(reg_domain_z(c))
}

/// 802.15.4 frame type name (IEEE 802.15.4-2015 table 7-1), the same text the 2.4 GHz mode uses.
pub fn frame_type_name(t: i32) -> &'static str {
    match t {
        0 => "Beacon",
        1 => "Data",
        2 => "Ack",
        3 => "MAC cmd",
        5 => "Multipurpose",
        6 => "Fragment",
        7 => "Extended",
        _ => "Reserved",
    }
}

/// One line about a schedule ("Australia / NZ class 1, fixed ch 5, dwell 255 ms"); empty when it is not present.
pub fn schedule_text(w: &mut Buf, s: &Schedule, fmt: onair_common::text::FormatF) {
    if s.present == 0 {
        return;
    }
    // the plan text, then the channel function, then the interval / dwell part, then the excluded count
    if s.ch_plan == 0 {
        w.str(reg_domain(s.reg_domain));
        w.str(" class ");
        w.idec(s.op_class as i64);
    } else if s.ch_plan == 1 {
        const SP: [i64; 4] = [200, 400, 600, 100];
        w.fixed(fmt, 3, s.ch0_mhz);
        w.str(" MHz + ");
        w.idec(s.num_channels as i64);
        w.str(" x ");
        w.idec(if (0..4).contains(&s.spacing_code) { SP[s.spacing_code as usize] } else { 0 });
        w.str(" kHz");
    } else if s.ch_plan == 2 {
        w.str(reg_domain(s.reg_domain));
        w.str(" plan ");
        w.idec(s.ch_plan_id as i64);
    }
    w.str(", ");
    if s.ch_function == 0 && s.fixed_channel >= 0 {
        w.str("fixed ch ");
        w.idec(s.fixed_channel as i64);
    } else {
        w.str(ch_function(s.ch_function));
    }
    if s.bcast_interval_ms != 0 {
        w.str(", every ");
        w.udec(s.bcast_interval_ms as u64);
        w.str(" ms, dwell ");
    } else {
        w.str(", dwell ");
    }
    w.idec(s.dwell_ms as i64);
    w.str(" ms");
    if s.excluded_count != 0 {
        w.str(", ");
        w.idec(s.excluded_count as i64);
        w.str(" excluded");
    }
}
