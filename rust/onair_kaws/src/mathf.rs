// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Math functions for the `no_std` crate: the C library in the library build, the std methods under `cargo test`.
#[cfg(not(test))]
#[allow(unsafe_code)]
mod c {
    extern "C" {
        pub fn sin(x: f64) -> f64;
        pub fn cos(x: f64) -> f64;
        pub fn sqrt(x: f64) -> f64;
        pub fn exp(x: f64) -> f64;
        pub fn log(x: f64) -> f64;
        pub fn atan2(y: f64, x: f64) -> f64;
        pub fn floor(x: f64) -> f64;
    }
}
#[cfg(not(test))]
#[allow(unsafe_code)]
mod w {
    use super::c;
    // SAFETY: the C library math functions are pure and take plain values.
    pub fn sin(x: f64) -> f64 { unsafe { c::sin(x) } }
    pub fn cos(x: f64) -> f64 { unsafe { c::cos(x) } }
    pub fn sqrt(x: f64) -> f64 { unsafe { c::sqrt(x) } }
    pub fn exp(x: f64) -> f64 { unsafe { c::exp(x) } }
    pub fn ln(x: f64) -> f64 { unsafe { c::log(x) } }
    pub fn atan2(y: f64, x: f64) -> f64 { unsafe { c::atan2(y, x) } }
    pub fn floor(x: f64) -> f64 { unsafe { c::floor(x) } }
}
#[cfg(test)]
mod w {
    pub fn sin(x: f64) -> f64 { x.sin() }
    pub fn cos(x: f64) -> f64 { x.cos() }
    pub fn sqrt(x: f64) -> f64 { x.sqrt() }
    pub fn exp(x: f64) -> f64 { x.exp() }
    pub fn ln(x: f64) -> f64 { x.ln() }
    pub fn atan2(y: f64, x: f64) -> f64 { y.atan2(x) }
    pub fn floor(x: f64) -> f64 { x.floor() }
}
pub use w::*;
pub const PI: f64 = core::f64::consts::PI;
pub fn log10(x: f64) -> f64 { ln(x) / core::f64::consts::LN_10 }
pub fn pow10(x: f64) -> f64 { exp(x * core::f64::consts::LN_10) }
/// ln I0(x) for x >= 0 (modified Bessel function of order 0), polynomial for small x, asymptotic series for large x.
pub fn ln_i0(x: f64) -> f64 {
    if x < 3.75 {
        let t = (x / 3.75) * (x / 3.75);
        let s = 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
        ln(s)
    } else {
        x - 0.5 * ln(2.0 * PI * x) + ln(1.0 + 1.0 / (8.0 * x) + 9.0 / (128.0 * x * x))
    }
}
