//! RTL2832U register protocol and the librtlsdr-compatible device API, as pure logic over [`UsbTransfer`].
//!
//! This is a translation of `src/librtlsdr.c` of the RTL-SDR Blog fork of librtlsdr 1.3.6 (derived from osmocom rtl-sdr; GPL-2.0-or-later,
//! Copyright (C) 2012-2014 Steve Markgraf, Dimitri Stolnikov and others, see README.md). Control flow, register sequences and integer conversions follow
//! the C on purpose so the transfers are identical (the differential tests compare them byte for byte); the functions keep the C return convention
//! (0 / byte count on success, negative on failure) and the C names without the `rtlsdr_` prefix.
//!
//! Deliberate differences from the C: no `fprintf` diagnostics; scratch buffers are zero filled (the C leaves them uninitialised); an unreadable EEPROM
//! leaves the "force bias-T" hack off (the C would read stack garbage); USB device enumeration, open, claim and the async transfer machinery belong to the
//! platform layer (they are libusb calls, not protocol), so [`RtlSdr::open`] starts at the dummy register write that follows `libusb_claim_interface`.

use crate::e4k::E4k;
use crate::r82xx::R82;
use crate::transfer::{UsbString, UsbTransfer, BULK_ENDPOINT, BULK_TIMEOUT_MS, CTRL_TIMEOUT_MS, REQ_VENDOR_IN, REQ_VENDOR_OUT};

/// `-EINVAL` as librtlsdr returns it.
pub const EINVAL: i32 = 22;

/// Tuner chips (`enum rtlsdr_tuner`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tuner {
    /// No supported tuner found: the dongle runs in direct sampling mode.
    Unknown = 0,
    /// Elonics E4000
    E4000 = 1,
    /// Fitipower FC0012 (detected only)
    Fc0012 = 2,
    /// Fitipower FC0013
    Fc0013 = 3,
    /// FCI FC2580 (detected only)
    Fc2580 = 4,
    /// Rafael Micro R820T / R820T2
    R820T = 5,
    /// Rafael Micro R828D
    R828D = 6,
}

impl Tuner {
    /// The value of `enum rtlsdr_tuner` (what `rtlsdr_get_tuner_type` returns).
    pub fn code(self) -> i32 {
        self as i32
    }
}

/// Register blocks of the RTL2832U (`enum blocks`).
const USBB: u8 = 1;
const SYSB: u8 = 2;
const IICB: u8 = 6;

const USB_SYSCTL: u16 = 0x2000;
const USB_EPA_CTL: u16 = 0x2148;
const USB_EPA_MAXPKT: u16 = 0x2158;
const DEMOD_CTL: u16 = 0x3000;
const GPO: u16 = 0x3001;
const GPOE: u16 = 0x3003;
const GPD: u16 = 0x3004;
const DEMOD_CTL_1: u16 = 0x300b;

const EEPROM_ADDR: u8 = 0xa0;
/// Size of the dongle's EEPROM.
pub const EEPROM_SIZE: usize = 256;

/// Crystal frequency of the RTL2832U.
pub const DEF_RTL_XTAL_FREQ: u32 = 28_800_000;
const MIN_RTL_XTAL_FREQ: u32 = DEF_RTL_XTAL_FREQ - 1000;
const MAX_RTL_XTAL_FREQ: u32 = DEF_RTL_XTAL_FREQ + 1000;
/// Crystal of a plain R828D dongle (the RTL-SDR Blog V4 keeps 28.8 MHz).
pub const R828D_XTAL_FREQ: u32 = 16_000_000;
/// IF of the R82xx tuners.
pub const R82XX_IF_FREQ: u32 = 3_570_000;

const E4K_I2C_ADDR: u8 = 0xc8;
const E4K_CHECK_ADDR: u8 = 0x02;
const E4K_CHECK_VAL: u8 = 0x40;
const FC0013_I2C_ADDR: u8 = 0xc6;
const FC0013_CHECK_ADDR: u8 = 0x00;
const FC0013_CHECK_VAL: u8 = 0xa3;
const FC2580_I2C_ADDR: u8 = 0xac;
const FC2580_CHECK_ADDR: u8 = 0x01;
const FC2580_CHECK_VAL: u8 = 0x56;
const FC0012_I2C_ADDR: u8 = 0xc6;
const FC0012_CHECK_ADDR: u8 = 0x00;
const FC0012_CHECK_VAL: u8 = 0xa1;
/// I2C address of the R820T / R820T2.
pub const R820T_I2C_ADDR: u8 = 0x34;
/// I2C address of the R828D.
pub const R828D_I2C_ADDR: u8 = 0x74;
const R82XX_CHECK_ADDR: u8 = 0x00;
const R82XX_CHECK_VAL: u8 = 0x69;

/// Default FIR of the Windows driver (8 bit signed, then 12 bit signed coefficients).
const FIR_DEFAULT: [i32; 16] = [-54, -36, -41, -40, -32, -14, 14, 53, 101, 156, 215, 273, 327, 372, 404, 421];

const TWO_POW_22: f64 = 4_194_304.0;
const TWO_POW_24: f64 = 16_777_216.0;

/// An opened RTL-SDR (the Rust twin of `rtlsdr_dev_t`).
pub struct RtlSdr<T: UsbTransfer> {
    pub(crate) usb: T,
    pub(crate) rate: u32,
    pub(crate) rtl_xtal: u32,
    fir: [i32; 16],
    pub(crate) direct_sampling: i32,
    pub(crate) tuner_type: Tuner,
    pub(crate) tun_xtal: u32,
    pub(crate) freq: u32,
    pub(crate) bw: u32,
    pub(crate) offs_freq: u32,
    pub(crate) corr: i32,
    pub(crate) gain: i32,
    pub(crate) e4k: E4k,
    pub(crate) r82: R82,
    dev_lost: bool,
    manufact: [u8; 256],
    product: [u8; 256],
    force_bt: bool,
    direct_sampling_mode: i32,
}

/// Bytes of a NUL terminated buffer.
fn c_str(b: &[u8]) -> &[u8] {
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    &b[..n]
}

impl<T: UsbTransfer> RtlSdr<T> {
    // ---- register access -------------------------------------------------------------------------------------------------------------------------

    /// Reads `buf.len()` bytes of `block` at `addr` (`rtlsdr_read_array`).
    pub(crate) fn read_array(&mut self, block: u8, addr: u16, buf: &mut [u8]) -> i32 {
        let index = (block as u16) << 8;
        self.usb.control_in(REQ_VENDOR_IN, 0, addr, index, buf, CTRL_TIMEOUT_MS)
    }

    /// Writes `buf` to `block` at `addr` (`rtlsdr_write_array`).
    pub(crate) fn write_array(&mut self, block: u8, addr: u16, buf: &[u8]) -> i32 {
        let index = ((block as u16) << 8) | 0x10;
        self.usb.control_out(REQ_VENDOR_OUT, 0, addr, index, buf, CTRL_TIMEOUT_MS)
    }

    /// I2C read of one tuner register: a write of the register number, then a one byte read (`rtlsdr_i2c_read_reg`). 0 when the read fails.
    fn i2c_read_reg(&mut self, i2c_addr: u8, reg: u8) -> u8 {
        let mut data = [0u8; 1];
        self.write_array(IICB, i2c_addr as u16, &[reg]);
        self.read_array(IICB, i2c_addr as u16, &mut data);
        data[0]
    }

    /// I2C write of raw bytes (`rtlsdr_i2c_write_fn`), used by the tuner drivers.
    pub(crate) fn i2c_write(&mut self, i2c_addr: u8, buf: &[u8]) -> i32 {
        self.write_array(IICB, i2c_addr as u16, buf)
    }

    /// I2C read of raw bytes (`rtlsdr_i2c_read_fn`).
    pub(crate) fn i2c_read(&mut self, i2c_addr: u8, buf: &mut [u8]) -> i32 {
        self.read_array(IICB, i2c_addr as u16, buf)
    }

    /// Reads a 1 or 2 byte register of the USB / SYS blocks (`rtlsdr_read_reg`).
    fn read_reg(&mut self, block: u8, addr: u16, len: u8) -> u16 {
        let mut data = [0u8; 2];
        let n = (len as usize).min(2);
        let index = (block as u16) << 8;
        self.usb.control_in(REQ_VENDOR_IN, 0, addr, index, &mut data[..n], CTRL_TIMEOUT_MS);
        ((data[1] as u16) << 8) | data[0] as u16
    }

    /// Writes a 1 or 2 byte register (`rtlsdr_write_reg`); the two byte form is big endian.
    fn write_reg(&mut self, block: u8, addr: u16, val: u16, len: u8) -> i32 {
        let index = ((block as u16) << 8) | 0x10;
        let data = [if len == 1 { (val & 0xff) as u8 } else { (val >> 8) as u8 }, (val & 0xff) as u8];
        let n = (len as usize).min(2);
        self.usb.control_out(REQ_VENDOR_OUT, 0, addr, index, &data[..n], CTRL_TIMEOUT_MS)
    }

    /// Reads a demodulator register (`rtlsdr_demod_read_reg`).
    fn demod_read_reg(&mut self, page: u8, addr: u16, len: u8) -> u16 {
        let mut data = [0u8; 2];
        let n = (len as usize).min(2);
        let a = (addr << 8) | 0x20;
        self.usb.control_in(REQ_VENDOR_IN, 0, a, page as u16, &mut data[..n], CTRL_TIMEOUT_MS);
        ((data[1] as u16) << 8) | data[0] as u16
    }

    /// Writes a demodulator register, then does the dummy read the chip needs to latch it (`rtlsdr_demod_write_reg`).
    fn demod_write_reg(&mut self, page: u8, addr: u16, val: u16, len: u8) -> i32 {
        let index = 0x10 | page as u16;
        let a = (addr << 8) | 0x20;
        let data = [if len == 1 { (val & 0xff) as u8 } else { (val >> 8) as u8 }, (val & 0xff) as u8];
        let n = (len as usize).min(2);
        let r = self.usb.control_out(REQ_VENDOR_OUT, 0, a, index, &data[..n], CTRL_TIMEOUT_MS);
        self.demod_read_reg(0x0a, 0x01, 1);
        if r == len as i32 {
            0
        } else {
            -1
        }
    }

    /// Sets or clears one GPIO output bit (`rtlsdr_set_gpio_bit`).
    pub(crate) fn set_gpio_bit(&mut self, gpio: u8, val: bool) {
        let g = 1u8.wrapping_shl(gpio as u32) as u16;
        let mut r = self.read_reg(SYSB, GPO, 1);
        r = if val { r | g } else { r & !g };
        self.write_reg(SYSB, GPO, r, 1);
    }

    /// Makes a GPIO an output (`rtlsdr_set_gpio_output`).
    pub(crate) fn set_gpio_output(&mut self, gpio: u8) {
        let g = 1u8.wrapping_shl(gpio as u32) as u16;
        let r = self.read_reg(SYSB, GPD, 1);
        self.write_reg(SYSB, GPD, r & !g, 1);
        let r = self.read_reg(SYSB, GPOE, 1);
        self.write_reg(SYSB, GPOE, r | g, 1);
    }

    /// Opens or closes the I2C repeater to the tuner (`rtlsdr_set_i2c_repeater`).
    pub(crate) fn set_i2c_repeater(&mut self, on: bool) {
        self.demod_write_reg(1, 0x01, if on { 0x18 } else { 0x10 }, 1);
    }

    /// Programs the 16 FIR coefficients (`rtlsdr_set_fir`); -1 when one does not fit.
    fn set_fir(&mut self) -> i32 {
        let mut fir = [0u8; 20];
        for (f, &val) in fir.iter_mut().zip(self.fir.iter()).take(8) {
            if !(-128..=127).contains(&val) {
                return -1;
            }
            *f = val as u8;
        }
        let mut i = 0;
        while i < 8 {
            let val0 = self.fir[8 + i];
            let val1 = self.fir[8 + i + 1];
            if !(-2048..=2047).contains(&val0) || !(-2048..=2047).contains(&val1) {
                return -1;
            }
            let o = 8 + i * 3 / 2;
            fir[o] = (val0 >> 4) as u8;
            fir[o + 1] = ((val0 << 4) | ((val1 >> 8) & 0x0f)) as u8;
            fir[o + 2] = val1 as u8;
            i += 2;
        }
        for (i, &b) in fir.iter().enumerate() {
            if self.demod_write_reg(1, 0x1c + i as u16, b as u16, 1) != 0 {
                return -1;
            }
        }
        0
    }

    /// Power-on and default demodulator setup (`rtlsdr_init_baseband`).
    fn init_baseband(&mut self) {
        // initialize USB
        self.write_reg(USBB, USB_SYSCTL, 0x09, 1);
        self.write_reg(USBB, USB_EPA_MAXPKT, 0x0002, 2);
        self.write_reg(USBB, USB_EPA_CTL, 0x1002, 2);
        // poweron demod
        self.write_reg(SYSB, DEMOD_CTL_1, 0x22, 1);
        self.write_reg(SYSB, DEMOD_CTL, 0xe8, 1);
        // reset demod (bit 3, soft_rst)
        self.demod_write_reg(1, 0x01, 0x14, 1);
        self.demod_write_reg(1, 0x01, 0x10, 1);
        // disable spectrum inversion and adjacent channel rejection
        self.demod_write_reg(1, 0x15, 0x00, 1);
        self.demod_write_reg(1, 0x16, 0x0000, 2);
        // clear both DDC shift and IF frequency registers
        for i in 0..6 {
            self.demod_write_reg(1, 0x16 + i, 0x00, 1);
        }
        self.set_fir();
        // enable SDR mode, disable DAGC (bit 5)
        self.demod_write_reg(0, 0x19, 0x05, 1);
        // init FSM state-holding register
        self.demod_write_reg(1, 0x93, 0xf0, 1);
        self.demod_write_reg(1, 0x94, 0x0f, 1);
        // disable AGC (en_dagc, bit 0)
        self.demod_write_reg(1, 0x11, 0x00, 1);
        // disable RF and IF AGC loop
        self.demod_write_reg(1, 0x04, 0x00, 1);
        // disable PID filter
        self.demod_write_reg(0, 0x61, 0x60, 1);
        // opt_adc_iq = 0, default ADC_I/ADC_Q datapath
        self.demod_write_reg(0, 0x06, 0x80, 1);
        // Zero-IF mode, DC cancellation, IQ estimation / compensation
        self.demod_write_reg(1, 0xb1, 0x1b, 1);
        // disable 4.096 MHz clock output on pin TP_CK0
        self.demod_write_reg(0, 0x0d, 0x83, 1);
    }

    /// Powers the tuner and the demodulator down (`rtlsdr_deinit_baseband`).
    fn deinit_baseband(&mut self) -> i32 {
        let mut r = 0;
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            r = self.t_exit();
            self.set_i2c_repeater(false);
        }
        // poweroff demodulator and ADCs
        self.write_reg(SYSB, DEMOD_CTL, 0x20, 1);
        r
    }

    /// Programs the IF frequency of the demodulator's DDC (`rtlsdr_set_if_freq`).
    pub(crate) fn set_if_freq(&mut self, freq: u32) -> i32 {
        let (rtl_xtal, _) = self.get_xtal_freq();
        let if_freq = (-((freq as f64 * TWO_POW_22) / rtl_xtal as f64)) as i32;
        let mut r = self.demod_write_reg(1, 0x19, ((if_freq >> 16) & 0x3f) as u16, 1);
        r |= self.demod_write_reg(1, 0x1a, ((if_freq >> 8) & 0xff) as u16, 1);
        r |= self.demod_write_reg(1, 0x1b, (if_freq & 0xff) as u16, 1);
        r
    }

    /// Sample clock correction in ppm (`rtlsdr_set_sample_freq_correction`).
    fn set_sample_freq_correction(&mut self, ppm: i32) -> i32 {
        let offs = ((-(ppm as f64) * TWO_POW_24 / 1_000_000.0) as i32) as i16;
        let mut r = self.demod_write_reg(1, 0x3f, (offs & 0xff) as u16, 1);
        r |= self.demod_write_reg(1, 0x3e, ((offs >> 8) & 0x3f) as u16, 1);
        r
    }

    /// Sets the crystal frequencies, 0 keeping the default (`rtlsdr_set_xtal_freq`).
    pub fn set_xtal_freq(&mut self, rtl_freq: u32, tuner_freq: u32) -> i32 {
        let mut r = 0;
        if rtl_freq > 0 && !(MIN_RTL_XTAL_FREQ..=MAX_RTL_XTAL_FREQ).contains(&rtl_freq) {
            return -2;
        }
        if rtl_freq > 0 && self.rtl_xtal != rtl_freq {
            self.rtl_xtal = rtl_freq;
            // update xtal-dependent settings
            if self.rate != 0 {
                r = self.set_sample_rate(self.rate);
            }
        }
        if self.tun_xtal != tuner_freq {
            self.tun_xtal = if tuner_freq == 0 { self.rtl_xtal } else { tuner_freq };
            // read corrected clock value into e4k and r82xx structure
            let (_, t) = self.get_xtal_freq();
            self.e4k.vco.fosc = t;
            self.r82.xtal = t;
            // update xtal-dependent settings
            if self.freq != 0 {
                r = self.set_center_freq(self.freq);
            }
        }
        r
    }

    /// The crystal frequencies with the ppm correction applied: (RTL2832U, tuner) (`rtlsdr_get_xtal_freq`).
    pub fn get_xtal_freq(&self) -> (u32, u32) {
        let k = 1.0 + (self.corr as f64) / 1e6;
        ((self.rtl_xtal as f64 * k) as u32, (self.tun_xtal as f64 * k) as u32)
    }

    /// Reads the manufacturer / product / serial strings into the buffers (zero filled, ASCII, at most 255 bytes) (`rtlsdr_get_usb_strings`).
    pub fn get_usb_strings(&mut self, manufact: Option<&mut [u8]>, product: Option<&mut [u8]>, serial: Option<&mut [u8]>) -> i32 {
        for (which, buf) in [(UsbString::Manufacturer, manufact), (UsbString::Product, product), (UsbString::Serial, serial)] {
            if let Some(b) = buf {
                b.fill(0);
                let n = b.len().saturating_sub(1).min(255);
                self.usb.string_descriptor(which, &mut b[..n]);
            }
        }
        0
    }

    /// Writes `data` into the EEPROM at `offset` (`rtlsdr_write_eeprom`).
    pub fn write_eeprom(&mut self, data: &[u8], offset: u8) -> i32 {
        if data.len() + offset as usize > 256 {
            return -2;
        }
        for (i, &d) in data.iter().enumerate() {
            let mut cmd = [(i as u8).wrapping_add(offset), 0];
            self.write_array(IICB, EEPROM_ADDR as u16, &cmd[..1]);
            let mut b = [0u8; 1];
            self.read_array(IICB, EEPROM_ADDR as u16, &mut b);
            cmd[1] = b[0];
            // only write the byte if it differs
            if cmd[1] == d {
                continue;
            }
            cmd[1] = d;
            let r = self.write_array(IICB, EEPROM_ADDR as u16, &cmd);
            if r != 2 {
                return -3;
            }
            // some EEPROMs need a delay between write operations
            self.usb.delay_ms(5);
        }
        0
    }

    /// Reads `data.len()` EEPROM bytes from `offset` (`rtlsdr_read_eeprom`).
    pub fn read_eeprom(&mut self, data: &mut [u8], offset: u8) -> i32 {
        if data.len() + offset as usize > 256 {
            return -2;
        }
        let r = self.write_array(IICB, EEPROM_ADDR as u16, &[offset]);
        if r < 0 {
            return -3;
        }
        let mut r = r;
        for i in 0..data.len() {
            r = self.read_array(IICB, EEPROM_ADDR as u16, &mut data[i..i + 1]);
            if r < 0 {
                return -3;
            }
        }
        r
    }

    // ---- tuner dispatch (`tuners[]`) --------------------------------------------------------------------------------------------------------------

    /// True when the dongle has a tuner driver (E4000, FC0013, R820T, R828D; the unsupported FC0012 / FC2580 never get this far, see [`RtlSdr::open`]).
    pub(crate) fn t_has_tuner(&self) -> bool {
        matches!(self.tuner_type, Tuner::E4000 | Tuner::Fc0013 | Tuner::R820T | Tuner::R828D)
    }

    fn t_init(&mut self) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_init(),
            Tuner::Fc0013 => self.fc0013_init(),
            _ => self.r820t_init(),
        }
    }

    fn t_exit(&mut self) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_exit(),
            Tuner::Fc0013 => self.fc0013_exit(),
            _ => self.r820t_exit(),
        }
    }

    fn t_set_freq(&mut self, freq: u32) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_set_freq(freq),
            Tuner::Fc0013 => self.fc0013_set_freq(freq),
            _ => self.r820t_set_freq(freq),
        }
    }

    fn t_set_bw(&mut self, bw: i32) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_set_bw(bw),
            Tuner::Fc0013 => self.fc0013_set_bw(bw),
            _ => self.r820t_set_bw(bw),
        }
    }

    fn t_set_gain(&mut self, gain: i32) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_set_gain(gain),
            Tuner::Fc0013 => self.fc0013_set_gain(gain),
            _ => self.r820t_set_gain(gain),
        }
    }

    fn t_set_gain_mode(&mut self, manual: i32) -> i32 {
        match self.tuner_type {
            Tuner::E4000 => self.e4000_set_gain_mode(manual),
            Tuner::Fc0013 => self.fc0013_set_gain_mode(manual),
            _ => self.r820t_set_gain_mode(manual),
        }
    }

    // ---- the device API ----------------------------------------------------------------------------------------------------------------------------

    /// Tunes to `freq` Hz, switching direct sampling by itself for an R820T below 24 MHz (`rtlsdr_set_center_freq`). 0 on success.
    pub fn set_center_freq(&mut self, freq: u32) -> i32 {
        let mut r = -1;
        // the last direct sampling status
        let last_ds = self.direct_sampling;
        // automatic direct sampling only while the mode is 0 (standard)
        if self.direct_sampling_mode == 0 {
            self.direct_sampling = if freq < 24_000_000 && self.tuner_type == Tuner::R820T && !self.check_dongle_model("RTLSDRBlog", "Blog V4L") { 2 } else { 0 };
        }
        if self.direct_sampling != 0 {
            self.set_i2c_repeater(false);
            r = self.set_if_freq(freq);
        } else if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            r = self.t_set_freq(freq.wrapping_sub(self.offs_freq));
        }
        self.freq = if r == 0 { freq } else { 0 };
        // after dev->freq is updated, so the previous frequency is not set back
        if last_ds != self.direct_sampling {
            return self.set_direct_sampling_inner(self.direct_sampling);
        }
        r
    }

    /// The frequency last set, 0 after a failed tune (`rtlsdr_get_center_freq`).
    pub fn get_center_freq(&self) -> u32 {
        self.freq
    }

    /// Frequency correction in ppm; -2 when unchanged (`rtlsdr_set_freq_correction`).
    pub fn set_freq_correction(&mut self, ppm: i32) -> i32 {
        if self.corr == ppm {
            return -2;
        }
        self.corr = ppm;
        let mut r = self.set_sample_freq_correction(ppm);
        // read corrected clock value into e4k and r82xx structure
        let (_, t) = self.get_xtal_freq();
        self.e4k.vco.fosc = t;
        self.r82.xtal = t;
        if self.freq != 0 {
            // retune to apply new correction value
            r |= self.set_center_freq(self.freq);
        }
        r
    }

    /// The ppm correction set (`rtlsdr_get_freq_correction`).
    pub fn get_freq_correction(&self) -> i32 {
        self.corr
    }

    /// The tuner found at open (`rtlsdr_get_tuner_type`).
    pub fn get_tuner_type(&self) -> Tuner {
        self.tuner_type
    }

    /// The discrete gains of the tuner in tenths of a dB; copies them into `gains` when given and returns the count (`rtlsdr_get_tuner_gains`).
    pub fn get_tuner_gains(&self, gains: Option<&mut [i32]>) -> i32 {
        const E4K_GAINS: [i32; 14] = [-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420];
        const FC0012_GAINS: [i32; 5] = [-99, -40, 71, 179, 192];
        const FC0013_GAINS: [i32; 23] =
            [-99, -73, -65, -63, -60, -58, -54, 58, 61, 63, 65, 67, 68, 70, 71, 179, 181, 182, 184, 186, 188, 191, 197];
        const R82XX_GAINS: [i32; 29] = [
            0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445,
            480, 496,
        ];
        const NONE: [i32; 1] = [0];
        let ptr: &[i32] = match self.tuner_type {
            Tuner::E4000 => &E4K_GAINS,
            Tuner::Fc0012 => &FC0012_GAINS,
            Tuner::Fc0013 => &FC0013_GAINS,
            Tuner::R820T | Tuner::R828D => &R82XX_GAINS,
            Tuner::Fc2580 | Tuner::Unknown => &NONE,
        };
        if let Some(g) = gains {
            let n = ptr.len().min(g.len());
            g[..n].copy_from_slice(&ptr[..n]);
        }
        ptr.len() as i32
    }

    /// Tuner IF bandwidth in Hz, 0 = the sample rate (`rtlsdr_set_tuner_bandwidth`).
    pub fn set_tuner_bandwidth(&mut self, bw: u32) -> i32 {
        let mut r = 0;
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            let b = if bw > 0 { bw } else { self.rate };
            r = self.t_set_bw(b as i32);
            self.set_i2c_repeater(false);
            if r != 0 {
                return r;
            }
            self.bw = bw;
        }
        r
    }

    /// Manual tuner gain in tenths of a dB (`rtlsdr_set_tuner_gain`).
    pub fn set_tuner_gain(&mut self, gain: i32) -> i32 {
        let mut r = 0;
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            r = self.t_set_gain(gain);
        }
        self.gain = if r == 0 { gain } else { 0 };
        r
    }

    /// The gain last set (`rtlsdr_get_tuner_gain`).
    pub fn get_tuner_gain(&self) -> i32 {
        self.gain
    }

    /// IF stage gain of the E4000 (`rtlsdr_set_tuner_if_gain`); other tuners have none and return 0.
    pub fn set_tuner_if_gain(&mut self, stage: i32, gain: i32) -> i32 {
        if self.tuner_type == Tuner::E4000 {
            self.set_i2c_repeater(true);
            return self.e4000_set_if_gain(stage, gain);
        }
        0
    }

    /// Tuner gain mode: 0 = automatic, 1 = manual (`rtlsdr_set_tuner_gain_mode`).
    pub fn set_tuner_gain_mode(&mut self, mode: i32) -> i32 {
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            return self.t_set_gain_mode(mode);
        }
        0
    }

    /// Sample rate in Hz (225 001 .. 300 000 and 900 001 .. 3 200 000); -22 for others (`rtlsdr_set_sample_rate`).
    pub fn set_sample_rate(&mut self, samp_rate: u32) -> i32 {
        // check if the rate is supported by the resampler
        if samp_rate <= 225_000 || samp_rate > 3_200_000 || (samp_rate > 300_000 && samp_rate <= 900_000) {
            return -EINVAL;
        }
        let mut rsamp_ratio = ((self.rtl_xtal as f64 * TWO_POW_22) / samp_rate as f64) as u32;
        rsamp_ratio &= 0x0fff_fffc;
        let real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x0800_0000) << 1);
        let real_rate = (self.rtl_xtal as f64 * TWO_POW_22) / real_rsamp_ratio as f64;
        self.rate = real_rate as u32;
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            let b = if self.bw > 0 { self.bw } else { self.rate };
            self.t_set_bw(b as i32);
            self.set_i2c_repeater(false);
        }
        let mut r = self.demod_write_reg(1, 0x9f, (rsamp_ratio >> 16) as u16, 2);
        r |= self.demod_write_reg(1, 0xa1, (rsamp_ratio & 0xffff) as u16, 2);
        r |= self.set_sample_freq_correction(self.corr);
        // reset demod (bit 3, soft_rst)
        r |= self.demod_write_reg(1, 0x01, 0x14, 1);
        r |= self.demod_write_reg(1, 0x01, 0x10, 1);
        // recalculate offset frequency if offset tuning is enabled
        if self.offs_freq != 0 {
            self.set_offset_tuning(true);
        }
        r
    }

    /// The sample rate in effect, after rounding by the resampler (`rtlsdr_get_sample_rate`).
    pub fn get_sample_rate(&self) -> u32 {
        self.rate
    }

    /// Demodulator test mode (`rtlsdr_set_testmode`).
    pub fn set_testmode(&mut self, on: bool) -> i32 {
        self.demod_write_reg(0, 0x19, if on { 0x03 } else { 0x05 }, 1)
    }

    /// The RTL2832U's own AGC (`rtlsdr_set_agc_mode`).
    pub fn set_agc_mode(&mut self, on: bool) -> i32 {
        self.demod_write_reg(0, 0x19, if on { 0x25 } else { 0x05 }, 1)
    }

    /// Direct sampling: 0 = off (automatic switching below 24 MHz on an R820T), 1 = I branch, 2 = Q branch (`rtlsdr_set_direct_sampling`).
    pub fn set_direct_sampling(&mut self, on: i32) -> i32 {
        // remember the mode set by the user
        self.direct_sampling_mode = on;
        self.set_direct_sampling_inner(on)
    }

    /// `_rtlsdr_set_direct_sampling`.
    fn set_direct_sampling_inner(&mut self, on: i32) -> i32 {
        let mut r = 0;
        if on != 0 {
            if self.t_has_tuner() {
                self.set_i2c_repeater(true);
                r = self.t_exit();
                self.set_i2c_repeater(false);
            }
            // disable Zero-IF mode
            r |= self.demod_write_reg(1, 0xb1, 0x1a, 1);
            // disable spectrum inversion
            r |= self.demod_write_reg(1, 0x15, 0x00, 1);
            // only enable In-phase ADC input
            r |= self.demod_write_reg(0, 0x08, 0x4d, 1);
            // swap I and Q ADC, this allows to select between two inputs
            r |= self.demod_write_reg(0, 0x06, if on > 1 { 0x90 } else { 0x80 }, 1);
            self.direct_sampling = on;
        } else {
            if self.t_has_tuner() {
                self.set_i2c_repeater(true);
                r |= self.t_init();
                self.set_i2c_repeater(false);
            }
            if self.tuner_type == Tuner::R820T || self.tuner_type == Tuner::R828D {
                r |= self.set_if_freq(R82XX_IF_FREQ);
                // enable spectrum inversion
                r |= self.demod_write_reg(1, 0x15, 0x01, 1);
            } else {
                r |= self.set_if_freq(0);
                // enable In-phase + Quadrature ADC input
                r |= self.demod_write_reg(0, 0x08, 0xcd, 1);
                // Enable Zero-IF mode
                r |= self.demod_write_reg(1, 0xb1, 0x1b, 1);
            }
            // opt_adc_iq = 0, default ADC_I/ADC_Q datapath
            r |= self.demod_write_reg(0, 0x06, 0x80, 1);
            self.direct_sampling = 0;
        }
        r |= self.set_center_freq(self.freq);
        r
    }

    /// The direct sampling state in effect (`rtlsdr_get_direct_sampling`).
    pub fn get_direct_sampling(&self) -> i32 {
        self.direct_sampling
    }

    /// Offset tuning (E4000 / FC00xx only). On an R820T / R828D the Blog fork turns the bias-T with it and returns -2 (`rtlsdr_set_offset_tuning`).
    pub fn set_offset_tuning(&mut self, on: bool) -> i32 {
        if self.tuner_type == Tuner::R820T || self.tuner_type == Tuner::R828D {
            // RTL-SDR Blog hack: "offset tuning" switches the bias tee on software without bias-T support
            self.set_bias_tee(on);
            return -2;
        }
        if self.direct_sampling != 0 {
            return -3;
        }
        // based on keenerds 1/f noise measurements
        self.offs_freq = if on { (self.rate / 2).wrapping_mul(170) / 100 } else { 0 };
        let mut r = self.set_if_freq(self.offs_freq);
        if self.t_has_tuner() {
            self.set_i2c_repeater(true);
            let bw = if on {
                self.offs_freq.wrapping_mul(2) as i32
            } else if self.bw > 0 {
                self.bw as i32
            } else {
                self.rate as i32
            };
            self.t_set_bw(bw);
            self.set_i2c_repeater(false);
        }
        if self.freq > self.offs_freq {
            r |= self.set_center_freq(self.freq);
        }
        r
    }

    /// 1 when offset tuning is on (`rtlsdr_get_offset_tuning`).
    pub fn get_offset_tuning(&self) -> i32 {
        i32::from(self.offs_freq != 0)
    }

    /// True when the USB strings read at open are exactly these (`rtlsdr_check_dongle_model`, e.g. "RTLSDRBlog" / "Blog V4").
    pub fn check_dongle_model(&self, manufact: &str, product: &str) -> bool {
        c_str(&self.manufact) == manufact.as_bytes() && c_str(&self.product) == product.as_bytes()
    }

    /// The manufacturer and product strings read at open (NUL terminated buffers).
    pub fn usb_strings(&self) -> (&[u8], &[u8]) {
        (c_str(&self.manufact), c_str(&self.product))
    }

    /// Opens an RTL2832U whose USB interface 0 the platform layer has already claimed (`rtlsdr_open` from the dummy register write on): resets the
    /// baseband, probes the tuner (E4000, FC0013, R820T, R828D, then FC2580 and FC0012 after a GPIO reset) and initialises it.
    ///
    /// On success returns the device. A dongle whose tuner is an FC0012 or FC2580 is recognised but not driven (rare; the C++ build never
    /// supported them either): the transport comes back with the error -95.
    pub fn open(usb: T) -> Result<Self, (i32, T)> {
        let mut dev = RtlSdr {
            usb,
            rate: 0,
            rtl_xtal: DEF_RTL_XTAL_FREQ,
            fir: FIR_DEFAULT,
            direct_sampling: 0,
            tuner_type: Tuner::Unknown,
            tun_xtal: 0,
            freq: 0,
            bw: 0,
            offs_freq: 0,
            corr: 0,
            gain: 0,
            e4k: E4k::new(),
            r82: R82::new(),
            dev_lost: true,
            manufact: [0; 256],
            product: [0; 256],
            force_bt: false,
            direct_sampling_mode: 0,
        };
        // perform a dummy write, if it fails, reset the device
        if dev.write_reg(USBB, USB_SYSCTL, 0x09, 1) < 0 {
            dev.usb.reset_device();
        }
        dev.init_baseband();
        dev.dev_lost = false;
        // Get device manufacturer and product id
        let mut m = [0u8; 256];
        let mut p = [0u8; 256];
        dev.get_usb_strings(Some(&mut m), Some(&mut p), None);
        dev.manufact = m;
        dev.product = p;

        // Probe tuners
        dev.set_i2c_repeater(true);
        let mut found = Tuner::Unknown;
        if dev.i2c_read_reg(E4K_I2C_ADDR, E4K_CHECK_ADDR) == E4K_CHECK_VAL {
            found = Tuner::E4000;
        } else if dev.i2c_read_reg(FC0013_I2C_ADDR, FC0013_CHECK_ADDR) == FC0013_CHECK_VAL {
            found = Tuner::Fc0013;
        } else if dev.i2c_read_reg(R820T_I2C_ADDR, R82XX_CHECK_ADDR) == R82XX_CHECK_VAL {
            found = Tuner::R820T;
        } else if dev.i2c_read_reg(R828D_I2C_ADDR, R82XX_CHECK_ADDR) == R82XX_CHECK_VAL {
            found = Tuner::R828D;
        } else {
            // initialise GPIOs
            dev.set_gpio_output(4);
            // reset tuner before probing
            dev.set_gpio_bit(4, true);
            dev.set_gpio_bit(4, false);
            if dev.i2c_read_reg(FC2580_I2C_ADDR, FC2580_CHECK_ADDR) & 0x7f == FC2580_CHECK_VAL {
                found = Tuner::Fc2580;
            } else if dev.i2c_read_reg(FC0012_I2C_ADDR, FC0012_CHECK_ADDR) == FC0012_CHECK_VAL {
                dev.set_gpio_output(6);
                found = Tuner::Fc0012;
            }
        }
        dev.tuner_type = found;
        if matches!(found, Tuner::Fc0012 | Tuner::Fc2580) {
            dev.set_i2c_repeater(false);
            return Err((-95, dev.usb));
        }
        // use the rtl clock value by default
        dev.tun_xtal = dev.rtl_xtal;
        match found {
            Tuner::R828D | Tuner::R820T => {
                // If NOT an RTL-SDR Blog V4, set typical R828D 16 MHz freq. Otherwise, keep at 28.8 MHz.
                if found == Tuner::R828D && !dev.check_dongle_model("RTLSDRBlog", "Blog V4") {
                    dev.tun_xtal = R828D_XTAL_FREQ;
                }
                // disable Zero-IF mode
                dev.demod_write_reg(1, 0xb1, 0x1a, 1);
                // only enable In-phase ADC input
                dev.demod_write_reg(0, 0x08, 0x4d, 1);
                // the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode, and 4.57 MHz for the 8 MHz mode
                dev.set_if_freq(R82XX_IF_FREQ);
                // enable spectrum inversion
                dev.demod_write_reg(1, 0x15, 0x01, 1);
            }
            Tuner::Unknown => {
                dev.set_direct_sampling(1);
            }
            _ => {}
        }
        // Hack to force the Bias T to always be on if the IR-Endpoint bit in the EEPROM is 0 (default 1). An unreadable EEPROM leaves it off.
        let mut buf = [0u8; EEPROM_SIZE];
        let r = dev.read_eeprom(&mut buf, 0);
        dev.force_bt = r >= 0 && buf[7] & 0x02 == 0;
        if dev.force_bt {
            dev.set_bias_tee(true);
        }
        if dev.t_has_tuner() {
            dev.t_init();
        }
        dev.set_i2c_repeater(false);
        Ok(dev)
    }

    /// Powers the tuner and demodulator down and gives the transport back (`rtlsdr_close` without the libusb release).
    pub fn close(mut self) -> T {
        if !self.dev_lost {
            self.deinit_baseband();
        }
        self.usb
    }

    /// Flushes the sample FIFO before streaming (`rtlsdr_reset_buffer`).
    pub fn reset_buffer(&mut self) -> i32 {
        self.write_reg(USBB, USB_EPA_CTL, 0x1002, 2);
        self.write_reg(USBB, USB_EPA_CTL, 0x0000, 2);
        0
    }

    /// Reads raw 8 bit I/Q bytes from the bulk endpoint; returns the byte count or a negative error (`rtlsdr_read_sync`).
    pub fn read_sync(&mut self, buf: &mut [u8]) -> i32 {
        self.usb.bulk_read(BULK_ENDPOINT, buf, BULK_TIMEOUT_MS)
    }

    /// Bias-T GPIO `gpio` (0 is the antenna power) (`rtlsdr_set_bias_tee_gpio`). GPIO 0 stays on when the EEPROM forces the bias tee.
    pub fn set_bias_tee_gpio(&mut self, gpio: u8, on: bool) -> i32 {
        let on = if gpio == 0 && self.force_bt { true } else { on };
        self.set_gpio_output(gpio);
        self.set_gpio_bit(gpio, on);
        0
    }

    /// Antenna power (bias-T) on GPIO 0 (`rtlsdr_set_bias_tee`).
    pub fn set_bias_tee(&mut self, on: bool) -> i32 {
        self.set_bias_tee_gpio(0, on)
    }

    /// The transport (for tests and platform code).
    pub fn transport(&mut self) -> &mut T {
        &mut self.usb
    }
}
