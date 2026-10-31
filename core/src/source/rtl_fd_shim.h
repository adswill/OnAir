// The RTL-SDR driver (source_rtlsdr.cpp) drives the radio through a table of librtlsdr entry points (RtlApi). Where the platform hands over an
// opened USB descriptor instead of letting a library scan the bus (Android), the same table is filled with the functions below: the RTL2832U /
// tuner protocol is the Rust layer rust/onair_usb (rs/usb_rtl.hpp) running over a usbfs descriptor (usb_fd_io.h). The driver itself is shared;
// only the table differs. Included by source_rtlsdr.cpp when DECT2_RUST_USB is defined (the Android build turns it on).
#pragma once
#ifdef DECT2_RUST_USB
#include "usb_fd_io.h"
#include "dect2/rs/usb_rtl.hpp"
#include "dect2/source/usb_fd.h"
#include "dect2/source/stream_watchdog.h"
#include "dect2/common/verbose_log.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace dect2 {
namespace native {
namespace rtlfd {

inline UsbFdSys*& sysOverride() { static UsbFdSys* s = nullptr; return s; }   // tests put a fake here
inline UsbFdSys& sys() {
#if defined(__linux__)
    return sysOverride() ? *sysOverride() : usbFdLinuxSys();
#else
    return *sysOverride();   // other platforms: only the tests use this path
#endif
}

struct Dev final : rs::RtlTransport {
    UsbFdRadio info;
    UsbFdDevice usb;
    std::unique_ptr<rs::RtlSdrRs> rtl;
    std::atomic<bool> cancel{false};
    Dev(const UsbFdRadio& r) : info(r), usb(sys(), r.fd) {}
    int controlIn(uint8_t t, uint8_t r, uint16_t v, uint16_t i, uint8_t* d, uint16_t n, uint32_t ms) override { return usb.controlIn(t, r, v, i, d, n, ms); }
    int controlOut(uint8_t t, uint8_t r, uint16_t v, uint16_t i, const uint8_t* d, uint16_t n, uint32_t ms) override { return usb.controlOut(t, r, v, i, d, n, ms); }
    int bulkRead(uint8_t ep, uint8_t* d, int n, uint32_t ms) override { return usb.bulkRead(ep, d, n, ms); }
    int stringDescriptor(int which, uint8_t* out, int cap) override {
        const std::string& s = which == 0 ? info.manufacturer : which == 1 ? info.product : info.serial;
        if (!s.empty()) { const int n = (int)std::min<size_t>(s.size(), (size_t)cap - 1); memcpy(out, s.data(), (size_t)n); out[n] = 0; return n; }
        return usb.stringDescriptor((uint8_t)(which + 1), out, cap);   // device descriptor order is manufacturer, product, serial = 1, 2, 3 only on dongles that follow it
    }
    int resetDevice() override { return usb.resetDevice(); }
    void delayMs(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

inline Dev* D(void* p) { return static_cast<Dev*>(p); }
inline std::vector<UsbFdRadio> radios() { return usbFdList("rtlsdr"); }

// ---- the librtlsdr entry points (same names and meaning, without the rtlsdr_ prefix) ----
inline uint32_t getDeviceCount() { return (uint32_t)radios().size(); }
inline const char* getDeviceName(uint32_t i) {
    thread_local std::string s;
    const auto v = radios();
    if (i >= v.size()) return "";
    s = v[i].product.empty() ? "RTL-SDR" : v[i].product;
    return s.c_str();
}
inline int getUsbStrings(uint32_t i, char* vendor, char* product, char* serial) {
    const auto v = radios();
    if (i >= v.size()) return -1;
    snprintf(vendor, 256, "%s", v[i].manufacturer.c_str());
    snprintf(product, 256, "%s", v[i].product.c_str());
    snprintf(serial, 256, "%s", v[i].serial.c_str());
    return 0;
}
inline int open(void** out, uint32_t i) {
    const auto v = radios();
    if (i >= v.size()) return -1;
    auto d = std::make_unique<Dev>(v[i]);
    if (int r = d->usb.claimInterface(0); r < 0) return r;
    int err = 0;
    d->rtl = rs::RtlSdrRs::open(*d, &err);
    if (!d->rtl) return err < 0 ? err : -1;
    *out = d.release();
    return 0;
}
inline int close(void* p) { delete D(p); return 0; }   // releases the interface; the descriptor stays the app's
inline int setCenterFreq(void* p, uint32_t f) { return D(p)->rtl->setCenterFreq(f); }
inline int setSampleRate(void* p, uint32_t r) { return D(p)->rtl->setSampleRate(r); }
inline uint32_t getSampleRate(void* p) { return D(p)->rtl->sampleRate(); }
inline int setGainMode(void* p, int m) { return D(p)->rtl->setTunerGainMode(m); }
inline int getGains(void* p, int* out) {
    const int n = D(p)->rtl->tunerGains(nullptr, 0);
    if (out && n > 0) D(p)->rtl->tunerGains(reinterpret_cast<int32_t*>(out), (size_t)n);
    return n;
}
inline int setGain(void* p, int g) { return D(p)->rtl->setTunerGain(g); }
inline int setAgc(void* p, int on) { return D(p)->rtl->setAgcMode(on != 0); }
inline int resetBuffer(void* p) { return D(p)->rtl->resetBuffer(); }
inline int getTunerType(void* p) { return D(p)->rtl->tunerType(); }
inline int setDirect(void* p, int m) { return D(p)->rtl->setDirectSampling(m); }
inline int setBias(void* p, int on) { return D(p)->rtl->setBiasTee(on != 0); }
inline int setPpm(void* p, int ppm) { return D(p)->rtl->setFreqCorrection(ppm); }
inline int setOffset(void* p, int on) { return D(p)->rtl->setOffsetTuning(on != 0); }
inline int checkModel(void* p, char* manufact, char* product) { return D(p)->rtl->checkDongleModel(manufact, product) == 1 ? 0 : -1; }   // librtlsdr's convention: 0 = the strings match
// Samples are read on the bulk endpoint straight through the descriptor, not through the protocol object, so a retune on another thread
// (control transfers through that object) does not race with the read.
inline int readSync(void* p, void* buf, int len, int* got) {
    const int r = D(p)->usb.bulkRead(0x81, static_cast<uint8_t*>(buf), len, 1000);
    if (r < 0) { if (got) *got = 0; return r == kUsbTimeout ? 0 : r; }   // a timeout is an empty block: the caller asks again
    if (got) *got = r;
    return 0;
}
constexpr int kStreamUrbs = 64;
constexpr size_t kStreamBlock = 65536;
inline double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Starts the queue of transfers; clearHalt first when the endpoint stopped (EPIPE leaves it halted: every new transfer would fail at once)
inline int startQueue(Dev* d, bool clearHalt, void** st) {
    if (clearHalt) sys().clearHalt(d->info.fd, 0x81);
    return sys().streamStart(d->info.fd, 0x81, kStreamUrbs, UsbFdDevice::kBulkChunk, st);
}

inline int readAsync(void* p, void (*cb)(unsigned char*, uint32_t, void*), void* ctx, uint32_t, uint32_t bufLen) {
    Dev* d = D(p);
    d->cancel = false;
    // Like librtlsdr: many transfers in flight at once (64 x 16 KiB, about 1 MiB, 100 ms at 5 MB/s), the callback fed in blocks of about 64 KiB
    void* st = nullptr;
    if (startQueue(d, false, &st) == 0 && st) {
        std::vector<unsigned char> acc;
        acc.reserve(kStreamBlock + UsbFdDevice::kBulkChunk);
        StreamWatchdog wd;
        wd.start(nowSec());
        int rc = 0;
        double lastReport = nowSec();
        StreamStats rep;
        // the transfers are restarted (3.5): a stalled stream or a run of failed transfers was a frozen spectrum on some phones. verboseLog() prints
        // the counts every 2 s when "Verbose logging" is on
        auto restart = [&](StreamWatchdog::Action act, const char* why) -> bool {
            sys().streamStop(st); st = nullptr; acc.clear();
            if (act == StreamWatchdog::Action::Reopen) return false;
            diagLog("USB stream %s: %s the endpoint and restarting the transfers (restart %llu)", why, act == StreamWatchdog::Action::ResetRestart ? "resetting the device, clearing" : "clearing",
                    (unsigned long long)wd.stats().restarts);
            if (act == StreamWatchdog::Action::ResetRestart) d->usb.resetDevice();
            const int r = startQueue(d, true, &st);
            if (r != 0 || !st) { st = nullptr; diagLog("USB stream: the transfers could not be restarted (%d): the radio is opened again", r); return false; }
            return true;
        };
        while (!d->cancel) {
            uint8_t* data = nullptr; int n = 0;
            const int r = sys().streamRead(st, &data, &n, acc.empty() ? 100 : 5);
            const double t = nowSec();
            StreamWatchdog::Action act = StreamWatchdog::Action::None;
            const char* why = "";
            if (r == 0) {
                wd.onData(t, (uint64_t)n);
                acc.insert(acc.end(), data, data + n);
                sys().streamRecycle(st, data);
            } else if (r == -ETIMEDOUT) {
                act = wd.poll(t); why = "stalled (no samples for 2 s)";
            } else {
                act = wd.onError(t, usbErrnoToLibusb(-r)); why = "failed (broken pipe or transfer error)";
                if (act == StreamWatchdog::Action::None) continue;   // a stray error: the next read goes on
            }
            if (act != StreamWatchdog::Action::None) {
                if (!restart(act, why)) { rc = usbErrnoToLibusb(-r) < 0 && r != -ETIMEDOUT ? usbErrnoToLibusb(-r) : kUsbTimeout; break; }   // the caller closes and reopens the radio
                continue;
            }
            if (!acc.empty() && (acc.size() >= kStreamBlock || r == -ETIMEDOUT) && !d->cancel) {
                cb(acc.data(), (uint32_t)acc.size(), ctx);
                acc.clear();
            }
            if (t - lastReport >= 2.0 && verboseLogging()) {
                const StreamStats& s = wd.stats();
                verboseLog("usb: %.1f MB/s, %llu transfers, %llu errors (last %d), %llu stalls, %llu restarts, %llu resets, %llu reopens",
                           (double)(s.bytes - rep.bytes) / (t - lastReport) / 1e6, (unsigned long long)(s.transfers - rep.transfers), (unsigned long long)s.errors, s.lastError,
                           (unsigned long long)s.stalls, (unsigned long long)s.restarts, (unsigned long long)s.resets, (unsigned long long)s.reopens);
                rep = s; lastReport = t;
            } else if (t - lastReport >= 2.0) { rep = wd.stats(); lastReport = t; }
        }
        if (st) sys().streamStop(st);
        return rc;
    }
    std::vector<unsigned char> buf(bufLen ? bufLen : 262144);
    while (!d->cancel) {
        int got = 0;
        const int r = readSync(p, buf.data(), (int)buf.size(), &got);
        if (r < 0) return r;
        if (got > 0 && !d->cancel) cb(buf.data(), (uint32_t)got, ctx);
    }
    return 0;
}
inline int cancelAsync(void* p) { D(p)->cancel = true; return 0; }

} // namespace rtlfd
} // namespace native
} // namespace dect2
#endif
