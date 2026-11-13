// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_usb` (core/include/dect2/rs/usb.h). Every export is `onair_usb_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//!
//! The RTL-SDR object lives in caller supplied memory (`onair_usb_rtl_size()` bytes, aligned to `onair_usb_rtl_align()`): the crate does not allocate.
//! The transport is a table of C callbacks ([`Ops`]). A null handle, a null buffer or a bad length gives -1 (or 0 for the getters).
#![allow(clippy::missing_safety_doc)]

use crate::rtl::RtlSdr;
use crate::transfer::{UsbString, UsbTransfer};
use core::mem::{align_of, size_of};

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_usb_version() -> u32 {
    1
}

/// The transport callbacks (`onair_usb_ops` in usb.h). `ctx` is passed to every callback. The three transfer callbacks are required, the other three may be null.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Ops {
    /// Opaque pointer handed back to every callback.
    pub ctx: *mut core::ffi::c_void,
    /// Control transfer device -> host.
    pub control_in: Option<
        unsafe extern "C" fn(
            ctx: *mut core::ffi::c_void,
            request_type: u8,
            request: u8,
            value: u16,
            index: u16,
            data: *mut u8,
            len: u16,
            timeout_ms: u32,
        ) -> i32,
    >,
    /// Control transfer host -> device.
    pub control_out: Option<
        unsafe extern "C" fn(
            ctx: *mut core::ffi::c_void,
            request_type: u8,
            request: u8,
            value: u16,
            index: u16,
            data: *const u8,
            len: u16,
            timeout_ms: u32,
        ) -> i32,
    >,
    /// Bulk read.
    pub bulk_read: Option<unsafe extern "C" fn(ctx: *mut core::ffi::c_void, endpoint: u8, data: *mut u8, len: i32, timeout_ms: u32) -> i32>,
    /// String descriptor as ASCII (which: 0 manufacturer, 1 product, 2 serial); returns the length or a negative error. May be null.
    pub string_descriptor: Option<unsafe extern "C" fn(ctx: *mut core::ffi::c_void, which: i32, out: *mut u8, cap: i32) -> i32>,
    /// Reset the USB device. May be null.
    pub reset_device: Option<unsafe extern "C" fn(ctx: *mut core::ffi::c_void) -> i32>,
    /// Sleep for `ms` milliseconds. May be null.
    pub delay_ms: Option<unsafe extern "C" fn(ctx: *mut core::ffi::c_void, ms: u32)>,
}

/// The [`UsbTransfer`] over the C callbacks.
pub struct CbUsb {
    ops: Ops,
}

impl UsbTransfer for CbUsb {
    fn control_in(&mut self, request_type: u8, request: u8, value: u16, index: u16, buf: &mut [u8], timeout_ms: u32) -> i32 {
        match self.ops.control_in {
            // SAFETY: the pointer/length come from a live slice; the callback contract is documented in usb.h.
            Some(f) => unsafe { f(self.ops.ctx, request_type, request, value, index, buf.as_mut_ptr(), buf.len() as u16, timeout_ms) },
            None => -1,
        }
    }

    fn control_out(&mut self, request_type: u8, request: u8, value: u16, index: u16, data: &[u8], timeout_ms: u32) -> i32 {
        match self.ops.control_out {
            // SAFETY: as above.
            Some(f) => unsafe { f(self.ops.ctx, request_type, request, value, index, data.as_ptr(), data.len() as u16, timeout_ms) },
            None => -1,
        }
    }

    fn bulk_read(&mut self, endpoint: u8, buf: &mut [u8], timeout_ms: u32) -> i32 {
        let len = buf.len().min(i32::MAX as usize) as i32;
        match self.ops.bulk_read {
            // SAFETY: as above.
            Some(f) => unsafe { f(self.ops.ctx, endpoint, buf.as_mut_ptr(), len, timeout_ms) },
            None => -1,
        }
    }

    fn string_descriptor(&mut self, which: UsbString, out: &mut [u8]) -> i32 {
        let w = match which {
            UsbString::Manufacturer => 0,
            UsbString::Product => 1,
            UsbString::Serial => 2,
        };
        match self.ops.string_descriptor {
            // SAFETY: as above.
            Some(f) => unsafe { f(self.ops.ctx, w, out.as_mut_ptr(), out.len() as i32) },
            None => -1,
        }
    }

    fn reset_device(&mut self) -> i32 {
        match self.ops.reset_device {
            // SAFETY: as above.
            Some(f) => unsafe { f(self.ops.ctx) },
            None => 0,
        }
    }

    fn delay_ms(&mut self, ms: u32) {
        if let Some(f) = self.ops.delay_ms {
            // SAFETY: as above.
            unsafe { f(self.ops.ctx, ms) }
        }
    }
}

type Dev = RtlSdr<CbUsb>;

/// Size in bytes of the memory the caller provides for one RTL-SDR object.
#[no_mangle]
pub extern "C" fn onair_usb_rtl_size() -> usize {
    size_of::<Dev>()
}

/// Alignment of that memory.
#[no_mangle]
pub extern "C" fn onair_usb_rtl_align() -> usize {
    align_of::<Dev>()
}

unsafe fn dev<'a>(h: *mut u8) -> Option<&'a mut Dev> {
    if h.is_null() || (h as usize) % align_of::<Dev>() != 0 {
        None
    } else {
        Some(&mut *(h as *mut Dev))
    }
}

/// Opens the RTL2832U behind `ops` (interface already claimed) into `mem`. 0 on success (call `onair_usb_rtl_close` later), otherwise a negative error and
/// `mem` stays unused: -1 bad arguments, -95 an FC0012 / FC0013 / FC2580 tuner (recognised, not driven).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_open(mem: *mut u8, mem_len: usize, ops: *const Ops) -> i32 {
    if mem.is_null() || ops.is_null() || mem_len < size_of::<Dev>() || (mem as usize) % align_of::<Dev>() != 0 {
        return -1;
    }
    let ops = *ops;
    if ops.control_in.is_none() || ops.control_out.is_none() || ops.bulk_read.is_none() {
        return -1;
    }
    match Dev::open(CbUsb { ops }) {
        Ok(d) => {
            core::ptr::write(mem as *mut Dev, d);
            0
        }
        Err((e, _)) => e,
    }
}

/// Powers the radio down and ends the object (the memory can be freed afterwards). The USB interface is the caller's to release.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_close(h: *mut u8) -> i32 {
    match dev(h) {
        Some(_) => {
            let d = core::ptr::read(h as *mut Dev);
            d.close();
            0
        }
        None => -1,
    }
}

macro_rules! with_dev {
    ($h:expr, $d:ident, $body:expr, $none:expr) => {
        match dev($h) {
            Some($d) => $body,
            None => $none,
        }
    };
}

/// Tune to `freq` Hz (rtlsdr_set_center_freq). 0 on success.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_center_freq(h: *mut u8, freq: u32) -> i32 {
    with_dev!(h, d, d.set_center_freq(freq), -1)
}

/// The frequency last set, 0 after a failed tune.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_get_center_freq(h: *mut u8) -> u32 {
    with_dev!(h, d, d.get_center_freq(), 0)
}

/// Sample rate in Hz (rtlsdr_set_sample_rate); -22 outside 225001..300000 and 900001..3200000.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_sample_rate(h: *mut u8, rate: u32) -> i32 {
    with_dev!(h, d, d.set_sample_rate(rate), -1)
}

/// The sample rate in effect after rounding.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_get_sample_rate(h: *mut u8) -> u32 {
    with_dev!(h, d, d.get_sample_rate(), 0)
}

/// Frequency correction in ppm (rtlsdr_set_freq_correction); -2 when unchanged.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_freq_correction(h: *mut u8, ppm: i32) -> i32 {
    with_dev!(h, d, d.set_freq_correction(ppm), -1)
}

/// Tuner type (rtlsdr_tuner: 0 unknown, 1 E4000, 5 R820T, 6 R828D), -1 for a bad handle.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_get_tuner_type(h: *mut u8) -> i32 {
    with_dev!(h, d, d.get_tuner_type().code(), -1)
}

/// Number of tuner gains; with a non-null `out` of `n` ints also copies them (tenths of a dB).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_get_tuner_gains(h: *mut u8, out: *mut i32, n: usize) -> i32 {
    with_dev!(
        h,
        d,
        {
            if out.is_null() || n == 0 {
                d.get_tuner_gains(None)
            } else {
                d.get_tuner_gains(Some(core::slice::from_raw_parts_mut(out, n)))
            }
        },
        -1
    )
}

/// Tuner gain mode: 0 automatic, 1 manual (rtlsdr_set_tuner_gain_mode).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_tuner_gain_mode(h: *mut u8, manual: i32) -> i32 {
    with_dev!(h, d, d.set_tuner_gain_mode(manual), -1)
}

/// Manual tuner gain in tenths of a dB (rtlsdr_set_tuner_gain).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_tuner_gain(h: *mut u8, gain: i32) -> i32 {
    with_dev!(h, d, d.set_tuner_gain(gain), -1)
}

/// IF stage gain of the E4000 (rtlsdr_set_tuner_if_gain); 0 for other tuners.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_tuner_if_gain(h: *mut u8, stage: i32, gain: i32) -> i32 {
    with_dev!(h, d, d.set_tuner_if_gain(stage, gain), -1)
}

/// Tuner IF bandwidth in Hz, 0 = the sample rate (rtlsdr_set_tuner_bandwidth).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_tuner_bandwidth(h: *mut u8, bw: u32) -> i32 {
    with_dev!(h, d, d.set_tuner_bandwidth(bw), -1)
}

/// The RTL2832U AGC on or off (rtlsdr_set_agc_mode).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_agc_mode(h: *mut u8, on: i32) -> i32 {
    with_dev!(h, d, d.set_agc_mode(on != 0), -1)
}

/// Direct sampling: 0 off / automatic below 24 MHz on an R820T, 1 I branch, 2 Q branch (rtlsdr_set_direct_sampling).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_direct_sampling(h: *mut u8, on: i32) -> i32 {
    with_dev!(h, d, d.set_direct_sampling(on), -1)
}

/// Offset tuning (E4000); on an R820T / R828D the Blog fork switches the bias-T instead and returns -2.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_offset_tuning(h: *mut u8, on: i32) -> i32 {
    with_dev!(h, d, d.set_offset_tuning(on != 0), -1)
}

/// Bias-T (antenna power) on GPIO 0 (rtlsdr_set_bias_tee).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_bias_tee(h: *mut u8, on: i32) -> i32 {
    with_dev!(h, d, d.set_bias_tee(on != 0), -1)
}

/// Any GPIO as bias-T output (rtlsdr_set_bias_tee_gpio).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_bias_tee_gpio(h: *mut u8, gpio: i32, on: i32) -> i32 {
    with_dev!(h, d, d.set_bias_tee_gpio(gpio as u8, on != 0), -1)
}

/// Flush the sample FIFO before streaming (rtlsdr_reset_buffer).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_reset_buffer(h: *mut u8) -> i32 {
    with_dev!(h, d, d.reset_buffer(), -1)
}

/// Read raw 8 bit I/Q bytes: the byte count or a negative error (rtlsdr_read_sync).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_read_sync(h: *mut u8, buf: *mut u8, len: usize) -> i32 {
    if buf.is_null() || len == 0 {
        return -1;
    }
    with_dev!(h, d, d.read_sync(core::slice::from_raw_parts_mut(buf, len)), -1)
}

/// Crystal frequencies, 0 keeps the default (rtlsdr_set_xtal_freq).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_set_xtal_freq(h: *mut u8, rtl_freq: u32, tuner_freq: u32) -> i32 {
    with_dev!(h, d, d.set_xtal_freq(rtl_freq, tuner_freq), -1)
}

/// Read EEPROM bytes at `offset` (rtlsdr_read_eeprom).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_read_eeprom(h: *mut u8, out: *mut u8, offset: u32, len: usize) -> i32 {
    if out.is_null() || len == 0 {
        return -1;
    }
    with_dev!(h, d, d.read_eeprom(core::slice::from_raw_parts_mut(out, len), offset as u8), -1)
}

/// Write EEPROM bytes at `offset` (rtlsdr_write_eeprom).
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_write_eeprom(h: *mut u8, data: *const u8, offset: u32, len: usize) -> i32 {
    if data.is_null() || len == 0 {
        return -1;
    }
    with_dev!(h, d, d.write_eeprom(core::slice::from_raw_parts(data, len), offset as u8), -1)
}

/// True (1) when the manufacturer / product strings read at open equal the NUL terminated `manufact` / `product` (rtlsdr_check_dongle_model); 0 otherwise.
#[no_mangle]
pub unsafe extern "C" fn onair_usb_rtl_check_dongle_model(h: *mut u8, manufact: *const u8, product: *const u8) -> i32 {
    if manufact.is_null() || product.is_null() {
        return 0;
    }
    let (m, p) = (c_bytes(manufact), c_bytes(product));
    with_dev!(
        h,
        d,
        {
            let (dm, dp) = d.usb_strings();
            i32::from(dm == m && dp == p)
        },
        0
    )
}

unsafe fn c_bytes<'a>(p: *const u8) -> &'a [u8] {
    let mut n = 0;
    while n < 255 && *p.add(n) != 0 {
        n += 1;
    }
    core::slice::from_raw_parts(p, n)
}

#[cfg(test)]
mod ffi_tests {
    extern crate std;
    use super as ffi;
    use super::Ops;
    use crate::fake::{FakeChip, FakeRtl};
    use crate::transfer::{UsbString, UsbTransfer};
    #[test]
    fn ffi_matches_the_safe_api() {
        use core::ffi::c_void;

        unsafe extern "C" fn cin(ctx: *mut c_void, rt: u8, req: u8, v: u16, i: u16, d: *mut u8, n: u16, t: u32) -> i32 {
            let f = &mut *(ctx as *mut FakeRtl);
            f.control_in(rt, req, v, i, core::slice::from_raw_parts_mut(d, n as usize), t)
        }
        unsafe extern "C" fn cout(ctx: *mut c_void, rt: u8, req: u8, v: u16, i: u16, d: *const u8, n: u16, t: u32) -> i32 {
            let f = &mut *(ctx as *mut FakeRtl);
            f.control_out(rt, req, v, i, core::slice::from_raw_parts(d, n as usize), t)
        }
        unsafe extern "C" fn bulk(ctx: *mut c_void, ep: u8, d: *mut u8, n: i32, t: u32) -> i32 {
            let f = &mut *(ctx as *mut FakeRtl);
            f.bulk_read(ep, core::slice::from_raw_parts_mut(d, n as usize), t)
        }
        unsafe extern "C" fn strd(ctx: *mut c_void, w: i32, out: *mut u8, cap: i32) -> i32 {
            let f = &mut *(ctx as *mut FakeRtl);
            let which = [UsbString::Manufacturer, UsbString::Product, UsbString::Serial][w as usize % 3];
            f.string_descriptor(which, core::slice::from_raw_parts_mut(out, cap as usize))
        }

        let mut fake = FakeRtl::new(FakeChip::R820T);
        let ops = Ops {
            ctx: &mut fake as *mut FakeRtl as *mut c_void,
            control_in: Some(cin),
            control_out: Some(cout),
            bulk_read: Some(bulk),
            string_descriptor: Some(strd),
            reset_device: None,
            delay_ms: None,
        };
        let size = ffi::onair_usb_rtl_size();
        let align = ffi::onair_usb_rtl_align();
        assert!(size > 0 && align.is_power_of_two());
        let mut mem = std::vec![0u8; size + align];
        let off = mem.as_ptr().align_offset(align);
        let h = unsafe { mem.as_mut_ptr().add(off) };
        unsafe {
            assert_eq!(ffi::onair_usb_rtl_open(core::ptr::null_mut(), size, &ops), -1);
            assert_eq!(ffi::onair_usb_rtl_open(h, size - 1, &ops), -1);
            assert_eq!(ffi::onair_usb_rtl_open(h, size, &ops), 0);
            assert_eq!(ffi::onair_usb_rtl_get_tuner_type(h), 5);
            assert_eq!(ffi::onair_usb_rtl_set_sample_rate(h, 2_048_000), 0);
            assert_eq!(ffi::onair_usb_rtl_set_center_freq(h, 100_000_000), 0);
            assert_eq!(ffi::onair_usb_rtl_get_center_freq(h), 100_000_000);
            assert_eq!(ffi::onair_usb_rtl_get_tuner_gains(h, core::ptr::null_mut(), 0), 29);
            let mut g = [0i32; 29];
            assert_eq!(ffi::onair_usb_rtl_get_tuner_gains(h, g.as_mut_ptr(), 29), 29);
            assert_eq!(g[28], 496);
            let mut b = [0u8; 32];
            assert_eq!(ffi::onair_usb_rtl_read_sync(h, b.as_mut_ptr(), 32), 32);
            assert_eq!(b[1], 10);
            assert_eq!(ffi::onair_usb_rtl_read_sync(h, core::ptr::null_mut(), 32), -1);
            assert_eq!(ffi::onair_usb_rtl_check_dongle_model(h, b"Realtek\0".as_ptr(), b"RTL2838UHIDIR\0".as_ptr()), 1);
            assert_eq!(ffi::onair_usb_rtl_check_dongle_model(h, b"Realtek\0".as_ptr(), b"x\0".as_ptr()), 0);
            assert_eq!(ffi::onair_usb_rtl_close(h), 0);
            assert_eq!(ffi::onair_usb_rtl_set_center_freq(core::ptr::null_mut(), 1), -1);
        }
    }
}
