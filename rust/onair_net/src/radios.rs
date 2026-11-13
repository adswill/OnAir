// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The list of network radios the settings keep as text: "rtltcp,host,port;pluto,host,0;soapyremote,host,port,remote driver;..." (parse and format), the clean-up of a typed
//! host name, and the type tables (key, display name, default port and host). Bad items are skipped.
use crate::{atoi, Span};

/// PlutoSDR.
pub const PLUTO: u32 = 0;
/// rtl_tcp.
pub const RTL_TCP: u32 = 1;
/// SoapyRemote.
pub const SOAPY_REMOTE: u32 = 2;
/// SpyServer.
pub const SPY_SERVER: u32 = 3;
/// Hermes Lite 2.
pub const HERMES_LITE2: u32 = 4;
/// Red Pitaya.
pub const RED_PITAYA: u32 = 5;
/// SDR++ server.
pub const SDRPP_SERVER: u32 = 6;
/// SDRplay nRSP-ST (SDRconnect WebSocket API).
pub const NRSP_ST: u32 = 7;
/// KiwiSDR (HF receiver on the network, WebSocket on port 8073); its `remote` text is the password, if it has one.
pub const KIWI_SDR: u32 = 8;
/// A SoapySDR device given by its own argument string ("type=uhd,addr=10.0.0.55"); the host is only a label, the `remote` text is the arguments.
pub const SOAPY_ARGS: u32 = 9;
/// TCI (Transceiver Control Interface of ExpertSDR3 and others; WebSocket on port 50001), receive only.
pub const TCI: u32 = 10;

/// The key of a type in the settings text (an unknown type is written as rtl_tcp).
pub fn type_key(t: u32) -> &'static str {
    match t {
        PLUTO => "pluto",
        SOAPY_REMOTE => "soapyremote",
        SPY_SERVER => "spyserver",
        HERMES_LITE2 => "hl2",
        RED_PITAYA => "redpitaya",
        SDRPP_SERVER => "sdrpp",
        NRSP_ST => "nrspst",
        KIWI_SDR => "kiwisdr",
        SOAPY_ARGS => "soapyargs",
        TCI => "tci",
        _ => "rtltcp",
    }
}

/// The name of a type in the radio list.
pub fn type_name(t: u32) -> &'static str {
    match t {
        PLUTO => "PlutoSDR",
        SOAPY_REMOTE => "SoapyRemote",
        SPY_SERVER => "SpyServer",
        HERMES_LITE2 => "Hermes Lite 2",
        RED_PITAYA => "Red Pitaya",
        SDRPP_SERVER => "SDR++ server",
        NRSP_ST => "SDRplay nRSP-ST (SDRconnect)",
        KIWI_SDR => "KiwiSDR",
        SOAPY_ARGS => "SoapySDR (device string)",
        TCI => "TCI (network)",
        _ => "rtl_tcp",
    }
}

/// The default port of a type (0 for a Pluto).
pub fn default_port(t: u32) -> i32 {
    match t {
        PLUTO => 0,
        SOAPY_REMOTE => 55132,
        SPY_SERVER => 5555,
        HERMES_LITE2 | RED_PITAYA => 1024,
        SDRPP_SERVER => 5259,
        NRSP_ST => 5454,
        KIWI_SDR => 8073,
        SOAPY_ARGS => 0,
        TCI => 50001,
        _ => 1234,
    }
}

/// The default host of a type ("192.168.2.1" for a Pluto).
pub fn default_host(t: u32) -> &'static str {
    if t == PLUTO {
        "192.168.2.1"
    } else {
        ""
    }
}

/// The port connected to: `port` when it is positive, else a Pluto's iiod port 30431 or the type's default.
pub fn port_of(t: u32, port: i32) -> i32 {
    if port > 0 {
        port
    } else if t == PLUTO {
        30431
    } else {
        default_port(t)
    }
}

fn filtered(h: &[u8]) -> impl Iterator<Item = u8> + Clone + '_ {
    h.iter().copied().filter(|c| !matches!(c, b' ' | b'\t' | b',' | b';'))
}

/// The host as typed, cleaned: no spaces, tabs, commas or semicolons, then a leading "ip:" (a Pluto's libiio URI) and a leading "tcp://" (a SoapyRemote URL) dropped
/// (in that order, one each). Returns the cleaned bytes as an iterator.
fn cleaned(h: &[u8]) -> impl Iterator<Item = u8> + Clone + '_ {
    let f = filtered(h);
    let mut skip = 0usize;
    for pre in [&b"ip:"[..], &b"tcp://"[..]] {
        if f.clone().skip(skip).take(pre.len()).eq(pre.iter().copied()) {
            skip += pre.len();
        }
    }
    f.skip(skip)
}

/// Cleans a typed host name into `out`; returns its length (at most `h.len()`), or `usize::MAX` when `out` is too short.
pub fn clean_host(h: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0usize;
    for c in cleaned(h) {
        if n >= out.len() {
            return usize::MAX;
        }
        out[n] = c;
        n += 1;
    }
    n
}

/// One radio of the list. `host` is a range of the arena the parser wrote (or of the caller's pool when formatting), `remote` a range of the text.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Radio {
    /// Type (`PLUTO` ...).
    pub kind: u32,
    /// Port, 0 for the type's default.
    pub port: i32,
    /// Host name.
    pub host: Span,
    /// SoapyRemote only: which radio of the server (may hold commas).
    pub remote: Span,
}

fn type_from_key(t: &[u8]) -> Option<u32> {
    Some(match t {
        b"pluto" => PLUTO,
        b"rtltcp" => RTL_TCP,
        b"soapyremote" => SOAPY_REMOTE,
        b"spyserver" => SPY_SERVER,
        b"hl2" => HERMES_LITE2,
        b"redpitaya" => RED_PITAYA,
        b"sdrpp" => SDRPP_SERVER,
        b"nrspst" => NRSP_ST,
        b"kiwisdr" => KIWI_SDR,
        b"soapyargs" => SOAPY_ARGS,
        b"tci" => TCI,
        _ => return None,
    })
}

/// Parses the list. Radios go to `radios`, their cleaned host names to `arena` (`s.len()` bytes are enough). Returns the number of radios, which is at most the number of
/// items; `usize::MAX` when an array is too small (`radios.len()` = number of ';' + 1 suffices).
pub fn parse(s: &[u8], radios: &mut [Radio], arena: &mut [u8]) -> usize {
    let mut n = 0usize;
    let mut used = 0usize;
    let mut pos = 0usize;
    while pos <= s.len() {
        let e = s[pos..].iter().position(|&c| c == b';').map_or(s.len(), |p| p + pos);
        let item = &s[pos..e];
        let base = pos;
        pos = e + 1;
        let Some(c1) = item.iter().position(|&c| c == b',') else { continue };
        let Some(c2) = item[c1 + 1..].iter().position(|&c| c == b',').map(|p| p + c1 + 1) else { continue };
        let c3 = item[c2 + 1..].iter().position(|&c| c == b',').map(|p| p + c2 + 1);
        let Some(kind) = type_from_key(&item[..c1]) else { continue };
        let raw_host = &item[c1 + 1..c2];
        if arena.len() < used + raw_host.len() {
            return usize::MAX;
        }
        let hl = clean_host(raw_host, &mut arena[used..used + raw_host.len()]);
        let mut port = atoi(&item[c2 + 1..]);
        let mut remote = Span::default();
        if let (Some(c3), SOAPY_REMOTE | KIWI_SDR | SOAPY_ARGS) = (c3, kind) {
            remote = Span { off: (base + c3 + 1) as u32, len: (item.len() - c3 - 1) as u32 };
        }
        if !(0..=65535).contains(&port) {
            port = 0;
        }
        if hl == 0 {
            continue;
        }
        let host = Span { off: used as u32, len: hl as u32 };
        let r = Radio { kind, port, host, remote };
        let same = |o: &Radio| {
            o.kind == r.kind
                && o.port == r.port
                && arena[o.host.off as usize..(o.host.off + o.host.len) as usize] == arena[used..used + hl]
                && s[o.remote.off as usize..(o.remote.off + o.remote.len) as usize]
                    == s[r.remote.off as usize..(r.remote.off + r.remote.len) as usize]
        };
        if radios[..n].iter().any(same) {
            continue;
        }
        if n >= radios.len() {
            return usize::MAX;
        }
        radios[n] = r;
        n += 1;
        used += hl;
    }
    n
}

/// Formats a list (`host` and `remote` are ranges of `pool`): items with an empty cleaned host are left out, a SoapyRemote's remote text (a KiwiSDR's password) loses its ';', a port of 0 or
/// less is written as 0. Writes up to `out.len()` bytes and returns the full length.
pub fn format(radios: &[Radio], pool: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0usize;
    let mut first = true;
    let mut put = |b: &[u8], n: &mut usize| {
        for &c in b {
            if *n < out.len() {
                out[*n] = c;
            }
            *n += 1;
        }
    };
    for r in radios {
        let sl = |s: Span| pool.get(s.off as usize..s.off as usize + s.len as usize).unwrap_or(&[]);
        let host = cleaned(sl(r.host));
        if host.clone().next().is_none() {
            continue;
        }
        if !first {
            put(b";", &mut n);
        }
        first = false;
        put(type_key(r.kind).as_bytes(), &mut n);
        put(b",", &mut n);
        for c in host {
            put(&[c], &mut n);
        }
        put(b",", &mut n);
        let mut digits = [0u8; 12];
        let mut v = if r.port > 0 { r.port as u32 } else { 0 };
        let mut d = digits.len();
        loop {
            d -= 1;
            digits[d] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        put(&digits[d..], &mut n);
        if r.kind == SOAPY_REMOTE || r.kind == KIWI_SDR || r.kind == SOAPY_ARGS {
            let rem = sl(r.remote);
            if rem.iter().any(|&c| c != b';') {
                put(b",", &mut n);
                for &c in rem {
                    if c != b';' {
                        put(&[c], &mut n);
                    }
                }
            }
        }
    }
    n
}

#[cfg(test)]
mod kiwi_tests {
    use super::*;

    #[test]
    fn kiwisdr_keeps_its_password_and_the_tables_name_it() {
        let text = b"kiwisdr,kiwi.example.org,8074,pass word;kiwisdr,h2,0;rtltcp,h3,1,notkept";
        let mut radios = [Radio::default(); 4];
        let mut arena = vec![0u8; text.len()];
        let n = parse(text, &mut radios, &mut arena);
        assert_eq!(n, 3);
        assert_eq!(radios[0].kind, KIWI_SDR);
        let rem = |r: &Radio| &text[r.remote.off as usize..(r.remote.off + r.remote.len) as usize];
        assert_eq!(rem(&radios[0]), b"pass word");
        assert_eq!(radios[1].remote.len, 0);
        assert_eq!(radios[2].remote.len, 0);
        assert_eq!((type_key(KIWI_SDR), type_name(KIWI_SDR), default_port(KIWI_SDR)), ("kiwisdr", "KiwiSDR", 8073));
    }

    #[test]
    fn tci_is_listed_and_round_trips() {
        let text = b"tci,192.168.1.5,50001;tci,h2,0,ignored";
        let mut radios = [Radio::default(); 3];
        let mut arena = vec![0u8; text.len()];
        let n = parse(text, &mut radios, &mut arena);
        assert_eq!(n, 2);
        assert_eq!((radios[0].kind, radios[1].remote.len), (TCI, 0));
        assert_eq!((type_key(TCI), type_name(TCI), default_port(TCI)), ("tci", "TCI (network)", 50001));
        let mut out = [0u8; 64];
        let len = format(&radios[..1], &arena, &mut out);
        assert_eq!(&out[..len], b"tci,192.168.1.5,50001");
    }
}

#[cfg(test)]
mod soapy_args_tests {
    use super::*;

    #[test]
    fn device_string_round_trips() {
        let s = b"soapyargs,custom,0,type=uhd,addr=10.0.0.55";
        let mut radios = [Radio::default(); 2];
        let mut arena = [0u8; 64];
        assert_eq!(parse(s, &mut radios, &mut arena), 1);
        assert_eq!(radios[0].kind, SOAPY_ARGS);
        let r = radios[0].remote;
        assert_eq!(&s[r.off as usize..(r.off + r.len) as usize], b"type=uhd,addr=10.0.0.55");
    }
}
