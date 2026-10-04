// HackRF driver of our own: talks to the radio over USB (libusb) without libhackrf.
//
// The protocol is small. Everything except the samples goes through vendor control requests on endpoint 0 (tuning, gains, sample rate,
// filter, amplifier, receive on/off); the samples come on bulk endpoint 0x81 as signed 8-bit I/Q pairs. The requests are the ones of the
// open-source HackRF firmware (firmware/common/usb_request.h, host/libhackrf).
//
// Why our own: the sample stream is the part of the receiver where lost data is most expensive, and this lets the reader run at the highest
// priority, use large transfers, and write converted samples straight into the receiver's ring buffer, with one conversion and no other copy.
#include "dect2/source.h"
#include "dect2/platform.h"
#include <libusb.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

namespace {

constexpr uint16_t kVid = 0x1d50;
constexpr uint16_t kPids[] = {0x6089 /* One and Pro */, 0x604b /* Jawbreaker */, 0xcc15 /* rad1o */};

enum Request : uint8_t {
    kSetTransceiverMode = 1,
    kSampleRateSet = 6,
    kBasebandFilterSet = 7,
    kBoardIdRead = 14,
    kVersionStringRead = 15,
    kSetFreq = 16,
    kAmpEnable = 17,
    kSetLnaGain = 19,
    kSetVgaGain = 20,
    kAntennaEnable = 23,
};
constexpr uint8_t kOut = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT;
constexpr uint8_t kIn = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN;
constexpr uint8_t kRxEndpoint = 0x81;
constexpr int kControlTimeoutMs = 1000;
constexpr int kTransferCount = 8;
constexpr int kTransferBytes = 262144;   // 8 x 256 KiB = 0.1 s of samples at 10 Msps in flight

// the filter widths of the radio's baseband filter (MAX2837)
const uint32_t kFilterHz[] = {1750000, 2500000, 3500000, 5000000, 5500000, 6000000, 7000000, 8000000,
                              9000000, 10000000, 12000000, 14000000, 15000000, 20000000, 24000000, 28000000};

// the largest filter width below the wish (the radio's own library rounds the same way)
uint32_t filterBelow(uint32_t wishHz) {
    size_t i = 0;
    while (i < sizeof kFilterHz / sizeof *kFilterHz && kFilterHz[i] < wishHz) i++;
    if (i == sizeof kFilterHz / sizeof *kFilterHz) i--;
    else if (i > 0) i--;
    return kFilterHz[i];
}

// one libusb context for the whole program, created at the first use
std::mutex gCtxMu;
libusb_context* gCtx = nullptr;
int gCtxRefs = 0;

libusb_context* acquireContext(std::string& err) {
    std::lock_guard<std::mutex> lk(gCtxMu);
    if (gCtxRefs == 0) {
        const int r = libusb_init(&gCtx);
        if (r != 0) { err = std::string("libusb_init: ") + libusb_error_name(r); gCtx = nullptr; return nullptr; }
    }
    gCtxRefs++;
    return gCtx;
}
void releaseContext() {
    std::lock_guard<std::mutex> lk(gCtxMu);
    if (--gCtxRefs == 0) { libusb_exit(gCtx); gCtx = nullptr; }
}

std::string boardName(int id) {
    switch (id) {
    case 0: return "HackRF Jellybean";
    case 1: return "HackRF Jawbreaker";
    case 2: return "HackRF One";
    case 3: return "rad1o";
    case 5: return "HackRF Pro";
    default: return "HackRF";
    }
}

bool isHackrf(const libusb_device_descriptor& d) {
    if (d.idVendor != kVid) return false;
    for (uint16_t p : kPids) if (d.idProduct == p) return true;
    return false;
}

struct Handle {
    libusb_device_handle* h = nullptr;
    bool claimed = false;
    bool open(libusb_device* dev, std::string& err) {
        int r = libusb_open(dev, &h);
        if (r != 0) { err = std::string("open: ") + libusb_error_name(r); h = nullptr; return false; }
#ifdef __linux__
        if (libusb_kernel_driver_active(h, 0) == 1) libusb_detach_kernel_driver(h, 0);
#endif
        r = libusb_claim_interface(h, 0);
        if (r != 0) { err = std::string("claim interface: ") + libusb_error_name(r); libusb_close(h); h = nullptr; return false; }
        claimed = true;
        return true;
    }
    void close() {
        if (!h) return;
        if (claimed) libusb_release_interface(h, 0);
        libusb_close(h);
        h = nullptr; claimed = false;
    }
    ~Handle() { close(); }

    int out(uint8_t req, uint16_t value, uint16_t index, const void* data = nullptr, uint16_t len = 0) {
        return libusb_control_transfer(h, kOut, req, value, index, const_cast<unsigned char*>(static_cast<const unsigned char*>(data)), len, kControlTimeoutMs);
    }
    int in(uint8_t req, uint16_t value, uint16_t index, void* data, uint16_t len) {
        return libusb_control_transfer(h, kIn, req, value, index, static_cast<unsigned char*>(data), len, kControlTimeoutMs);
    }
};

std::string serialOf(libusb_device_handle* h, const libusb_device_descriptor& d) {
    unsigned char buf[96] = {0};
    if (d.iSerialNumber && libusb_get_string_descriptor_ascii(h, d.iSerialNumber, buf, sizeof buf - 1) > 0) return reinterpret_cast<char*>(buf);
    return {};
}

void le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

class UsbHackrf : public IqSource {
public:
    explicit UsbHackrf(std::string serial) : serial_(std::move(serial)) {}
    ~UsbHackrf() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        stop();
        ctx_ = acquireContext(err);
        if (!ctx_) return false;
        if (!openDevice(err)) { releaseContext(); ctx_ = nullptr; return false; }
        ring_ = &ring;
        if (!apply(s, err, false)) { dev_.close(); releaseContext(); ctx_ = nullptr; return false; }
        rate_ = s.sampleRate;

        stopping_ = false; inflight_ = 0; lost_ = false;
        xfers_.assign(kTransferCount, nullptr);
        bufs_.assign(kTransferCount, std::vector<uint8_t>(kTransferBytes));
        if (dev_.out(kSetTransceiverMode, 1, 0) < 0) { err = "cannot start receiving"; dev_.close(); releaseContext(); ctx_ = nullptr; return false; }
        for (int i = 0; i < kTransferCount; i++) {
            xfers_[i] = libusb_alloc_transfer(0);
            libusb_fill_bulk_transfer(xfers_[i], dev_.h, kRxEndpoint, bufs_[i].data(), kTransferBytes, &UsbHackrf::onTransfer, this, 0);
            const int r = libusb_submit_transfer(xfers_[i]);
            if (r != 0) {
                err = std::string("submit transfer: ") + libusb_error_name(r);
                finish();
                return false;
            }
            inflight_++;
        }
        events_ = std::thread([this] { eventLoop(); });
        return true;
    }

    void stop() override {
        if (!dev_.h && !events_.joinable()) return;
        finish();
    }

    bool retune(const TuneSettings& s, std::string& err) override { return dev_.h ? apply(s, err, true) : false; }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

private:
    bool openDevice(std::string& err) {
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) { err = std::string("device list: ") + libusb_error_name((int)n); return false; }
        bool found = false;
        std::string lastErr = "no HackRF found";
        for (ssize_t i = 0; i < n && !found; i++) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) != 0 || !isHackrf(d)) continue;
            if (!serial_.empty()) {   // match the whole serial number or its end, as the radio's own tools do
                libusb_device_handle* probe = nullptr;
                if (libusb_open(list[i], &probe) != 0) { lastErr = "open: the radio is in use or has no driver (on Windows: install WinUSB with Zadig)"; continue; }
                const std::string sn = serialOf(probe, d);
                libusb_close(probe);
                if (sn.size() < serial_.size() || sn.compare(sn.size() - serial_.size(), serial_.size(), serial_) != 0) continue;
            }
            std::string e;
            if (dev_.open(list[i], e)) found = true; else lastErr = e;
        }
        libusb_free_device_list(list, 1);
        if (!found) err = lastErr;
        return found;
    }

    bool apply(const TuneSettings& s, std::string& err, bool live) {
        auto fail = [&](const char* what, int r) { err = std::string(what) + ": " + (r < 0 ? libusb_error_name(r) : "the radio refused it"); return false; };
        if (!live || s.sampleRate != rate_) {
            // a sample rate is a frequency and a divider; whole rates need no divider
            double f = s.sampleRate;
            uint32_t div = 1, hz = (uint32_t)std::llround(f);
            for (uint32_t i = 1; i < 32; i++) {
                const double v = f * i;
                if (std::fabs(v - std::round(v)) < 1e-6) { div = i; hz = (uint32_t)std::llround(v); break; }
            }
            uint8_t d[8]; le32(d, hz); le32(d + 4, div);
            int r = dev_.out(kSampleRateSet, 0, 0, d, sizeof d);
            if (r < 0) return fail("set sample rate", r);
            rate_ = s.sampleRate;
        }
        const uint32_t bw = s.basebandFilterHz > 0 ? (uint32_t)s.basebandFilterHz : filterBelow((uint32_t)(s.sampleRate * 0.5));
        const uint32_t bwHz = bw;   // a width given in the settings is used as given
        {
            int r = dev_.out(kBasebandFilterSet, (uint16_t)(bwHz & 0xffff), (uint16_t)(bwHz >> 16));
            if (r < 0) return fail("set baseband filter", r);
        }
        {
            const uint64_t f = (uint64_t)s.centerHz;
            uint8_t d[8]; le32(d, (uint32_t)(f / 1000000)); le32(d + 4, (uint32_t)(f % 1000000));
            int r = dev_.out(kSetFreq, 0, 0, d, sizeof d);
            if (r < 0) return fail("set frequency", r);
        }
        {
            const uint16_t lna = (uint16_t)(std::min(40, std::max(0, (int)s.lnaDb)) & ~7);
            uint8_t ok = 0;
            int r = dev_.in(kSetLnaGain, 0, lna, &ok, 1);
            if (r < 0 || !ok) return fail("set LNA gain", r);
        }
        {
            const uint16_t vga = (uint16_t)(std::min(62, std::max(0, (int)s.vgaDb)) & ~1);
            uint8_t ok = 0;
            int r = dev_.in(kSetVgaGain, 0, vga, &ok, 1);
            if (r < 0 || !ok) return fail("set VGA gain", r);
        }
        {
            int r = dev_.out(kAmpEnable, s.ampOn ? 1 : 0, 0);
            if (r < 0) return fail("set amplifier", r);
        }
        return true;
    }

    // one finished transfer: convert the samples into the ring and hand the buffer back to the USB controller
    static void LIBUSB_CALL onTransfer(libusb_transfer* t) {
        auto* self = static_cast<UsbHackrf*>(t->user_data);
        const bool ok = t->status == LIBUSB_TRANSFER_COMPLETED;
        if (ok && t->actual_length > 0) {
            const int8_t* p = reinterpret_cast<const int8_t*>(t->buffer);
            const size_t n = (size_t)t->actual_length / 2;
            constexpr size_t kChunk = 4096;
            cf32 tmp[kChunk];
            size_t i = 0;
            while (i < n) {
                const size_t m = std::min(kChunk, n - i);
                for (size_t k = 0; k < m; k++) tmp[k] = cf32(p[2 * (i + k)] * (1.0f / 128), p[2 * (i + k) + 1] * (1.0f / 128));
                self->ring_->write(tmp, m);
                i += m;
            }
        }
        if (t->status == LIBUSB_TRANSFER_NO_DEVICE) self->lost_ = true;
        if (self->stopping_ || self->lost_ || t->status == LIBUSB_TRANSFER_CANCELLED) { self->inflight_--; return; }
        if (!ok) self->errors_++;
        if (libusb_submit_transfer(t) != 0) { self->inflight_--; self->lost_ = true; }
    }

    void eventLoop() {
        setThreadPriority(ThreadPriority::Realtime);   // the radio's buffers are only 0.1 s deep: this thread must not wait for anything else
        timeval tv{0, 100000};
        while (!stopping_ && !lost_ && inflight_ > 0) libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }

    void finish() {
        stopping_ = true;
        if (dev_.h) dev_.out(kSetTransceiverMode, 0, 0);   // stop sending samples
        for (auto* t : xfers_) if (t) libusb_cancel_transfer(t);   // fails harmlessly for transfers that already ended
        if (events_.joinable()) events_.join();
        // transfers that were cancelled still need their completion events
        timeval tv{0, 50000};
        for (int i = 0; i < 100 && inflight_ > 0 && ctx_; i++) libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
        for (auto*& t : xfers_) if (t) { libusb_free_transfer(t); t = nullptr; }
        xfers_.clear(); bufs_.clear();
        dev_.close();
        if (ctx_) { releaseContext(); ctx_ = nullptr; }
    }

    std::string serial_;
    libusb_context* ctx_ = nullptr;
    Handle dev_;
    IqRing* ring_ = nullptr;
    double rate_ = 0;
    std::vector<libusb_transfer*> xfers_;
    std::vector<std::vector<uint8_t>> bufs_;
    std::thread events_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> lost_{false};
    std::atomic<int> inflight_{0};
    std::atomic<uint64_t> errors_{0};
};

} // namespace

std::unique_ptr<IqSource> makeUsbHackrfSource(const std::string& serial) { return std::make_unique<UsbHackrf>(serial); }

std::vector<DeviceInfo> listUsbHackrfDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    libusb_context* ctx = acquireContext(err);
    if (!ctx) return out;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0 || !isHackrf(d)) continue;
        DeviceInfo info;
        info.kind = DeviceInfo::HackRF;
        info.board = "HackRF";
        libusb_device_handle* h = nullptr;
        if (libusb_open(list[i], &h) == 0) {
            info.serial = serialOf(h, d);
            uint8_t id = 0xff;
            if (libusb_control_transfer(h, kIn, kBoardIdRead, 0, 0, &id, 1, kControlTimeoutMs) == 1) info.board = boardName(id);
            libusb_close(h);
        } else {
            info.board = "HackRF (busy or without driver)";
        }
        const std::string tail = info.serial.size() > 8 ? info.serial.substr(info.serial.size() - 8) : info.serial;
        info.name = info.board + (tail.empty() ? std::string() : " (" + tail + ")");
        out.push_back(info);
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    releaseContext();
    return out;
}

} // namespace dect2
