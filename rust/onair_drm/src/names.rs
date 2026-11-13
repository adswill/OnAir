// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Descriptive tables of the DRM signalling (ES 201 980 tables 18 and 19, clause 6.3.3, table 48) and the application names of the data services (ETSI TS 101 756
//! table 16, as DRM lists them).

/// Number of audio services for a FAC "number of services" code (-1 for the reserved codes).
pub fn fac_audio_services(c: i32) -> i32 {
    const A: [i32; 16] = [4, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, -1, 3, 3, -1, 0];
    A[(c & 15) as usize]
}

/// Number of data services for a FAC "number of services" code (-1 for the reserved codes).
pub fn fac_data_services(c: i32) -> i32 {
    const D: [i32; 16] = [0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, -1, 0, 1, -1, 4];
    D[(c & 15) as usize]
}

/// Language name (table 18).
pub fn language_name(c: i32) -> &'static [u8] {
    const N: [&[u8]; 16] = [
        b"",
        b"Arabic",
        b"Bengali",
        b"Chinese (Mandarin)",
        b"Dutch",
        b"English",
        b"French",
        b"German",
        b"Hindi",
        b"Japanese",
        b"Javanese",
        b"Korean",
        b"Portuguese",
        b"Russian",
        b"Spanish",
        b"Other",
    ];
    N[(c & 15) as usize]
}

/// Programme type name (table 19).
pub fn programme_type_name(c: i32) -> &'static [u8] {
    const N: [&[u8]; 32] = [
        b"",
        b"News",
        b"Current affairs",
        b"Information",
        b"Sport",
        b"Education",
        b"Drama",
        b"Culture",
        b"Science",
        b"Varied",
        b"Pop music",
        b"Rock music",
        b"Easy listening",
        b"Light classical",
        b"Serious classical",
        b"Other music",
        b"Weather",
        b"Finance",
        b"Children's",
        b"Social affairs",
        b"Religion",
        b"Phone-in",
        b"Travel",
        b"Leisure",
        b"Jazz",
        b"Country music",
        b"National music",
        b"Oldies",
        b"Folk music",
        b"Documentary",
        b"Alarm",
        b"Skip",
    ];
    N[(c & 31) as usize]
}

/// MSC constellation name for the FAC MSC mode field.
pub fn msc_mode_name(mode_e: bool, m: i32) -> &'static [u8] {
    if !mode_e {
        return match m {
            0 => b"64-QAM",
            3 => b"16-QAM",
            _ => b"reserved",
        };
    }
    match m {
        0 => b"16-QAM",
        3 => b"4-QAM",
        _ => b"reserved",
    }
}

/// Spectrum occupancy name (table 48).
pub fn occupancy_name(mode_e: bool, o: i32) -> &'static [u8] {
    const A: [&[u8]; 6] = [b"4.5 kHz", b"5 kHz", b"9 kHz", b"10 kHz", b"18 kHz", b"20 kHz"];
    if mode_e {
        return if o == 0 { b"100 kHz" } else { b"reserved" };
    }
    if (0..6).contains(&o) {
        A[o as usize]
    } else {
        b"reserved"
    }
}

/// Name of an application: domain 0 (DAB specified) with its 11 bit user application type.
pub fn app_name(domain: i32, app: i32) -> &'static [u8] {
    if domain != 0 {
        return if domain == 1 { b"DRM domain application" } else { b"application of another domain" };
    }
    match app {
        0x001 => b"dynamic labels",
        0x002 => b"MOT slide show",
        0x003 => b"MOT broadcast website",
        0x004 => b"TPEG",
        0x005 => b"DGPS",
        0x006 => b"TMC",
        0x007 => b"EPG",
        0x008 => b"DAB Java",
        0x009 => b"DMB",
        0x00A => b"IPDC services",
        0x00B => b"voice applications",
        0x00C => b"middleware",
        0x00D => b"filecasting",
        0x44A => b"Journaline",
        _ => {
            if app < 0 {
                b"unknown application"
            } else {
                b"unlisted application"
            }
        }
    }
}
