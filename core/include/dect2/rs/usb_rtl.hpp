// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C++ wrapper of the Rust RTL-SDR protocol layer (rs/usb.h), for a platform layer that owns the USB access (libusb on a desktop, a file descriptor of an
// Android UsbDeviceConnection through JNI). Only meaningful when DECT2_RUST_USB is defined. Header only.
//
//   struct MyUsb : dect2::rs::RtlTransport { ... controlIn / controlOut / bulkRead over your USB handle ... };
//   MyUsb usb;  int err = 0;
//   auto rtl = dect2::rs::RtlSdrRs::open(usb, &err);        // interface 0 claimed, kernel driver detached
//   rtl->setSampleRate(2048000); rtl->setCenterFreq(100000000); rtl->setTunerGainMode(1); rtl->setTunerGain(300);
//   rtl->resetBuffer(); while (run) { int n = rtl->readSync(buf, sizeof buf); ... }
#pragma once
#ifdef DECT2_RUST_USB
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include "dect2/rs/usb.h"

namespace dect2 {
namespace rs {

// The USB access the protocol layer runs over. Return the byte count (>= 0) or a negative libusb error code.
class RtlTransport {
public:
    virtual ~RtlTransport() = default;
    virtual int controlIn(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len, uint32_t timeoutMs) = 0;
    virtual int controlOut(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, uint16_t len, uint32_t timeoutMs) = 0;
    virtual int bulkRead(uint8_t endpoint, uint8_t* data, int len, uint32_t timeoutMs) = 0;
    // ASCII string descriptor (which: 0 manufacturer, 1 product, 2 serial): the length or a negative error.
    virtual int stringDescriptor(int which, uint8_t* out, int cap) { (void)which; (void)out; (void)cap; return -1; }
    virtual int resetDevice() { return 0; }
    virtual void delayMs(uint32_t ms) { (void)ms; }
};

class RtlSdrRs {
public:
    // Opens the dongle. `transport` must outlive the object. Returns null and sets *err (-95: unsupported Fitipower / FCI tuner) on failure.
    static std::unique_ptr<RtlSdrRs> open(RtlTransport& transport, int* err = nullptr) {
        onair_usb_ops ops{};
        ops.ctx = &transport;
        ops.control_in = [](void* c, uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, uint8_t* d, uint16_t n, uint32_t t) -> int32_t {
            return static_cast<RtlTransport*>(c)->controlIn(rt, rq, v, i, d, n, t); };
        ops.control_out = [](void* c, uint8_t rt, uint8_t rq, uint16_t v, uint16_t i, const uint8_t* d, uint16_t n, uint32_t t) -> int32_t {
            return static_cast<RtlTransport*>(c)->controlOut(rt, rq, v, i, d, n, t); };
        ops.bulk_read = [](void* c, uint8_t ep, uint8_t* d, int32_t n, uint32_t t) -> int32_t { return static_cast<RtlTransport*>(c)->bulkRead(ep, d, n, t); };
        ops.string_descriptor = [](void* c, int32_t w, uint8_t* o, int32_t cap) -> int32_t { return static_cast<RtlTransport*>(c)->stringDescriptor(w, o, cap); };
        ops.reset_device = [](void* c) -> int32_t { return static_cast<RtlTransport*>(c)->resetDevice(); };
        ops.delay_ms = [](void* c, uint32_t ms) { static_cast<RtlTransport*>(c)->delayMs(ms); };
        const size_t size = onair_usb_rtl_size(), align = onair_usb_rtl_align();
        void* mem = ::operator new(size, std::align_val_t(align));
        const int r = onair_usb_rtl_open(mem, size, &ops);
        if (r != 0) {
            ::operator delete(mem, std::align_val_t(align));
            if (err) *err = r;
            return nullptr;
        }
        return std::unique_ptr<RtlSdrRs>(new RtlSdrRs(mem, align));
    }
    ~RtlSdrRs() {
        onair_usb_rtl_close(h_);
        ::operator delete(h_, std::align_val_t(align_));
    }
    RtlSdrRs(const RtlSdrRs&) = delete;
    RtlSdrRs& operator=(const RtlSdrRs&) = delete;

    int setCenterFreq(uint32_t hz) { return onair_usb_rtl_set_center_freq(h_, hz); }
    uint32_t centerFreq() { return onair_usb_rtl_get_center_freq(h_); }
    int setSampleRate(uint32_t hz) { return onair_usb_rtl_set_sample_rate(h_, hz); }
    uint32_t sampleRate() { return onair_usb_rtl_get_sample_rate(h_); }
    int setFreqCorrection(int ppm) { return onair_usb_rtl_set_freq_correction(h_, ppm); }
    int tunerType() { return onair_usb_rtl_get_tuner_type(h_); }
    int tunerGains(int32_t* out, size_t n) { return onair_usb_rtl_get_tuner_gains(h_, out, n); }
    int setTunerGainMode(int manual) { return onair_usb_rtl_set_tuner_gain_mode(h_, manual); }
    int setTunerGain(int tenthsDb) { return onair_usb_rtl_set_tuner_gain(h_, tenthsDb); }
    int setTunerBandwidth(uint32_t hz) { return onair_usb_rtl_set_tuner_bandwidth(h_, hz); }
    int setAgcMode(bool on) { return onair_usb_rtl_set_agc_mode(h_, on); }
    int setDirectSampling(int mode) { return onair_usb_rtl_set_direct_sampling(h_, mode); }
    int setOffsetTuning(bool on) { return onair_usb_rtl_set_offset_tuning(h_, on); }
    int setBiasTee(bool on) { return onair_usb_rtl_set_bias_tee(h_, on); }
    int resetBuffer() { return onair_usb_rtl_reset_buffer(h_); }
    int readSync(uint8_t* buf, size_t len) { return onair_usb_rtl_read_sync(h_, buf, len); }
    // 1 when the manufacturer / product strings read at open equal these (NUL terminated), else 0
    int checkDongleModel(const char* manufact, const char* product) {
        return onair_usb_rtl_check_dongle_model(h_, reinterpret_cast<const uint8_t*>(manufact), reinterpret_cast<const uint8_t*>(product)); }

private:
    RtlSdrRs(void* h, size_t align) : h_(h), align_(align) {}
    void* h_;
    size_t align_;
};

} // namespace rs
} // namespace dect2
#endif // DECT2_RUST_USB
