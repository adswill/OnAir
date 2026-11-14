// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Diagnostic codes for Rust (docs/DIAGNOSTICS.md): the same `[E482917] message` line the C++ runtime (`dect2/common/diag.h`) writes.
//!
//! ```ignore
//! diag!(482917, "The DAB+ header 0x{:02X} is not valid, so this service stays silent.", hdr);   // always shown (warning)
//! diag_debug!(613587, "A silent drop: {} bytes were too short.", n);                              // only while debug diagnostics are on
//! ```
//!
//! Rules (`tools/lint/diag_check.py` checks them): the code is one six-digit decimal literal on the line of the call, unique in the tree
//! and never reused; the message is one string literal. The code is also checked at compile time, with the same rules as the C++ side.
//!
//! Debug diagnostics are on when the environment variable `ONAIR_DEBUG_DIAG` is set to anything but empty or `0` (read once, with the
//! `std` feature or in tests), or after [`set_debug`]. While they are off nothing is formatted. The line goes to the sink set with
//! [`set_sink`] (the C++ side routes it into the app log); without a sink and with `std` it goes to standard error. Without either
//! (the `no_std` library build) the line is dropped, so a crate never depends on where it runs.
//!
//! This is the only module of the crate with `unsafe`: one `transmute` that turns the stored sink address back into a function pointer.
#![allow(unsafe_code)]

use core::fmt::{self, Write};
use core::sync::atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering};

/// How serious a line is. `Debug` lines start with `[v] ` like the other verbose lines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Debug,
    Warn,
    Error,
}

/// A place the finished line goes to.
pub type Sink = fn(Level, &str);

/// The code rules: 100000 to 999999, last digit not 0, not within 1 of a power of two (2^16 to 2^20), more than 3 bits set, not all digits equal.
pub const fn valid_code(c: u32) -> bool {
    if c < 100_000 || c > 999_999 || c % 10 == 0 {
        return false;
    }
    let mut k = 16;
    while k <= 20 {
        let p = 1u32 << k;
        if c + 1 >= p && c <= p + 1 {
            return false;
        }
        k += 1;
    }
    if c.count_ones() <= 3 {
        return false;
    }
    let d = c % 10;
    let mut v = c;
    let mut same = true;
    while v > 0 {
        if v % 10 != d {
            same = false;
        }
        v /= 10;
    }
    !same
}

/// Compile-time check of the literal at a call site: a code that breaks the rules does not compile.
pub struct Code<const C: u32>;
impl<const C: u32> Code<C> {
    pub const VALUE: u32 = {
        assert!(valid_code(C), "diagnostic code: six digits 100000-999999, last digit not 0, not within 1 of a power of two, more than 3 bits set, not all digits the same");
        C
    };
}

static SINK: AtomicUsize = AtomicUsize::new(0);
// 0 unknown (the environment is read on first use), 1 off, 2 on
static DEBUG: AtomicU8 = AtomicU8::new(0);
static DEBUG_SET: AtomicBool = AtomicBool::new(false);

/// Sends every line to `sink` from now on (the C++ side uses it to reach the app log).
pub fn set_sink(sink: Sink) {
    SINK.store(sink as usize, Ordering::Release);
}

/// Turns debug diagnostics on or off, over the environment variable.
pub fn set_debug(on: bool) {
    DEBUG_SET.store(true, Ordering::Relaxed);
    DEBUG.store(if on { 2 } else { 1 }, Ordering::Relaxed);
}

#[cfg(any(test, feature = "std"))]
fn env_debug() -> bool {
    match std::env::var("ONAIR_DEBUG_DIAG") {
        Ok(v) => !v.is_empty() && v != "0",
        Err(_) => false,
    }
}
#[cfg(not(any(test, feature = "std")))]
fn env_debug() -> bool {
    false
}

/// True when debug diagnostics are on. One relaxed load and a branch once the environment has been read.
pub fn debug_enabled() -> bool {
    match DEBUG.load(Ordering::Relaxed) {
        2 => true,
        1 => false,
        _ => {
            let on = env_debug();
            if !DEBUG_SET.load(Ordering::Relaxed) {
                DEBUG.store(if on { 2 } else { 1 }, Ordering::Relaxed);
            }
            on
        }
    }
}

const LINE: usize = 1024;

struct Buf {
    b: [u8; LINE],
    n: usize,
}
impl Write for Buf {
    // a line longer than the buffer is cut at a character boundary; nothing is lost silently otherwise
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let room = LINE - self.n;
        let mut take = s.len().min(room);
        while take > 0 && !s.is_char_boundary(take) {
            take -= 1;
        }
        self.b[self.n..self.n + take].copy_from_slice(&s.as_bytes()[..take]);
        self.n += take;
        Ok(())
    }
}

/// Formats `[E<code>] <text>` (a debug line also gets the `[v] ` mark) and hands it to the sink. Use the macros, not this.
pub fn emit(level: Level, code: u32, args: fmt::Arguments) {
    if level == Level::Debug && !debug_enabled() {
        return;
    }
    let mut buf = Buf { b: [0; LINE], n: 0 };
    let _ = write!(buf, "[E{:06}] ", code);
    let _ = buf.write_fmt(args);
    let line = match core::str::from_utf8(&buf.b[..buf.n]) {
        Ok(s) => s,
        Err(e) => core::str::from_utf8(&buf.b[..e.valid_up_to()]).unwrap_or(""),
    };
    let s = SINK.load(Ordering::Acquire);
    if s != 0 {
        // SAFETY: SINK only ever holds 0 or the address of a `Sink` stored by `set_sink`, so the transmute restores that exact function pointer.
        let f: Sink = unsafe { core::mem::transmute::<usize, Sink>(s) };
        f(level, line);
        return;
    }
    #[cfg(any(test, feature = "std"))]
    {
        if level == Level::Debug {
            std::eprintln!("[v] {}", line);
        } else {
            std::eprintln!("{}", line);
        }
    }
}

/// The text `[E<code>] <text>` as a `String`, not logged: error out-parameters and status texts.
#[cfg(any(test, feature = "std"))]
pub fn text(code: u32, args: fmt::Arguments) -> std::string::String {
    let mut s = std::string::String::new();
    let _ = write!(s, "[E{:06}] ", code);
    let _ = s.write_fmt(args);
    s
}

/// A warning: a failure or condition the user should see in the log.
#[macro_export]
macro_rules! diag {
    ($code:literal, $($arg:tt)+) => {
        $crate::diag::emit($crate::diag::Level::Warn, $crate::diag::Code::<{ $code }>::VALUE, format_args!($($arg)+))
    };
}

/// An error: a failure that stops what the user asked for.
#[macro_export]
macro_rules! diag_error {
    ($code:literal, $($arg:tt)+) => {
        $crate::diag::emit($crate::diag::Level::Error, $crate::diag::Code::<{ $code }>::VALUE, format_args!($($arg)+))
    };
}

/// A failure that is handled quietly (a silent `Err`/`None` path): shown only while debug diagnostics are on, and nothing is formatted otherwise.
#[macro_export]
macro_rules! diag_debug {
    ($code:literal, $($arg:tt)+) => {
        if $crate::diag::debug_enabled() {
            $crate::diag::emit($crate::diag::Level::Debug, $crate::diag::Code::<{ $code }>::VALUE, format_args!($($arg)+))
        }
    };
}

/// The same `[E######] text`, returned as a `String` instead of logged (needs the `std` feature).
#[cfg(any(test, feature = "std"))]
#[macro_export]
macro_rules! diag_text {
    ($code:literal, $($arg:tt)+) => {
        $crate::diag::text($crate::diag::Code::<{ $code }>::VALUE, format_args!($($arg)+))
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static GOT: Mutex<std::vec::Vec<(Level, std::string::String)>> = Mutex::new(std::vec::Vec::new());
    fn grab(l: Level, s: &str) {
        GOT.lock().unwrap().push((l, s.into()));
    }

    #[test]
    fn code_rules_match_the_cpp_side() {
        assert!(valid_code(482917));
        assert!(!valid_code(482910)); // last digit 0
        assert!(!valid_code(131073)); // within 1 of 2^17
        assert!(!valid_code(111111)); // all digits the same
        assert!(!valid_code(99999));
        assert!(!valid_code(1_000_000));
        assert!(!valid_code(262144)); // 2^18, and few bits
    }

    #[test]
    fn lines_have_the_cpp_format_and_the_debug_switch_works() {
        set_sink(grab);
        set_debug(false);
        crate::diag!(482917, "The value {} is out of range, so it was ignored.", 7);
        crate::diag_debug!(613587, "A quiet drop of {} bytes.", 3);
        assert_eq!(GOT.lock().unwrap().len(), 1);
        assert_eq!(GOT.lock().unwrap()[0], (Level::Warn, "[E482917] The value 7 is out of range, so it was ignored.".into()));
        set_debug(true);
        crate::diag_debug!(613587, "A quiet drop of {} bytes.", 3);
        crate::diag_error!(357821, "The decoder stopped for good.");
        let g = GOT.lock().unwrap();
        assert_eq!(g.len(), 3);
        assert_eq!(g[1], (Level::Debug, "[E613587] A quiet drop of 3 bytes.".into()));
        assert_eq!(g[2].0, Level::Error);
        drop(g);
        assert_eq!(crate::diag_text!(357821, "Text {}.", "here"), "[E357821] Text here.");
    }
}
