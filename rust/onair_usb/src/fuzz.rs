// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Fuzz entry for the transfer layer: `run(data)` drives a whole driver session against a fake dongle whose answers are corrupted according to `data`,
//! and calls the API in an order `data` chooses. It must never panic, loop forever or index out of range, whatever the bytes are. Used by the
//! seeded-mutation test; a libFuzzer / AFL harness can call it directly.

use crate::fake::{FakeChip, FakeRtl};
use crate::rtl::RtlSdr;
use crate::transfer::{UsbString, UsbTransfer};

/// Answers every transfer from `data` bytes (cycling): the byte picks success / error / short / garbage and the content.
struct ByteUsb<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> ByteUsb<'a> {
    fn next(&mut self) -> u8 {
        if self.data.is_empty() {
            return 0;
        }
        let b = self.data[self.pos % self.data.len()];
        self.pos += 1;
        b
    }
}

impl UsbTransfer for ByteUsb<'_> {
    fn control_in(&mut self, _rt: u8, _req: u8, _value: u16, _index: u16, buf: &mut [u8], _t: u32) -> i32 {
        let sel = self.next();
        for b in buf.iter_mut() {
            *b = self.next();
        }
        match sel & 7 {
            0 => -1,
            1 => (buf.len() as i32 - 1).max(0),
            2 => 0,
            3 => -(sel as i32),
            _ => buf.len() as i32,
        }
    }

    fn control_out(&mut self, _rt: u8, _req: u8, _value: u16, _index: u16, data: &[u8], _t: u32) -> i32 {
        match self.next() & 7 {
            0 => -1,
            1 => (data.len() as i32 - 1).max(0),
            2 => 0,
            _ => data.len() as i32,
        }
    }

    fn bulk_read(&mut self, _ep: u8, buf: &mut [u8], _t: u32) -> i32 {
        let sel = self.next();
        if sel & 1 == 0 {
            -7
        } else {
            buf.len() as i32
        }
    }

    fn string_descriptor(&mut self, _w: UsbString, out: &mut [u8]) -> i32 {
        let n = (self.next() as usize).min(out.len());
        for b in out[..n].iter_mut() {
            *b = self.next();
        }
        n as i32
    }
}

/// One API call chosen by a byte; the argument is derived from the following bytes.
fn call<T: UsbTransfer>(d: &mut RtlSdr<T>, op: u8, a: u32) -> i32 {
    match op % 22 {
        0 => d.set_center_freq(a),
        1 => d.set_center_freq(a % 2_200_000_000),
        2 => d.set_sample_rate(a % 4_000_000),
        3 => d.set_freq_correction((a % 2001) as i32 - 1000),
        4 => d.set_tuner_gain_mode((a & 1) as i32),
        5 => d.set_tuner_gain((a % 1200) as i32 - 100),
        6 => d.set_tuner_bandwidth(a % 9_000_000),
        7 => d.set_agc_mode(a & 1 != 0),
        8 => d.set_direct_sampling((a % 4) as i32),
        9 => d.set_offset_tuning(a & 1 != 0),
        10 => d.set_bias_tee(a & 1 != 0),
        11 => d.set_bias_tee_gpio((a % 9) as u8, a & 0x100 != 0),
        12 => d.reset_buffer(),
        13 => {
            let mut b = [0u8; 64];
            d.read_sync(&mut b[..(a % 65) as usize])
        }
        14 => d.set_xtal_freq(28_799_000 + a % 2100, a % 31_000_000),
        15 => {
            let mut b = [0u8; 40];
            d.read_eeprom(&mut b[..(a % 41) as usize], (a >> 8) as u8)
        }
        16 => d.write_eeprom(&[1, 2, 3][..(a % 4) as usize], (a >> 8) as u8),
        17 => d.set_tuner_if_gain((a % 8) as i32, (a >> 4) as i32 % 200 - 50),
        18 => d.set_testmode(a & 1 != 0),
        19 => d.get_tuner_gains(None),
        20 => {
            let mut g = [0i32; 30];
            d.get_tuner_gains(Some(&mut g))
        }
        _ => d.get_direct_sampling(),
    }
}

fn session<T: UsbTransfer>(usb: T, data: &[u8]) -> u32 {
    let mut sum = 0u32;
    if let Ok(mut d) = RtlSdr::open(usb) {
        let mut i = 0;
        while i + 5 <= data.len() && i < 400 {
            let a = u32::from_le_bytes([data[i + 1], data[i + 2], data[i + 3], data[i + 4]]);
            sum = sum.wrapping_add(call(&mut d, data[i], a) as u32);
            i += 5;
        }
        d.close();
    }
    sum
}

/// Runs one fuzz case; the result only exists so the work is not optimised away.
pub fn run(data: &[u8]) -> u32 {
    if data.is_empty() {
        return 0;
    }
    let mode = data[0];
    let rest = &data[1..];
    match mode % 6 {
        0 => session(ByteUsb { data: rest, pos: 0 }, rest),
        k => {
            let chip = match k {
                1 => FakeChip::R820T,
                2 => FakeChip::R828D,
                3 => FakeChip::E4000,
                4 => FakeChip::None,
                _ => FakeChip::Fc0013,
            };
            let mut f = FakeRtl::new(chip);
            f.corrupt_every = if rest.len() > 1 { 1 + rest[0] as u32 % 40 } else { 0 };
            f.lock = rest.len() < 2 || rest[1] & 1 == 0;
            f.r82_vco_cal = rest.get(2).copied().unwrap_or(0x22);
            f.seed(rest.iter().fold(7u32, |h, &b| h.wrapping_mul(31).wrapping_add(b as u32)));
            session(f, rest.get(3..).unwrap_or(&[]))
        }
    }
}
