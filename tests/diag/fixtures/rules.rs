// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// Fixture for tools/lint/diag_check.py --self-test (Rust call forms).
fn fixture(n_bits: usize, len: usize, s: &str) -> Option<()> {
    diag_debug!(358927, "The FIG 0/1 subchannel list is {} bits, longer than its {} byte FIB, so it is skipped.", n_bits, len);
    diag!(358921, r#"A raw Rust string with "quotes" inside is one literal and passes every rule."#);
    diag!(358920, "This code ends in a zero digit and must be rejected by the number rules.");   // expect: code
    diag!(358923, s);   // expect: literal
    let _lifetime: &'static str = "a lifetime before a call does not confuse the lexer";
    None
}
