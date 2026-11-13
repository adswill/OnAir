// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The transport the RTL-SDR protocol layer runs over. Everything above this trait is pure logic: on a desktop an implementation wraps libusb,
//! on Android it wraps the file descriptor of a `UsbDeviceConnection` (usbdevfs ioctls, or `controlTransfer` / `bulkTransfer` called back through JNI).
//! Return values follow libusb: the number of bytes transferred (>= 0) or a negative error code (libusb's `LIBUSB_ERROR_*`, e.g. -1 I/O, -7 timeout, -4 no device).

/// Which USB string descriptor [`UsbTransfer::string_descriptor`] should read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UsbString {
    /// iManufacturer
    Manufacturer,
    /// iProduct
    Product,
    /// iSerialNumber
    Serial,
}

/// `bmRequestType` of a vendor request device -> host (the RTL2832U register protocol).
pub const REQ_VENDOR_IN: u8 = 0xC0;
/// `bmRequestType` of a vendor request host -> device.
pub const REQ_VENDOR_OUT: u8 = 0x40;
/// Control transfer timeout the original driver uses, in milliseconds.
pub const CTRL_TIMEOUT_MS: u32 = 300;
/// Bulk read timeout (0 = none), as the original driver.
pub const BULK_TIMEOUT_MS: u32 = 0;
/// Bulk IN endpoint carrying the samples.
pub const BULK_ENDPOINT: u8 = 0x81;

/// USB access of one claimed RTL2832U interface (interface 0 already claimed, kernel driver detached by the platform layer).
pub trait UsbTransfer {
    /// Control transfer device -> host into `buf`; returns the byte count or a negative error.
    fn control_in(&mut self, request_type: u8, request: u8, value: u16, index: u16, buf: &mut [u8], timeout_ms: u32) -> i32;
    /// Control transfer host -> device sending `data`; returns the byte count or a negative error.
    fn control_out(&mut self, request_type: u8, request: u8, value: u16, index: u16, data: &[u8], timeout_ms: u32) -> i32;
    /// Bulk read from `endpoint` into `buf`; returns the byte count or a negative error.
    fn bulk_read(&mut self, endpoint: u8, buf: &mut [u8], timeout_ms: u32) -> i32;
    /// Reads a string descriptor as ASCII into `out`; returns the length or a negative error. The default has no strings (the dongle is then "generic").
    fn string_descriptor(&mut self, _which: UsbString, _out: &mut [u8]) -> i32 {
        -1
    }
    /// Resets the USB device (the original driver does it when its first register write fails). Default: nothing.
    fn reset_device(&mut self) -> i32 {
        0
    }
    /// Waits `ms` milliseconds (the EEPROM write needs 5 ms between bytes). Default: no wait (fine for a fake device).
    fn delay_ms(&mut self, _ms: u32) {}
}
