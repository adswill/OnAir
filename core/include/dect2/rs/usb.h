// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_usb (USB SDR drivers). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Only meaningful when DECT2_RUST_USB is defined (see rust/README.md). Default OFF: the desktop C++ driver (dlopen of librtlsdr) stays the default.
//
// The RTL-SDR protocol layer (RTL2832U registers, R820T / R820T2 / R828D and E4000 tuners) as pure logic over a table of transfer callbacks, so the same
// code runs over libusb on a desktop and over an Android UsbDeviceConnection (file descriptor or JNI callbacks). Function names, arguments and return
// values follow librtlsdr (the RTL-SDR Blog fork 1.3.6): 0 or a count on success, negative on failure. The object lives in memory the caller provides.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_usb_version(void);

// The transport. All transfers are on the device that interface 0 of was claimed by the caller. Return the byte count (>= 0) or a negative libusb error code.
// control_in / control_out / bulk_read are required, the others may be NULL. `ctx` is handed back unchanged.
typedef struct onair_usb_ops {
    void* ctx;
    int32_t (*control_in)(void* ctx, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, uint32_t timeout_ms);
    int32_t (*control_out)(void* ctx, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, uint16_t len, uint32_t timeout_ms);
    int32_t (*bulk_read)(void* ctx, uint8_t endpoint, uint8_t* data, int32_t len, uint32_t timeout_ms);
    // String descriptor as ASCII into out[0..cap): which 0 = manufacturer, 1 = product, 2 = serial. Returns the length or a negative error.
    int32_t (*string_descriptor)(void* ctx, int32_t which, uint8_t* out, int32_t cap);
    // Reset the USB device (done when the first register write fails).
    int32_t (*reset_device)(void* ctx);
    // Sleep (the EEPROM write needs 5 ms between bytes).
    void (*delay_ms)(void* ctx, uint32_t ms);
} onair_usb_ops;

typedef void onair_usb_rtl_t;   // an opened RTL-SDR (memory of onair_usb_rtl_size() bytes)

// Size and alignment of the memory for one RTL-SDR object (allocate with aligned_alloc / operator new[] with alignment; the crate does not allocate).
size_t onair_usb_rtl_size(void);
size_t onair_usb_rtl_align(void);
// Opens the dongle behind `ops` into `mem` (resets the baseband, probes and initialises the tuner). 0 on success; -1 bad arguments; -95 an FC0012 / FC0013 /
// FC2580 tuner (recognised, not driven). `ops` is copied. After 0, release with onair_usb_rtl_close.
int32_t onair_usb_rtl_open(void* mem, size_t mem_len, const onair_usb_ops* ops);
// Powers the tuner and the demodulator down. The memory can be freed afterwards; releasing the USB interface is the caller's job.
int32_t onair_usb_rtl_close(onair_usb_rtl_t* h);

// rtlsdr_set_center_freq: 0 on success. Below 24 MHz an R820T switches to direct sampling by itself (the Blog fork's behaviour).
int32_t onair_usb_rtl_set_center_freq(onair_usb_rtl_t* h, uint32_t freq);
// The frequency last set, 0 after a failed tune.
uint32_t onair_usb_rtl_get_center_freq(onair_usb_rtl_t* h);
// rtlsdr_set_sample_rate: 225001..300000 and 900001..3200000 Hz, otherwise -22.
int32_t onair_usb_rtl_set_sample_rate(onair_usb_rtl_t* h, uint32_t rate);
// The rate in effect after the resampler's rounding.
uint32_t onair_usb_rtl_get_sample_rate(onair_usb_rtl_t* h);
// rtlsdr_set_freq_correction in ppm; -2 when unchanged.
int32_t onair_usb_rtl_set_freq_correction(onair_usb_rtl_t* h, int32_t ppm);
// rtlsdr_tuner: 0 unknown, 1 E4000, 5 R820T / R820T2, 6 R828D; -1 for a bad handle.
int32_t onair_usb_rtl_get_tuner_type(onair_usb_rtl_t* h);
// Number of discrete tuner gains (tenths of a dB); with a non-NULL `out` of `n` ints also copies them.
int32_t onair_usb_rtl_get_tuner_gains(onair_usb_rtl_t* h, int32_t* out, size_t n);
// Gain mode: 0 automatic, 1 manual.
int32_t onair_usb_rtl_set_tuner_gain_mode(onair_usb_rtl_t* h, int32_t manual);
// Manual gain in tenths of a dB.
int32_t onair_usb_rtl_set_tuner_gain(onair_usb_rtl_t* h, int32_t gain);
// IF stage gain of the E4000 (stage 1..6, tenths of a dB); 0 and nothing done for other tuners.
int32_t onair_usb_rtl_set_tuner_if_gain(onair_usb_rtl_t* h, int32_t stage, int32_t gain);
// Tuner IF bandwidth in Hz, 0 = the sample rate.
int32_t onair_usb_rtl_set_tuner_bandwidth(onair_usb_rtl_t* h, uint32_t bw);
// The RTL2832U's own AGC.
int32_t onair_usb_rtl_set_agc_mode(onair_usb_rtl_t* h, int32_t on);
// Direct sampling: 0 off (automatic on an R820T below 24 MHz), 1 I branch, 2 Q branch.
int32_t onair_usb_rtl_set_direct_sampling(onair_usb_rtl_t* h, int32_t on);
// Offset tuning (E4000). On an R820T / R828D the RTL-SDR Blog fork switches the bias-T with it instead and returns -2.
int32_t onair_usb_rtl_set_offset_tuning(onair_usb_rtl_t* h, int32_t on);
// Antenna power (bias-T, GPIO 0). The EEPROM can force it on (then it cannot be turned off).
int32_t onair_usb_rtl_set_bias_tee(onair_usb_rtl_t* h, int32_t on);
// Any GPIO as an output (GPIO 5 switches the upconverter of the RTL-SDR Blog V4).
int32_t onair_usb_rtl_set_bias_tee_gpio(onair_usb_rtl_t* h, int32_t gpio, int32_t on);
// Flush the sample FIFO; call before streaming.
int32_t onair_usb_rtl_reset_buffer(onair_usb_rtl_t* h);
// Reads raw 8 bit I/Q bytes from the bulk endpoint: the byte count or a negative error.
int32_t onair_usb_rtl_read_sync(onair_usb_rtl_t* h, uint8_t* buf, size_t len);
// Crystal frequencies in Hz (0 keeps the default 28.8 MHz).
int32_t onair_usb_rtl_set_xtal_freq(onair_usb_rtl_t* h, uint32_t rtl_freq, uint32_t tuner_freq);
// EEPROM access (256 bytes).
int32_t onair_usb_rtl_read_eeprom(onair_usb_rtl_t* h, uint8_t* out, uint32_t offset, size_t len);
int32_t onair_usb_rtl_write_eeprom(onair_usb_rtl_t* h, const uint8_t* data, uint32_t offset, size_t len);
// 1 when the manufacturer / product strings read at open equal the NUL terminated arguments (e.g. "RTLSDRBlog", "Blog V4"), else 0.
int32_t onair_usb_rtl_check_dongle_model(onair_usb_rtl_t* h, const uint8_t* manufact, const uint8_t* product);

#ifdef __cplusplus
}
#endif
