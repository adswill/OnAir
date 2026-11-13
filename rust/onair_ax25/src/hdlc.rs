// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! HDLC receiver for AX.25 (ITU-T Q.921 framing as used by AX.25 v2.2 section 3.9): finds flags (0x7E), removes stuffed zeros, aborts on seven ones, cuts out
//! the frames and checks the FCS. One bit per call; the state is a plain C struct so the C++ class can own it.
use crate::frame::fcs_ok;

/// Longest frame body kept (bytes before the FCS check); longer ones are dropped.
pub const MAX_FRAME: usize = 400;

/// Receiver state (C layout; the C++ `HdlcRx` holds one and reads the public fields).
#[derive(Clone, Copy)]
#[repr(C)]
pub struct Hdlc {
    /// Previous line level (NRZI).
    pub prev: i32,
    /// Last eight data bits (flag detector), newest in bit 7.
    pub pat: u8,
    /// Byte being assembled.
    pub acc: u8,
    /// Bits in `acc`.
    pub cnt: i32,
    /// Inside a frame.
    pub in_frame: bool,
    /// Waiting for the first flag.
    pub hunting: bool,
    /// A good frame ended: the demodulator should resync its clock.
    pub resync_clock: bool,
    /// Bit times since the last flag, counting down from 16 (48 after a good frame).
    pub flag_recent: i32,
    /// Bytes in `buf`.
    pub len: u32,
    /// Frame bytes (the last frame stays readable until the next bit).
    pub buf: [u8; MAX_FRAME + 2],
}

/// What a bit did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    /// Nothing completed.
    None,
    /// A frame with a good FCS: `buf[..len]` is the frame without the FCS.
    Good(usize),
    /// A frame of plausible length with a bad FCS: `buf[..len]` is every byte.
    Bad(usize),
}

impl Default for Hdlc {
    fn default() -> Self {
        Hdlc {
            prev: 0,
            pat: 0,
            acc: 0,
            cnt: 0,
            in_frame: false,
            hunting: true,
            resync_clock: false,
            flag_recent: 0,
            len: 0,
            buf: [0; MAX_FRAME + 2],
        }
    }
}

impl Hdlc {
    /// Back to the start state.
    pub fn reset(&mut self) {
        *self = Hdlc::default();
    }

    /// Feeds one bit: an NRZI line level (`nrzi`) or a plain HDLC bit.
    pub fn bit(&mut self, level: i32, nrzi: bool) -> Event {
        let mut d = level & 1;
        if nrzi {
            d = i32::from(d == self.prev);
            self.prev = level & 1;
        }
        if self.flag_recent > 0 {
            self.flag_recent -= 1;
        }
        self.pat = (self.pat >> 1) | ((d as u8) << 7);
        if self.pat == 0x7E {
            // a flag: seven of its bits were already stored, so a frame that ends here leaves seven bits in the accumulator
            let mut flag_hold = 16;
            let mut ev = Event::None;
            let n = self.len as usize;
            if !self.hunting && self.in_frame && self.cnt == 7 && (17..=MAX_FRAME).contains(&n) {
                if fcs_ok(&self.buf[..n]) {
                    ev = Event::Good(n - 2);
                    self.resync_clock = true;
                    flag_hold = 48;
                } else {
                    ev = Event::Bad(n);
                }
                self.hunting = true;
                self.in_frame = false;
            } else {
                self.hunting = false;
                self.in_frame = true;
            }
            self.len = 0;
            self.acc = 0;
            self.cnt = 0;
            self.flag_recent = flag_hold;
            return ev;
        }
        if self.pat == 0xFE {
            // seven ones: abort
            self.in_frame = false;
            self.hunting = true;
            self.len = 0;
            self.cnt = 0;
            return Event::None;
        }
        if (self.pat & 0xFC) == 0x7C {
            return Event::None; // a stuffed zero
        }
        if self.hunting || !self.in_frame {
            return Event::None;
        }
        self.acc = (self.acc >> 1) | ((d as u8) << 7);
        self.cnt += 1;
        if self.cnt == 8 {
            if self.len as usize > MAX_FRAME {
                self.in_frame = false;
                self.len = 0;
                self.cnt = 0;
                return Event::None;
            }
            self.buf[self.len as usize] = self.acc;
            self.len += 1;
            self.cnt = 0;
        }
        Event::None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn receives_a_frame() {
        // flag, 18 bytes + FCS, flag; HDLC bits LSB first with stuffing
        let mut body = [0u8; 20];
        for (i, b) in body.iter_mut().enumerate().take(18) {
            *b = (i * 7 + 1) as u8;
        }
        let c = crate::frame::fcs(&body[..18]);
        body[18] = (c & 0xFF) as u8;
        body[19] = (c >> 8) as u8;
        let mut bits = [0u8; 400];
        let mut n = 0;
        let push = |v: u8, bits: &mut [u8; 400], n: &mut usize| {
            bits[*n] = v;
            *n += 1;
        };
        for _ in 0..2 {
            for v in [0u8, 1, 1, 1, 1, 1, 1, 0] {
                push(v, &mut bits, &mut n);
            }
        }
        let mut ones = 0;
        for b in body {
            for k in 0..8 {
                let v = (b >> k) & 1;
                push(v, &mut bits, &mut n);
                if v == 1 {
                    ones += 1;
                    if ones == 5 {
                        push(0, &mut bits, &mut n);
                        ones = 0;
                    }
                } else {
                    ones = 0;
                }
            }
        }
        for v in [0u8, 1, 1, 1, 1, 1, 1, 0] {
            push(v, &mut bits, &mut n);
        }
        let mut h = Hdlc::default();
        let mut got = None;
        for &v in &bits[..n] {
            if let Event::Good(l) = h.bit(i32::from(v), false) {
                got = Some(l);
            }
        }
        assert_eq!(got, Some(18));
        assert_eq!(&h.buf[..18], &body[..18]);
        assert!(h.resync_clock);
    }
}
