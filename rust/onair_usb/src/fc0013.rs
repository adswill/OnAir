//! Fitipower FC0013 tuner driver, a translation of `src/tuner_fc0013.c` of librtlsdr (osmocom).
//!
//! Copyright (C) 2012 Hans-Frieder Vogt <hfvogt@gmx.net>, partially based on driver code from Fitipower, Copyright (C) 2010 Fitipower Integrated
//! Technology Inc; modified for use in librtlsdr, Copyright (C) 2012 Steve Markgraf <steve@steve-m.de>. GPL-2.0-or-later.
//! Register values, the divider table and the integer conversions (`uint8_t` / `uint16_t` truncations) are those of the C. The FC0013 is a
//! zero-IF tuner for the RTL2832U (like the E4000): the demodulator IF stays 0, no `set_if_freq` is needed. As in librtlsdr, the bandwidth
//! is fixed (the 6 MHz setting is always used), `set_bw` does nothing, and unlike the FC0012 the FC0013 needs no GPIO 6 switching.
//! The unused rc_cal helpers of the C are not translated.

use crate::rtl::RtlSdr;
use crate::transfer::UsbTransfer;

pub(crate) const FC0013_I2C_ADDR: u8 = 0xc6;

/// LNA gain table of the C: (gain in tenths of a dB, register value of 0x14 bits 4..0).
const LNA_GAINS: [(i32, u8); 24] = [
    (-99, 0x02),
    (-73, 0x03),
    (-65, 0x05),
    (-63, 0x04),
    (-63, 0x00),
    (-60, 0x07),
    (-58, 0x01),
    (-54, 0x06),
    (58, 0x0f),
    (61, 0x0e),
    (63, 0x0d),
    (65, 0x0c),
    (67, 0x0b),
    (68, 0x0a),
    (70, 0x09),
    (71, 0x08),
    (179, 0x17),
    (181, 0x16),
    (182, 0x15),
    (184, 0x14),
    (186, 0x13),
    (188, 0x12),
    (191, 0x11),
    (197, 0x10),
];

/// VHF tracking filter bits of register 0x1d for `freq` (`fc0013_set_vhf_track`).
pub(crate) fn vhf_track_bits(freq: u32) -> u8 {
    if freq <= 177_500_000 {
        0x1c
    } else if freq <= 184_500_000 {
        0x18
    } else if freq <= 191_500_000 {
        0x14
    } else if freq <= 198_500_000 {
        0x10
    } else if freq <= 205_500_000 {
        0x0c
    } else if freq <= 219_500_000 {
        0x08
    } else if freq < 300_000_000 {
        0x04
    } else {
        0x1c
    }
}

/// Divider selection: (multiplier, reg 5 bits, reg 6 bits) for `freq`.
pub(crate) fn divider(freq: u32) -> (u32, u8, u8) {
    if freq < 37_084_000 {
        (96, 0x82, 0x00)
    } else if freq < 55_625_000 {
        (64, 0x02, 0x02)
    } else if freq < 74_167_000 {
        (48, 0x42, 0x00)
    } else if freq < 111_250_000 {
        (32, 0x82, 0x02)
    } else if freq < 148_334_000 {
        (24, 0x22, 0x00)
    } else if freq < 222_500_000 {
        (16, 0x42, 0x02)
    } else if freq < 296_667_000 {
        (12, 0x12, 0x00)
    } else if freq < 445_000_000 {
        (8, 0x22, 0x02)
    } else if freq < 593_334_000 {
        (6, 0x0a, 0x00)
    } else if freq < 950_000_000 {
        (4, 0x12, 0x02)
    } else {
        (2, 0x0a, 0x02)
    }
}

/// Register value of 0x14 (bits 4..0) for a LNA gain request, the first table entry at or above `gain`, else the last.
pub(crate) fn lna_code(gain: i32) -> u8 {
    for (i, &(g, c)) in LNA_GAINS.iter().enumerate() {
        if g >= gain || i + 1 == LNA_GAINS.len() {
            return c;
        }
    }
    0x10
}

impl<T: UsbTransfer> RtlSdr<T> {
    fn fc0013_writereg(&mut self, reg: u8, val: u8) -> i32 {
        if self.i2c_write(FC0013_I2C_ADDR, &[reg, val]) < 0 {
            -1
        } else {
            0
        }
    }

    fn fc0013_readreg(&mut self, reg: u8, val: &mut u8) -> i32 {
        if self.i2c_write(FC0013_I2C_ADDR, &[reg]) < 0 {
            return -1;
        }
        let mut b = [0u8; 1];
        if self.i2c_read(FC0013_I2C_ADDR, &mut b) < 0 {
            return -1;
        }
        *val = b[0];
        0
    }

    /// Read register `reg`, write back `f(value)`.
    fn fc0013_modify(&mut self, reg: u8, f: impl Fn(u8) -> u8) -> i32 {
        let mut tmp = 0u8;
        let r = self.fc0013_readreg(reg, &mut tmp);
        if r != 0 {
            return r;
        }
        self.fc0013_writereg(reg, f(tmp))
    }

    pub(crate) fn fc0013_init(&mut self) -> i32 {
        let mut reg: [u8; 22] = [
            0x00, // reg. 0x00: dummy
            0x09, 0x16, 0x00, 0x00, 0x17,
            0x02, // reg. 0x06: LPF bandwidth
            0x0a, // reg. 0x07
            0xff, // reg. 0x08: AGC clock divide by 256, AGC gain 1/256, loop bw 1/8
            0x6e, // reg. 0x09: loop through disabled
            0xb8, // reg. 0x0a: LO test buffer disabled
            0x82, // reg. 0x0b
            0xfc, // reg. 0x0c
            0x01, // reg. 0x0d: AGC not forcing, LNA forcing
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x50, // reg. 0x14: DVB-T high gain, UHF
            0x01, // reg. 0x15
        ];
        reg[0x07] |= 0x20;
        reg[0x0c] |= 0x02;
        let mut ret = 0;
        for i in 1..reg.len() {
            ret = self.fc0013_writereg(i as u8, reg[i]);
            if ret < 0 {
                break;
            }
        }
        ret
    }

    pub(crate) fn fc0013_exit(&mut self) -> i32 {
        0
    }

    pub(crate) fn fc0013_set_freq(&mut self, freq: u32) -> i32 {
        // the C calls fc0013_set_params(dev, freq, 6000000)
        let xtal_freq_div_2: u32 = self.get_xtal_freq().1 / 2;
        let mut ret;
        ret = self.fc0013_modify(0x1d, |t| (t & 0xe3) | vhf_track_bits(freq));
        if ret != 0 {
            return ret;
        }
        if freq < 300_000_000 {
            // VHF filter on, UHF and GPS off
            ret = self.fc0013_modify(0x07, |t| t | 0x10);
            if ret != 0 {
                return ret;
            }
            ret = self.fc0013_modify(0x14, |t| t & 0x1f);
        } else {
            // VHF filter off, UHF on, GPS off
            ret = self.fc0013_modify(0x07, |t| t & 0xef);
            if ret != 0 {
                return ret;
            }
            ret = self.fc0013_modify(0x14, |t| (t & 0x1f) | 0x40);
        }
        if ret != 0 {
            return ret;
        }
        let (multi, r5, r6) = divider(freq);
        let mut reg = [0u8; 7];
        reg[5] = r5;
        reg[6] = r6;
        let f_vco: u64 = (freq.wrapping_mul(multi)) as u64;
        let mut vco_select = false;
        if f_vco >= 3_060_000_000 {
            reg[6] |= 0x08;
            vco_select = true;
        }
        let x2 = xtal_freq_div_2 as u64;
        if x2 < 1000 {
            return -1;
        }
        // from the divided value (XDIV) determine the FA and FP values
        let mut xdiv: u16 = (f_vco / x2) as u16;
        if (f_vco - (xdiv as u64) * x2) >= x2 / 2 {
            xdiv = xdiv.wrapping_add(1);
        }
        let mut pm: u8 = (xdiv / 8) as u8;
        let mut am: u8 = (xdiv as u32 - 8 * pm as u32) as u8;
        if am < 2 {
            am = am.wrapping_add(8);
            pm = pm.wrapping_sub(1);
        }
        if pm > 31 {
            reg[1] = am.wrapping_add(8u8.wrapping_mul(pm - 31));
            reg[2] = 31;
        } else {
            reg[1] = am;
            reg[2] = pm;
        }
        if reg[1] > 15 || reg[2] < 0x0b {
            return -1; // no valid PLL combination for this frequency
        }
        // fix clock out
        reg[6] |= 0x20;
        // the fractional part of the delta sigma PLL
        let mut xin: u16 = ((f_vco - (f_vco / x2) * x2) / 1000) as u16;
        xin = (((xin as u32) << 15) / (x2 / 1000) as u32) as u16;
        if xin >= 16384 {
            xin = xin.wrapping_add(32768);
        }
        reg[3] = (xin >> 8) as u8;
        reg[4] = (xin & 0xff) as u8;
        reg[6] &= 0x3f; // bits 6 and 7 describe the bandwidth
        reg[6] |= 0x80; // 6 MHz, the only bandwidth the driver asks for
        // modified for the Realtek demodulator
        reg[5] |= 0x07;
        for i in 1..=6usize {
            ret = self.fc0013_writereg(i as u8, reg[i]);
            if ret != 0 {
                return ret;
            }
        }
        ret = if multi == 64 { self.fc0013_modify(0x11, |t| t | 0x04) } else { self.fc0013_modify(0x11, |t| t & 0xfb) };
        if ret != 0 {
            return ret;
        }
        // VCO calibration
        ret = self.fc0013_writereg(0x0e, 0x80);
        if ret == 0 {
            ret = self.fc0013_writereg(0x0e, 0x00);
        }
        // VCO re-calibration if needed
        if ret == 0 {
            ret = self.fc0013_writereg(0x0e, 0x00);
        }
        let mut tmp = 0u8;
        if ret == 0 {
            ret = self.fc0013_readreg(0x0e, &mut tmp);
        }
        if ret != 0 {
            return ret;
        }
        // VCO selection
        tmp &= 0x3f;
        if (vco_select && tmp > 0x3c) || (!vco_select && tmp < 0x02) {
            if vco_select {
                reg[6] &= !0x08;
            } else {
                reg[6] |= 0x08;
            }
            ret = self.fc0013_writereg(0x06, reg[6]);
            if ret == 0 {
                ret = self.fc0013_writereg(0x0e, 0x80);
            }
            if ret == 0 {
                ret = self.fc0013_writereg(0x0e, 0x00);
            }
        }
        ret
    }

    pub(crate) fn fc0013_set_bw(&mut self, _bw: i32) -> i32 {
        0
    }

    pub(crate) fn fc0013_set_gain(&mut self, gain: i32) -> i32 {
        let mut tmp = 0u8;
        let mut ret = self.fc0013_readreg(0x14, &mut tmp);
        tmp &= 0xe0;
        tmp |= lna_code(gain);
        ret |= self.fc0013_writereg(0x14, tmp);
        ret
    }

    pub(crate) fn fc0013_set_gain_mode(&mut self, manual: i32) -> i32 {
        let mut tmp = 0u8;
        let mut ret = self.fc0013_readreg(0x0d, &mut tmp);
        if manual != 0 {
            tmp |= 1 << 3;
        } else {
            tmp &= !(1 << 3);
        }
        ret |= self.fc0013_writereg(0x0d, tmp);
        // a fixed IF gain for now
        ret |= self.fc0013_writereg(0x13, 0x0a);
        ret
    }
}
