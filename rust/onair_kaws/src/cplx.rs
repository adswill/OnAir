// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Minimal complex type (f32) and helpers.
use crate::mathf;
use core::ops::{Add, AddAssign, Mul, Sub};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct C {
    pub re: f32,
    pub im: f32,
}
impl C {
    pub const ZERO: C = C { re: 0.0, im: 0.0 };
    pub fn new(re: f32, im: f32) -> C { C { re, im } }
    /// amp * exp(j phase), the phase in radians (f64 so long phase ramps stay exact).
    pub fn polar(amp: f32, phase: f64) -> C { C { re: amp * mathf::cos(phase) as f32, im: amp * mathf::sin(phase) as f32 } }
    pub fn conj(self) -> C { C { re: self.re, im: -self.im } }
    pub fn norm_sqr(self) -> f32 { self.re * self.re + self.im * self.im }
    pub fn scale(self, s: f32) -> C { C { re: self.re * s, im: self.im * s } }
}
impl Add for C { type Output = C; fn add(self, o: C) -> C { C { re: self.re + o.re, im: self.im + o.im } } }
impl Sub for C { type Output = C; fn sub(self, o: C) -> C { C { re: self.re - o.re, im: self.im - o.im } } }
impl Mul for C { type Output = C; fn mul(self, o: C) -> C { C { re: self.re * o.re - self.im * o.im, im: self.re * o.im + self.im * o.re } } }
impl AddAssign for C { fn add_assign(&mut self, o: C) { self.re += o.re; self.im += o.im; } }

/// Mean power |x|^2 of a block.
pub fn mean_power(x: &[C]) -> f32 {
    if x.is_empty() { return 0.0; }
    let mut s = 0f64;
    for v in x { s += v.norm_sqr() as f64; }
    (s / x.len() as f64) as f32
}
