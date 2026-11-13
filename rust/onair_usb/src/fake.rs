// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A fake RTL2832U dongle for tests and fuzzing: models the register file, the I2C repeater, an R820T / R828D / E4000 (or no) tuner and the EEPROM,
//! without hardware. Chips are modelled just far enough for the driver: the R82xx status bytes (chip id, PLL lock, VCO fine tune, filter calibration
//! code, bit reversed like the real chip), the E4000 chip id and PLL lock bit, plain register files for everything else. A seeded corruption
//! switch makes it answer wrongly (errors, short transfers, flipped bytes) for robustness tests.

use crate::transfer::{UsbString, UsbTransfer};

/// Which tuner the fake dongle carries.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FakeChip {
    /// R820T / R820T2 at I2C address 0x34
    R820T,
    /// R828D at 0x74
    R828D,
    /// E4000 at 0xc8
    E4000,
    /// FC0013 at 0xc6 (recognised, not driven)
    Fc0013,
    /// No tuner answers
    None,
}

const N_CHIPS: usize = 6;

fn slot(addr: u8) -> Option<usize> {
    match addr {
        0x34 => Some(0),
        0x74 => Some(1),
        0xc8 => Some(2),
        0xc6 => Some(3),
        0xac => Some(4),
        0xa0 => Some(5),
        _ => None,
    }
}

fn bitrev(b: u8) -> u8 {
    b.reverse_bits()
}

/// The fake dongle.
pub struct FakeRtl {
    usb_regs: [u8; 0x400],
    sys_regs: [u8; 0x40],
    demod: [[u8; 256]; 32],
    regs: [[u8; 256]; N_CHIPS],
    ptr: [u8; N_CHIPS],
    present: [bool; N_CHIPS],
    eeprom: [u8; 256],
    /// R82xx status byte 4 before bit reversal: bits 5..4 VCO fine tune, bits 3..0 filter calibration code.
    pub r82_vco_cal: u8,
    /// PLL locks (R82xx and E4000).
    pub lock: bool,
    /// Manufacturer string.
    pub vendor: &'static str,
    /// Product string.
    pub product: &'static str,
    /// Number of transfers served so far.
    pub transfers: u32,
    /// When non-zero, every n-th transfer is answered wrongly.
    pub corrupt_every: u32,
    seed: u32,
}

impl FakeRtl {
    /// A dongle with `chip`, PLL locking, generic strings and an EEPROM that does not force the bias-T.
    pub fn new(chip: FakeChip) -> Self {
        let mut f = FakeRtl {
            usb_regs: [0; 0x400],
            sys_regs: [0; 0x40],
            demod: [[0; 256]; 32],
            regs: [[0; 256]; N_CHIPS],
            ptr: [0; N_CHIPS],
            present: [false; N_CHIPS],
            eeprom: [0; 256],
            r82_vco_cal: 0x22,
            lock: true,
            vendor: "Realtek",
            product: "RTL2838UHIDIR",
            transfers: 0,
            corrupt_every: 0,
            seed: 0x1234_5678,
        };
        for (i, e) in f.eeprom.iter_mut().enumerate() {
            *e = (i as u8).wrapping_mul(5).wrapping_add(1);
        }
        f.eeprom[7] = 0x02;
        f.present[5] = true;
        match chip {
            FakeChip::R820T => f.present[0] = true,
            FakeChip::R828D => f.present[1] = true,
            FakeChip::E4000 => {
                f.present[2] = true;
                f.regs[2][2] = 0x40;
            }
            FakeChip::Fc0013 => {
                f.present[3] = true;
                f.regs[3][0] = 0xa3;
            }
            FakeChip::None => {}
        }
        f
    }

    /// Sets the seed of the corruption generator.
    pub fn seed(&mut self, seed: u32) {
        self.seed = seed | 1;
    }

    /// Makes the EEPROM force the bias-T on (IR endpoint bit cleared).
    pub fn eeprom_forces_bias_tee(&mut self) {
        self.eeprom[7] = 0x00;
    }

    /// The SYS block register at `addr` (0x3000 ..), e.g. GPO 0x3001.
    pub fn sys_reg(&self, addr: u16) -> u8 {
        self.sys_regs[(addr.wrapping_sub(0x3000) & 0x3f) as usize]
    }

    /// A demodulator register.
    pub fn demod_reg(&self, page: u8, reg: u8) -> u8 {
        self.demod[(page & 31) as usize][reg as usize]
    }

    /// A register of the tuner at I2C address `addr`, as written by the driver.
    pub fn chip_reg(&self, addr: u8, reg: u8) -> u8 {
        slot(addr).map(|s| self.regs[s][reg as usize]).unwrap_or(0)
    }

    fn rnd(&mut self) -> u32 {
        let mut x = self.seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.seed = x;
        x
    }

    /// Decides whether this transfer is answered wrongly; 0 = fine, otherwise a random word describing how.
    fn corrupt(&mut self) -> u32 {
        self.transfers = self.transfers.wrapping_add(1);
        if self.corrupt_every != 0 && self.transfers % self.corrupt_every == 0 {
            self.rnd() | 0x100
        } else {
            0
        }
    }

    fn i2c_in(&mut self, addr: u8, data: &mut [u8]) -> i32 {
        let s = match slot(addr) {
            Some(s) if self.present[s] => s,
            _ => return -9,
        };
        for d in data.iter_mut() {
            let p = self.ptr[s];
            *d = match s {
                5 => self.eeprom[p as usize],
                0 | 1 => match p {
                    0 => 0x69,
                    1 => 0,
                    2 => bitrev(if self.lock { 0x55 } else { 0x15 }),
                    3 => bitrev(0x5a),
                    4 => bitrev(self.r82_vco_cal),
                    _ => bitrev(self.regs[s][p as usize]),
                },
                2 if p == 0x07 => (self.regs[s][p as usize] & !1) | u8::from(self.lock),
                _ => self.regs[s][p as usize],
            };
            self.ptr[s] = p.wrapping_add(1);
        }
        data.len() as i32
    }

    fn i2c_out(&mut self, addr: u8, data: &[u8]) -> i32 {
        let s = match slot(addr) {
            Some(s) if self.present[s] => s,
            _ => return -9,
        };
        if let Some(&p) = data.first() {
            self.ptr[s] = p;
            if s == 5 {
                if data.len() == 2 {
                    self.eeprom[p as usize] = data[1];
                }
            } else {
                for (i, &v) in data.iter().enumerate().skip(1) {
                    self.regs[s][p.wrapping_add(i as u8 - 1) as usize] = v;
                }
            }
        }
        data.len() as i32
    }
}

impl UsbTransfer for FakeRtl {
    fn control_in(&mut self, _rt: u8, _req: u8, value: u16, index: u16, buf: &mut [u8], _timeout_ms: u32) -> i32 {
        let bad = self.corrupt();
        let mut ret = buf.len() as i32;
        if index < 0x100 {
            for (i, b) in buf.iter_mut().enumerate() {
                *b = self.demod[(index & 31) as usize][((value >> 8) as u8 as usize + i) & 0xff];
            }
        } else if index >> 8 == 6 {
            ret = self.i2c_in(value as u8, buf);
        } else {
            for (i, b) in buf.iter_mut().enumerate() {
                let a = value.wrapping_add(i as u16);
                *b = if index >> 8 == 2 {
                    self.sys_regs[(a.wrapping_sub(0x3000) & 0x3f) as usize]
                } else {
                    self.usb_regs[(a.wrapping_sub(0x2000) & 0x3ff) as usize]
                };
            }
        }
        if bad != 0 {
            match bad & 3 {
                0 => return -1,
                1 => return ret.saturating_sub(1).max(0),
                2 => {
                    for b in buf.iter_mut() {
                        *b ^= (bad >> 8) as u8;
                    }
                }
                _ => return -7,
            }
        }
        ret
    }

    fn control_out(&mut self, _rt: u8, _req: u8, value: u16, index: u16, data: &[u8], _timeout_ms: u32) -> i32 {
        let bad = self.corrupt();
        if bad != 0 && bad & 1 == 0 {
            return -1;
        }
        let mut ret = data.len() as i32;
        if index < 0x100 {
            for (i, &d) in data.iter().enumerate() {
                self.demod[(index & 31) as usize][((value >> 8) as u8 as usize + i) & 0xff] = d;
            }
        } else if index >> 8 == 6 {
            ret = self.i2c_out(value as u8, data);
        } else {
            for (i, &d) in data.iter().enumerate() {
                let a = value.wrapping_add(i as u16);
                if index >> 8 == 2 {
                    self.sys_regs[(a.wrapping_sub(0x3000) & 0x3f) as usize] = d;
                } else {
                    self.usb_regs[(a.wrapping_sub(0x2000) & 0x3ff) as usize] = d;
                }
            }
        }
        if bad != 0 {
            ret = ret.saturating_sub(1);
        }
        ret
    }

    fn bulk_read(&mut self, _endpoint: u8, buf: &mut [u8], _timeout_ms: u32) -> i32 {
        for (i, b) in buf.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(7).wrapping_add(3);
        }
        if self.corrupt() != 0 {
            return -7;
        }
        buf.len() as i32
    }

    fn string_descriptor(&mut self, which: UsbString, out: &mut [u8]) -> i32 {
        let s = match which {
            UsbString::Manufacturer => self.vendor,
            UsbString::Product => self.product,
            UsbString::Serial => "00000001",
        };
        let n = s.len().min(out.len());
        out[..n].copy_from_slice(&s.as_bytes()[..n]);
        n as i32
    }
}
