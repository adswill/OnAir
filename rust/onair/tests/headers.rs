// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Light consistency check between the hand-written C headers (core/include/dect2/rs/<module>.h) and each crate's `ffi.rs`:
//! every `onair_*` function declared in a header is defined as `extern "C"` in ffi.rs and vice versa, and every name carries the
//! module's prefix (`onair_<module>_`; the proto crate also owns `onair_ais_` and `onair_nmea_`).
use std::collections::BTreeSet;
use std::fs;
use std::path::PathBuf;

const MODULES: &[&str] = &[
    "proto", "adsb", "acars", "same", "pager", "ax25", "dmr", "p25", "nxdn", "dstar", "tetra", "vdl2", "hfdl", "mesh", "wisun", "uat",
    "ts", "dvbsi", "dab", "drm", "files", "net", "engine", "fec", "usb", "ring", "kris", "kaws",
];

fn ident_before_paren(line: &str, from: usize) -> Option<String> {
    let rest = &line[from..];
    let end = rest.find('(')?;
    let name: String = rest[..end].trim().chars().collect();
    (!name.is_empty() && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')).then_some(name)
}

fn header_names(text: &str) -> BTreeSet<String> {
    let mut v = BTreeSet::new();
    for line in text.lines() {
        let l = line.trim_start();
        if l.starts_with("//") || l.starts_with('#') {
            continue;
        }
        if let Some(i) = l.find("onair_") {
            let before = &l[..i];
            // a declaration: the name is preceded by a type, not by `.`/`(`/quote (calls and comments)
            if before.is_empty() || !before.ends_with(|c: char| c.is_ascii_alphanumeric() || c == '*' || c == ' ') {
                continue;
            }
            if let Some(n) = ident_before_paren(l, i) {
                v.insert(n);
            }
        }
    }
    v
}

fn ffi_names(text: &str) -> BTreeSet<String> {
    let mut v = BTreeSet::new();
    for line in text.lines() {
        if let Some(i) = line.find("extern \"C\" fn ") {
            if let Some(n) = ident_before_paren(line, i + "extern \"C\" fn ".len()) {
                v.insert(n);
            }
        }
    }
    v
}

#[test]
fn headers_match_ffi() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    for m in MODULES {
        let h = fs::read_to_string(root.join(format!("core/include/dect2/rs/{m}.h"))).unwrap_or_else(|_| panic!("missing header rs/{m}.h"));
        let f = fs::read_to_string(root.join(format!("rust/onair_{m}/src/ffi.rs")))
            .unwrap_or_else(|_| panic!("missing rust/onair_{m}/src/ffi.rs"));
        let mut hn = header_names(&h);
        let fnn: BTreeSet<String> = ffi_names(&f).into_iter().filter(|n| n.starts_with("onair_") || n == "rust_eh_personality").collect();
        hn.remove("rust_eh_personality");
        assert!(!hn.is_empty(), "{m}: no functions found in the header");
        assert_eq!(hn, fnn, "{m}: header and ffi.rs export different functions");
        let prefixes: Vec<String> = if *m == "proto" {
            vec!["onair_proto_".into(), "onair_ais_".into(), "onair_nmea_".into()]
        } else {
            vec![format!("onair_{m}_")]
        };
        for n in &hn {
            assert!(prefixes.iter().any(|p| n.starts_with(p)), "{m}: {n} does not start with {prefixes:?}");
        }
        assert!(hn.contains(&format!("onair_{}_version", if *m == "proto" { "proto" } else { m })), "{m}: no onair_{m}_version");
    }
}
