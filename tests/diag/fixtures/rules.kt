// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
// Fixture for tools/lint/diag_check.py --self-test (Kotlin call forms).
fun fixture(path: String, e: Throwable) {
    Diag.e(274913, "The IQ recording $path could not be written, so the recording stops.", e)
    Diag.d(715349) { "The USB device ${path} answered the control request late, so it is opened again." }
    Diag.w(274913, "The second use of a code must be reported by the uniqueness rule.")   // expect: dup-code
    Diag.w(274917, "The recording " + path + " is joined with plus signs, which the rule forbids.")   // expect: literal
    Diag.d(715343) { "short" }   // expect: sentence
    val t = Diag.text(715347, """A raw Kotlin string with $path is one literal and passes every rule.""")
}
