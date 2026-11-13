// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DVB subtitle pixels and colours (ETSI EN 300 743): the default 2-, 4- and 8-bit colour tables (section 10), YCrCbT to RGBA (ITU-R BT.601 studio swing),
//! the run-length coded pixel data sub-blocks of an object (2-, 4- and 8-bit code strings, map tables, section 7.2.5.2), index to RGBA conversion of a region
//! and alpha blending of regions into a display.

/// Largest integer not above `x` for the small finite values this module uses (no libm in `no_std`).
fn floor(x: f64) -> f64 {
    let t = x as i64 as f64;
    if t > x {
        t - 1.0
    } else {
        t
    }
}

/// YCrCbT to RGBA as `r | g << 8 | b << 16 | a << 24`; Y = 0 is fully transparent whatever T says.
pub fn ycrcb(y: i32, cr: i32, cb: i32, t: i32) -> u32 {
    if y == 0 {
        return 0;
    }
    let yy = (y - 16) as f64 * 255.0 / 219.0;
    let u = cb as f64 - 128.0;
    let v = cr as f64 - 128.0;
    let cl = |x: f64| -> u8 {
        let f = floor(x + 0.5);
        let m = if f < 255.0 { f } else { 255.0 };
        (if 0.0 < m { m } else { 0.0 }) as u8
    };
    let r = cl(yy + 1.402 * v * 255.0 / 224.0);
    let g = cl(yy - 0.344136 * u * 255.0 / 224.0 - 0.714136 * v * 255.0 / 224.0);
    let b = cl(yy + 1.772 * u * 255.0 / 224.0);
    let a = (255 - t.clamp(0, 255)) as u8;
    r as u32 | (g as u32) << 8 | (b as u32) << 16 | (a as u32) << 24
}

/// The default CLUTs of EN 300 743 section 10: 2-bit (4), 4-bit (16) and 8-bit (256) entries.
pub fn default_clut(e2: &mut [u32; 4], e4: &mut [u32; 16], e8: &mut [u32; 256]) {
    *e2 = [0x0000_0000, 0xFFFF_FFFF, 0xFF00_0000, 0xFF80_8080];
    for i in 0..16u32 {
        if i == 0 {
            e4[0] = 0;
            continue;
        }
        let lvl = if i < 8 { 255 } else { 127 };
        let (r, g, b) = (if i & 1 != 0 { lvl } else { 0 }, if i & 2 != 0 { lvl } else { 0 }, if i & 4 != 0 { lvl } else { 0 });
        e4[i as usize] = r | g << 8 | b << 16 | 0xFF00_0000;
        if i == 8 {
            e4[i as usize] = 0xFF00_0000;
        }
    }
    for i in 0..256u32 {
        let b = |m: u32, v: u32| if i & m != 0 { v } else { 0 };
        let e = if i == 0 {
            0
        } else if i < 8 {
            b(1, 255) | b(2, 255) << 8 | b(4, 255) << 16 | 0x4000_0000
        } else {
            let v = i & 0x88;
            if v == 0 {
                (b(1, 85) + b(0x10, 170)) | (b(2, 85) + b(0x20, 170)) << 8 | (b(4, 85) + b(0x40, 170)) << 16 | 0xBF00_0000
            } else if v == 8 {
                (b(1, 85) + b(0x10, 170)) | (b(2, 85) + b(0x20, 170)) << 8 | (b(4, 85) + b(0x40, 170)) << 16 | 0x8000_0000
            } else {
                let add = if v == 0x80 { 127 } else { 0 };
                (b(1, 43) + b(0x10, 85) + add) | (b(2, 43) + b(0x20, 85) + add) << 8 | (b(4, 43) + b(0x40, 85) + add) << 16 | 0xFF00_0000
            }
        };
        e8[i as usize] = e;
    }
}

/// Bit reader over a byte slice; reads past the end give 0 bits but still advance (the pixel loops stop on `eof`).
struct Bits<'a> {
    p: &'a [u8],
    n: usize,
    pos: usize,
}

impl<'a> Bits<'a> {
    fn new(p: &'a [u8]) -> Self {
        Bits { p, n: p.len() * 8, pos: 0 }
    }
    fn eof(&self) -> bool {
        self.pos >= self.n
    }
    fn get(&mut self, k: u32) -> i32 {
        let mut v = 0i32;
        for _ in 0..k {
            v <<= 1;
            if self.pos < self.n {
                v |= ((self.p[self.pos >> 3] >> (7 - (self.pos & 7))) & 1) as i32;
            }
            self.pos += 1;
        }
        v
    }
    fn align(&mut self) {
        self.pos = (self.pos + 7) & !7;
    }
}

/// Where decoded pixels go: a region of `w` x `h` palette indices of `depth` bits.
struct Painter<'a> {
    pix: &'a mut [u8],
    w: i32,
    h: i32,
    depth: i32,
    x0: i32,
    x: i32,
    y: i32,
    double_lines: bool,
    map2to4: [u8; 4],
    map2to8: [u8; 4],
    map4to8: [u8; 16],
}

impl Painter<'_> {
    /// Paint `run` pixels of palette code `code` of a `src_depth` bit string, converting to the region depth.
    fn put(&mut self, code: i32, run: i32, src_depth: i32) {
        if run <= 0 {
            return;
        }
        let mut v = code;
        if src_depth != self.depth {
            v = if src_depth == 2 && self.depth == 4 {
                self.map2to4[(code & 3) as usize] as i32
            } else if src_depth == 2 && self.depth == 8 {
                self.map2to8[(code & 3) as usize] as i32
            } else if src_depth == 4 && self.depth == 8 {
                self.map4to8[(code & 15) as usize] as i32
            } else {
                code & 1i32.wrapping_shl(self.depth as u32).wrapping_sub(1)
            };
        }
        for _ in 0..run {
            let x = self.x;
            self.x = self.x.wrapping_add(1);
            if x < 0 || x >= self.w {
                continue;
            }
            for l in 0..=(self.double_lines as i32) {
                let yy = self.y.wrapping_add(l);
                if yy >= 0 && yy < self.h {
                    let idx = yy as usize * self.w as usize + x as usize;
                    if idx < self.pix.len() {
                        self.pix[idx] = v as u8;
                    }
                }
            }
        }
    }
}

/// Decode one field's pixel-data sub-blocks `f` into the region (`pix`, `reg_w` x `reg_h`, `reg_depth` bits) starting at (`x0`, `y0`), advancing
/// `line_step` rows per coded line; with `double_lines` every line is also written to the one below (a missing bottom field).
#[allow(clippy::too_many_arguments)]
pub fn decode_field(
    f: &[u8],
    x0: i32,
    y0: i32,
    line_step: i32,
    double_lines: bool,
    reg_w: i32,
    reg_h: i32,
    reg_depth: i32,
    pix: &mut [u8],
) {
    let mut p = Painter {
        pix,
        w: reg_w,
        h: reg_h,
        depth: reg_depth,
        x0,
        x: x0,
        y: y0,
        double_lines,
        map2to4: [0, 7, 8, 15],
        map2to8: [0, 0x77, 0x88, 0xFF],
        map4to8: [0; 16],
    };
    for i in 0..16 {
        p.map4to8[i] = (i * 17) as u8;
    }
    let mut i = 0usize;
    while i < f.len() {
        let t = f[i];
        i += 1;
        if t == 0xF0 {
            p.x = p.x0;
            p.y = p.y.wrapping_add(line_step);
            continue;
        }
        if t == 0x20 {
            let mut k = 0;
            while k < 4 && i < f.len() {
                if k == 0 {
                    p.map2to4[0] = f[i] >> 4;
                    p.map2to4[1] = f[i] & 15;
                } else {
                    p.map2to4[2] = f[i] >> 4;
                    p.map2to4[3] = f[i] & 15;
                }
                i += 1;
                k += 2;
            }
            continue;
        }
        if t == 0x21 {
            let mut k = 0;
            while k < 4 && i < f.len() {
                p.map2to8[k] = f[i];
                i += 1;
                k += 1;
            }
            continue;
        }
        if t == 0x22 {
            let mut k = 0;
            while k < 16 && i < f.len() {
                p.map4to8[k] = f[i];
                i += 1;
                k += 1;
            }
            continue;
        }
        if t != 0x10 && t != 0x11 && t != 0x12 {
            break;
        }
        let mut b = Bits::new(&f[i..]);
        if t == 0x10 {
            while !b.eof() {
                let c = b.get(2);
                if c != 0 {
                    p.put(c, 1, 2);
                    continue;
                }
                if b.get(1) != 0 {
                    let run = b.get(3) + 3;
                    let v = b.get(2);
                    p.put(v, run, 2);
                    continue;
                }
                if b.get(1) != 0 {
                    p.put(0, 1, 2);
                    continue;
                }
                let s3 = b.get(2);
                if s3 == 0 {
                    break;
                }
                if s3 == 1 {
                    p.put(0, 2, 2);
                    continue;
                }
                if s3 == 2 {
                    let run = b.get(4) + 12;
                    let v = b.get(2);
                    p.put(v, run, 2);
                    continue;
                }
                let run = b.get(8) + 29;
                let v = b.get(2);
                p.put(v, run, 2);
            }
        } else if t == 0x11 {
            while !b.eof() {
                let c = b.get(4);
                if c != 0 {
                    p.put(c, 1, 4);
                    continue;
                }
                if b.get(1) == 0 {
                    let r = b.get(3);
                    if r == 0 {
                        break;
                    }
                    p.put(0, r + 2, 4);
                    continue;
                }
                if b.get(1) == 0 {
                    let run = b.get(2) + 4;
                    let v = b.get(4);
                    p.put(v, run, 4);
                    continue;
                }
                let s3 = b.get(2);
                if s3 == 0 {
                    p.put(0, 1, 4);
                    continue;
                }
                if s3 == 1 {
                    p.put(0, 2, 4);
                    continue;
                }
                if s3 == 2 {
                    let run = b.get(4) + 9;
                    let v = b.get(4);
                    p.put(v, run, 4);
                    continue;
                }
                let run = b.get(8) + 25;
                let v = b.get(4);
                p.put(v, run, 4);
            }
        } else {
            while !b.eof() {
                let c = b.get(8);
                if c != 0 {
                    p.put(c, 1, 8);
                    continue;
                }
                if b.get(1) == 0 {
                    let r = b.get(7);
                    if r == 0 {
                        break;
                    }
                    p.put(0, r, 8);
                    continue;
                }
                let run = b.get(7);
                let v = b.get(8);
                p.put(v, run, 8);
            }
        }
        b.align();
        i += b.pos / 8;
    }
}

/// Palette indices to RGBA bytes (`r, g, b, a` per pixel) through the CLUT of the region's depth; `out` needs 4 bytes per pixel.
pub fn region_rgba(pix: &[u8], depth: i32, e2: &[u32; 4], e4: &[u32; 16], e8: &[u32; 256], out: &mut [u8]) {
    for (k, &p) in pix.iter().enumerate() {
        if k * 4 + 4 > out.len() {
            break;
        }
        let v = if depth == 2 {
            e2[(p & 3) as usize]
        } else if depth == 4 {
            e4[(p & 15) as usize]
        } else {
            e8[p as usize]
        };
        out[k * 4..k * 4 + 4].copy_from_slice(&[v as u8, (v >> 8) as u8, (v >> 16) as u8, (v >> 24) as u8]);
    }
}

/// Alpha-blend one region (`rw` x `rh` straight-alpha RGBA at (`rx`, `ry`)) into a `dw` x `dh` RGBA picture.
#[allow(clippy::too_many_arguments)]
pub fn blend_region(out: &mut [u8], dw: i32, dh: i32, rx: i32, ry: i32, rw: i32, rh: i32, rgba: &[u8]) {
    for y in 0..rh.max(0) {
        let dy = ry + y;
        if dy < 0 || dy >= dh {
            continue;
        }
        for x in 0..rw.max(0) {
            let dx = rx + x;
            if dx < 0 || dx >= dw {
                continue;
            }
            let si = (y as usize * rw as usize + x as usize) * 4;
            let oi = (dy as usize * dw as usize + dx as usize) * 4;
            if si + 4 > rgba.len() || oi + 4 > out.len() {
                continue;
            }
            let s = [rgba[si], rgba[si + 1], rgba[si + 2], rgba[si + 3]];
            if s[3] == 0 {
                continue;
            }
            let (sa, oa) = (s[3] as i32, out[oi + 3] as i32);
            if sa == 255 || oa == 0 {
                out[oi..oi + 4].copy_from_slice(&s);
                continue;
            }
            let ra = sa + oa * (255 - sa) / 255;
            for c in 0..3 {
                out[oi + c] = ((s[c] as i32 * sa * 255 + out[oi + c] as i32 * oa * (255 - sa)) / (ra * 255)) as u8;
            }
            out[oi + 3] = ra as u8;
        }
    }
}

/// Distance a - b in seconds on the 33-bit PTS circle (26.5 hours), so a clock that wrapped still compares correctly.
pub fn pts_delta(a: f64, b: f64) -> f64 {
    let wrap = 8589934592.0 / 90000.0;
    let mut d = a - b;
    if d > wrap / 2.0 {
        d -= wrap;
    } else if d < -wrap / 2.0 {
        d += wrap;
    }
    d
}
