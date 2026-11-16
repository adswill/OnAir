# onair_usb

The RTL-SDR protocol layer in Rust, as pure logic over a `UsbTransfer` trait (control transfer in/out, bulk read, optional string descriptor / reset / delay).
The same code runs over libusb on a desktop and over an Android `UsbDeviceConnection` (file descriptor or JNI callbacks); this is the first of the
"Android USB SDR drivers in Rust" (Airspy and HackRF follow as further modules of this crate).

Covered: RTL2832U register / demodulator programming (init, FIR, IF frequency, sample rate with the resampler's rounding, ppm correction, AGC, test mode,
direct sampling I/Q incl. the automatic switch below 24 MHz, offset tuning), I2C repeater, GPIO and bias-T (RTL-SDR Blog V3/V4 upconverter switch, EEPROM
"force bias-T"), EEPROM read/write, tuner probing, and the tuners **R820T / R820T2**, **R828D** (incl. the RTL-SDR Blog V4 band switching and notch filters) and
**E4000** and Fitipower **FC0013** (`fc0013.rs`). Fitipower FC0012 and FCI FC2580 dongles are probed (same transfers as the original) but not driven: `open` returns -95 for them.

## Source and licence

A translation of the RTL-SDR Blog fork of librtlsdr 1.3.6 (`src/librtlsdr.c`, `src/tuner_r82xx.c`, `src/tuner_e4k.c`), itself derived from osmocom rtl-sdr.
**GPL-2.0-or-later**, so the crate is under the same terms (compatible with OnAir's GPL-3). Notices of the original files, kept in the module headers:

* `librtlsdr.c`: Copyright (C) 2012-2014 Steve Markgraf <steve@steve-m.de>, Copyright (C) 2012 Dimitri Stolnikov <horiz0n@gmx.net>; AUTHORS file: Steve Markgraf,
  Dimitri Stolnikov, Hoernchen <la@tfc-server.de>, Kyle Keen <keenerd@gmail.com>; RTL-SDR Blog changes (V4 support, auto direct sampling, bias-T hacks).
* `tuner_r82xx.c`: Rafael Micro R820T/R828D driver, Copyright (C) 2013 Mauro Carvalho Chehab <mchehab@redhat.com>, Copyright (C) 2013 Steve Markgraf; a heavily
  modified version of the Linux kernel driver `drivers/media/tuners/r820t.c`.
* `tuner_e4k.c`: Elonics E4000 tuner driver, (C) 2011-2012 Harald Welte <laforge@gnumonks.org>, (C) 2012 Sylvain Munaut <tnt@246tNt.com>, (C) 2012 Hoernchen.

`tools/credits_data.tsv` does not exist in this tree yet; add these authors when it does (issue "follow licences"). The test reference (`tests/ref/ref_harness.c`)
is OnAir's own code; the original librtlsdr is NOT in the repo, `gen_golden.sh` takes it from a path you give.

OnAir's C++ build does not contain a register level RTL-SDR driver: `core/src/source/source_rtlsdr.cpp` loads the installed `librtlsdr` at run time. This crate
is therefore translated from the C original, not from the C++.

## API

`RtlSdr<T: UsbTransfer>`: `open` (after the platform layer claimed interface 0), `close`, `set_center_freq`, `set_sample_rate`, `set_freq_correction`,
`set_tuner_gain_mode`, `set_tuner_gain`, `set_tuner_if_gain`, `set_tuner_bandwidth`, `set_agc_mode`, `set_direct_sampling`, `set_offset_tuning`,
`set_bias_tee[_gpio]`, `set_xtal_freq`, `reset_buffer`, `read_sync`, `read/write_eeprom`, getters, `get_tuner_gains`. Names and return values are librtlsdr's
(0 / count on success, negative on failure); control flow and integer conversions follow the C so the transfers are identical. Deliberate differences: no
`fprintf`; scratch buffers are zeroed (the C reads uninitialised stack in a few failure paths); an unreadable EEPROM leaves the "force bias-T" hack off;
enumeration / open / claim / kernel-driver detach / the async transfer ring are libusb things and stay in the platform layer (`read_sync` is the data path).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/usb.h`, C++ wrapper `core/include/dect2/rs/usb_rtl.hpp`. No allocation: the caller provides
`onair_usb_rtl_size()` bytes. Build switch **`DECT2_RUST_USB`, default OFF**: the desktop C++ driver stays the default until a real device test is possible.

## Tests

* **Differential** (`cargo test -p onair_usb`): `tests/golden/scenarios.txt` has 86 command scripts (open, rates, frequency sweeps over every tuner band, gains,
  bandwidths, direct sampling, ppm, xtal, bias-T, EEPROM, 48 fault-injection scripts where the Nth control transfer fails). `tests/golden/<name>.log` is what the ORIGINAL librtlsdr
  does over a fake libusb (`tests/ref/ref_harness.c` models the RTL2832U registers, the I2C repeater, R820T / R828D / E4000 / none and the EEPROM). The Rust
  driver is replayed against each log: every transfer (direction, request, value, index, length, timeout, outgoing bytes) and every API return value must be
  identical: 57 374 transfers, 0 mismatches. Regenerate with `RTLSDR_SRC=<rtl-sdr-blog-1.3.6> tests/ref/gen_golden.sh` (needs only a C compiler and the libusb header).
* Malformed answers: dead device (every transfer fails), short / flipped / error answers from the fake dongle at every period 1..30, unsupported tuner.
* Fuzz of the transfer layer: `fuzz::run(&[u8])` drives a whole session against answers chosen by the input; the test mutates 16 000 inputs. It is the entry
  point for a libFuzzer / AFL harness (no external crates here).
* FFI round trip over C callbacks; the header and `ffi.rs` list the same functions (`rust/onair/tests/headers.rs`).
* `cargo check --target aarch64-linux-android -p onair_usb` passes (pure Rust, no NDK needed).

## NOT verified without hardware

Everything above runs against fakes. The golden logs prove that the Rust driver sends the same bytes as librtlsdr to a *modelled* dongle; they cannot prove:
real timing (the C has no delays in the tuner code either, but the EEPROM write needs 5 ms: `delay_ms`), real PLL lock / calibration behaviour of a given R820T2
batch, the V4 input switching on real hardware, E4000 (the model is a plain register file with a lock bit), USB error codes on Android, and anything about
the Android permission / fd hand-over below. The model's answers are my reading of the chips, not recordings.

## Linking into the Android app (sketch; `~/onair-android` is not touched)

Kotlin: `UsbManager.requestPermission` -> `openDevice(device)` -> `UsbDeviceConnection`; `connection.claimInterface(iface, true)`; pass `connection.fileDescriptor`
and the endpoint number to JNI. Native side, option A (usbdevfs, no Java calls per transfer):

```cpp
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include "dect2/rs/usb_rtl.hpp"
struct FdUsb : dect2::rs::RtlTransport {                       // fd from UsbDeviceConnection.getFileDescriptor()
    int fd;
    int controlIn(uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, uint8_t* d, uint16_t n, uint32_t t) override {
        usbdevfs_ctrltransfer c{rt, rq, v, i, n, t, d};  return ioctl(fd, USBDEVFS_CONTROL, &c);   // bytes or -1 (errno)
    }
    int controlOut(uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, const uint8_t* d, uint16_t n, uint32_t t) override {
        usbdevfs_ctrltransfer c{rt, rq, v, i, n, t, const_cast<uint8_t*>(d)};  return ioctl(fd, USBDEVFS_CONTROL, &c);
    }
    int bulkRead(uint8_t ep, uint8_t* d, int n, uint32_t t) override {
        usbdevfs_bulktransfer b{ep, (unsigned)n, t, d};  return ioctl(fd, USBDEVFS_BULK, &b);
    }
    int resetDevice() override { return ioctl(fd, USBDEVFS_RESET, nullptr); }
    void delayMs(uint32_t ms) override { usleep(ms * 1000); }
    // stringDescriptor: USBDEVFS_CONTROL GET_DESCRIPTOR (0x80, 6, (3<<8)|index, langid 0x0409) or pass the strings from Kotlin (UsbDevice.manufacturerName)
};
extern "C" JNIEXPORT jlong JNICALL Java_..._nativeOpen(JNIEnv*, jobject, jint fd) {
    auto* u = new FdUsb; u->fd = fd; int err = 0;
    auto rtl = dect2::rs::RtlSdrRs::open(*u, &err);      // -95: FC0012/FC2580 tuner
    ...                                                  // keep {u, rtl}; the sample thread loops rtl->readSync(buf, 262144) (a multiple of 512)
}
```

Option B: libusb with `libusb_wrap_sys_device(ctx, fd, &handle)` (libusb >= 1.0.23, `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`) and the three transfer functions forwarding to
`libusb_control_transfer` / `libusb_bulk_transfer` (what a desktop build does with `libusb_open`). Build the Rust library for the NDK target
(`cargo build --release -p onair --features bare --target aarch64-linux-android`, linker = NDK clang) and link `libonair_rs.a` into the JNI library with
`-DDECT2_RUST_USB` for the header. Use `read_sync` on a worker thread with blocks of 16 KiB multiples; call `reset_buffer` first; stop the thread before `close`.
