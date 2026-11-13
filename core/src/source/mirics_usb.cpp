// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir's own driver for the Mirics MSi2500 USB bridge with the MSi001 tuner: the SDRplay RSP1 (and its clones), and the TV sticks built on the
// same pair. It talks to the radio over libusb, the way hackrf_usb.cpp does, so it works on Android from the descriptor the app hands over
// (usb_fd.h) and on Linux, macOS and Windows (WinUSB) without the SDRplay API or libmirisdr. The register maths is in mirics_regs.cpp.
//
// UNTESTED ON HARDWARE (issue #503): written from libmirisdr's register descriptions (GPL-2.0-or-later, THIRD_PARTY.md) without an RSP1 on
// the desk. The radio list and the help say so until a user confirms it works.
//
// The protocol: every setting is a vendor request 0x41 on endpoint 0 that writes one 24-bit register of the bridge (register 9 forwards the
// value to the tuner over its serial bus, register 8 drives the board's band-switch pins). Request 0x43 starts the samples, 0x45 stops them.
// The samples come on endpoint 0x81 in 1024-byte blocks: isochronous on alternate setting 1 (as libmirisdr uses on Linux and macOS) or bulk
// on alternate setting 3 (its Windows choice); the radio setting "USB transfer" picks one.
#include "dect2/source/mirics.h"
#include "dect2/source/source.h"
#include "dect2/source/usb_fd.h"
#include "dect2/common/diag.h"
#include "dect2/common/platform.h"
#include "dect2/common/thread_name.h"
#include "native_common.h"
#include <libusb.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000108
#define DECT2_MIRICS_FD 1
#endif

namespace dect2 {
namespace native {

namespace {

constexpr int kCtrlTimeoutMs = 1000;
constexpr int kIsoPacketBytes = 3 * 1024;   // high-bandwidth isochronous: 3 blocks per 125 us microframe (24.576 MB/s)
constexpr int kIsoPackets = 32;             // 4 ms per transfer
constexpr int kIsoTransfers = 16;           // 64 ms in flight
constexpr int kBulkBytes = 16384;           // 16 blocks; what older usbfs kernels take per request
constexpr int kBulkTransfers = 32;

// One libusb context for the life of the program, made at the first use and never closed: the radio list is read often, and a
// libusb_exit soon after libusb_init can hang on macOS (its event thread may not have started yet).
std::mutex gCtxMu;
libusb_context* gCtx = nullptr;

libusb_context* acquireContext(std::string& err) {
    std::lock_guard<std::mutex> lk(gCtxMu);
    if (!gCtx) {
#ifdef DECT2_MIRICS_FD
        if (usbFdPlatform()) libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);   // Android: no bus scan without root
#endif
        const int r = libusb_init(&gCtx);
        if (r != 0) { err = DIAG_TEXT(770655, "The USB library did not start for the Mirics radio (%s), so it cannot be opened.", libusb_error_name(r)); gCtx = nullptr; return nullptr; }
    }
    return gCtx;
}
void releaseContext() {}

const char* kUntested = " (untested on hardware)";

mirics::Flavour flavourOf(const TuneSettings& s, mirics::Flavour def) {
    const std::string v = radioOption(s, "frontend", def == mirics::Flavour::Rsp1 ? "rsp1" : "generic");
    return v == "generic" ? mirics::Flavour::Generic : mirics::Flavour::Rsp1;
}

std::vector<RadioSetting> miricsSettings(mirics::Flavour def) {
    std::vector<RadioSetting> v{ppmSetting(0.1)};
    v.push_back(choiceSetting("frontend", "Front end",
        "Which board the MSi2500 + MSi001 sit on: it decides the band-switch words the driver sends. An SDRplay RSP1 and its clones take\n"
        "\"SDRplay RSP1\"; the TV sticks take \"Generic\". If signals are weak on one band, try the other.",
        {"rsp1", "generic"}, {"SDRplay RSP1", "Generic MSi2500 stick"}, def == mirics::Flavour::Rsp1 ? "rsp1" : "generic"));
    const std::vector<std::string> st{"auto", "on", "off"}, stn{"Auto (from the gain)", "On", "Off"};
    v.push_back(choiceSetting("lna", "LNA (24 dB)",
        "The tuner's low-noise amplifier (on the AM inputs below 50 MHz: the mix buffer). Auto turns it on from 43 dB of overall gain; Off helps\n"
        "next to a strong station.", st, stn, "auto"));
    v.push_back(choiceSetting("mixer", "Mixer gain (19 dB)", "The tuner's mixer gain. Auto turns it on from 19 dB of overall gain.", st, stn, "auto"));
    std::vector<std::string> fv{"auto"}, fn{"Auto (widest within the sample rate)"};
    for (uint32_t w : mirics::filterWidths()) { fv.push_back(std::to_string(w)); char b[32]; std::snprintf(b, sizeof b, "%g MHz", w / 1e6); fn.push_back(b); }
    v.push_back(choiceSetting("bbfilter", "IF filter", "The MSi001's filter before the converter. Auto takes the widest that is not wider than the sample rate.", fv, fn, "auto"));
    v.push_back(choiceSetting("format", "Sample format",
        "How many bits each sample has on the USB cable. Auto takes the finest one the sample rate fits: 14-bit up to 6.048 Msps, 12-bit up to\n"
        "8.064, 10-bit up to 9.216, 8-bit up to 12.096 Msps. A format too fine for the rate is replaced by Auto.",
        {"auto", "14", "12", "10", "8"}, {"Auto", "14-bit", "12-bit", "10-bit", "8-bit"}, "auto", true));
    v.push_back(choiceSetting("xfer", "USB transfer",
        "Isochronous is what libmirisdr uses on Linux and macOS; bulk is its Windows choice and may help a phone whose USB port drops\n"
        "isochronous data.", {"auto", "iso", "bulk"}, {"Auto", "Isochronous", "Bulk"}, "auto", true));
    return v;
}

bool useBulk(const TuneSettings& s) {
    const std::string x = radioOption(s, "xfer", "auto");
    if (x == "bulk") return true;
    if (x == "iso") return false;
    return kOnWindows;   // WinUSB and isochronous endpoints do not mix well
}

class MiricsSource : public IqSource {
public:
    MiricsSource(std::string where, uint16_t vid, uint16_t pid) : where_(std::move(where)), vid_(vid), pid_(pid) {
        const mirics::UsbModel* m = mirics::usbModel(vid, pid);
        defFlavour_ = m ? m->flavour : mirics::Flavour::Rsp1;
    }
    ~MiricsSource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        stop();
        ctx_ = acquireContext(err);
        if (!ctx_) return false;
        if (!openDevice(err)) { releaseContext(); ctx_ = nullptr; return false; }
        ring_ = &ring;
        // the radio may still be streaming from a program that ended without stopping it
        request(mirics::kReqStopStream);
        write(mirics::adcStopReg());
        for (const auto& w : mirics::initRegs()) write(w);
        if (!program(s, true, err) || !startStream(s, err)) { finish(); return false; }
        return true;
    }

    void stop() override { if (h_ || events_.joinable()) finish(); }

    bool retune(const TuneSettings& s, std::string& err) override {
        if (!h_) return false;
        const double wantRate = std::clamp(s.sampleRate, mirics::kMinRateHz, mirics::kMaxRateHz);
        const bool restream = std::fabs(wantRate - askedRate_) > 0.5 || mirics::formatFor(wantRate, radioOption(s, "format", "auto")) != fmt_ ||
                              useBulk(s) != bulk_;
        if (restream) {   // the clock or the format can only change while the stream is stopped
            stopStream();
            if (!program(s, true, err)) return false;
            return startStream(s, err);
        }
        return program(s, false, err);
    }

    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }
    std::string statusNote() const override {
        const uint64_t l = lost_.load();
        if (!l) return {};
        char b[96];
        std::snprintf(b, sizeof b, "Mirics: %llu samples lost on USB", (unsigned long long)l);
        return b;
    }

private:
    int write(const mirics::Write& w) {
        uint16_t v, i;
        mirics::writeSetup(w, v, i);
        return libusb_control_transfer(h_, mirics::kReqType, mirics::kReqWriteReg, v, i, nullptr, 0, kCtrlTimeoutMs);
    }
    int request(uint8_t req) { return libusb_control_transfer(h_, mirics::kReqType, req, 0, 0, nullptr, 0, kCtrlTimeoutMs); }
    bool writeAll(const std::vector<mirics::Write>& ws, const char* what, std::string& err) {
        for (const auto& w : ws) {
            const int r = write(w);
            if (r < 0) { err = DIAG_TEXT(269342, "The Mirics radio refused the %s registers (%s), so it stops.", what, usbErrorText(r).c_str()); return false; }
        }
        return true;
    }

    bool openDevice(std::string& err) {
        if (usbFdPlatform()) {
#ifdef DECT2_MIRICS_FD
            // where_ = the registry name the app gave the radio
            for (const UsbFdRadio& r : usbFdList("mirics")) {
                if (r.name != where_) continue;
                libusb_device_handle* h = nullptr;
                const int rc = libusb_wrap_sys_device(ctx_, (intptr_t)r.fd, &h);
                if (rc == LIBUSB_ERROR_NOT_SUPPORTED) { err = DIAG_TEXT(651935, "This build of OnAir has no USB library for the Mirics radio (android/docs/USB.md, LIBUSB_SRC)."); return false; }
                if (rc != 0 || !h) { err = "Mirics: " + usbFdErrorText(rc); return false; }
                return claim(h, err);
            }
            err = "The Mirics radio is not connected any more, or Android has not given OnAir access to it: plug it in and tap Allow";
            return false;
#else
            err = DIAG_TEXT(783606, "The USB library of this build is older than 1.0.24, so it cannot open the Mirics radio Android handed over.");
            return false;
#endif
        }
        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) { err = std::string("device list: ") + libusb_error_name((int)n); return false; }
        libusb_device* pick = nullptr;
        for (ssize_t i = 0; i < n && !pick; i++) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != vid_ || d.idProduct != pid_) continue;
            char key[32];
            std::snprintf(key, sizeof key, "usb:%u-%u", (unsigned)libusb_get_bus_number(list[i]), (unsigned)libusb_get_device_address(list[i]));
            if (where_ == key) pick = list[i];
        }
        for (ssize_t i = 0; i < n && !pick; i++) {   // replugged (new address): the first one of the same id
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == vid_ && d.idProduct == pid_) pick = list[i];
        }
        bool ok = false;
        if (!pick) err = DIAG_TEXT(566915, "No Mirics MSi2500 radio was found on USB, so nothing was opened.");
        else {
            libusb_device_handle* h = nullptr;
            const int r = libusb_open(pick, &h);
            if (r != 0) {
                err = kOnWindows ? DIAG_TEXT(582446, "Windows did not let OnAir open the Mirics radio (%s): install the WinUSB driver for it with Zadig, or use the SDRplay API instead.", libusb_error_name(r))
                                 : "Mirics: open: " + usbErrorText(r);
            } else {
#ifdef __linux__
                if (libusb_kernel_driver_active(h, 0) == 1) libusb_detach_kernel_driver(h, 0);   // the kernel's msi2500 module
#endif
                ok = claim(h, err);
            }
        }
        libusb_free_device_list(list, 1);
        return ok;
    }
    bool claim(libusb_device_handle* h, std::string& err) {
        const int r = libusb_claim_interface(h, 0);
        if (r != 0) {
            err = usbFdPlatform() ? "Mirics: " + usbFdErrorText(r)
                                  : DIAG_TEXT(681312, "The Mirics radio is in use by another program or driver (%s): close SDRuno or the SDRplay service and try again.", libusb_error_name(r));
            libusb_close(h);
            return false;
        }
        h_ = h;
        return true;
    }

    // Rate and format (only while the stream is stopped: full), then the tuner and the gain
    bool program(const TuneSettings& s, bool full, std::string& err) {
        const mirics::Flavour fl = flavourOf(s, defFlavour_);
        const double rate = std::clamp(s.sampleRate, mirics::kMinRateHz, mirics::kMaxRateHz);
        if (full) {
            fmt_ = mirics::formatFor(rate, radioOption(s, "format", "auto"));
            const mirics::RateRegs rr = mirics::rateRegs(rate, fmt_);
            if (!writeAll({{7, mirics::formatInfo(fmt_).reg7}, {4, rr.reg4}, {3, rr.reg3}}, "sample rate", err)) return false;
            askedRate_ = rate;
            rate_ = rr.rateHz;
        }
        const std::string bw = radioOption(s, "bbfilter", "auto");
        const uint32_t filt = mirics::filterFor(rate_, bw == "auto" ? 0u : (uint32_t)std::strtoul(bw.c_str(), nullptr, 10));
        const double hz = std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0);   // no frequency correction in the radio itself
        const mirics::TuneRegs t = mirics::tuneRegs(hz, filt, fl);
        if (!writeAll(t.writes, "tuning", err)) return false;
        // the gain registers follow every retune (the band decides what the LNA bit means)
        const mirics::Gain g = mirics::gainFor(s.gainDb, radioOption(s, "lna", "auto"), radioOption(s, "mixer", "auto"));
        return writeAll(mirics::gainRegs(g, t.band), "gain", err);
    }

    bool startStream(const TuneSettings& s, std::string& err) {
        bulk_ = useBulk(s);
        unpack_.reset(fmt_);
        const int alt = libusb_set_interface_alt_setting(h_, 0, bulk_ ? mirics::kAltBulk : mirics::kAltIso);
        if (alt != 0) {
            err = DIAG_TEXT(204693, "The Mirics radio has no %s stream setting (%s): change \"USB transfer\" in its radio settings.", bulk_ ? "bulk" : "isochronous", libusb_error_name(alt));
            return false;
        }
        stopping_ = false; inflight_ = 0; lost_ = 0; gone_ = false;
        const int count = bulk_ ? kBulkTransfers : kIsoTransfers;
        const int bytes = bulk_ ? kBulkBytes : kIsoPackets * kIsoPacketBytes;
        xfers_.assign(count, nullptr);
        bufs_.assign(count, std::vector<uint8_t>(bytes));
        for (int i = 0; i < count; i++) {
            xfers_[i] = libusb_alloc_transfer(bulk_ ? 0 : kIsoPackets);
            if (!xfers_[i]) { err = "Mirics: out of memory for the USB transfers"; stopStream(); return false; }
            if (bulk_) libusb_fill_bulk_transfer(xfers_[i], h_, mirics::kEndpoint, bufs_[i].data(), bytes, &MiricsSource::onTransfer, this, 0);
            else {
                libusb_fill_iso_transfer(xfers_[i], h_, mirics::kEndpoint, bufs_[i].data(), bytes, kIsoPackets, &MiricsSource::onTransfer, this, 0);
                libusb_set_iso_packet_lengths(xfers_[i], kIsoPacketBytes);
            }
            const int r = libusb_submit_transfer(xfers_[i]);
            if (r != 0) { err = "Mirics: submit transfer: " + usbErrorText(r); stopStream(); return false; }
            inflight_++;
        }
        if (request(mirics::kReqStartStream) < 0) { err = "Mirics: the radio would not start sending samples"; stopStream(); return false; }
        events_ = dect2::namedThread("onair-miri-evt", [this] { eventLoop(); });
        return true;
    }

    static void LIBUSB_CALL onTransfer(libusb_transfer* t) {
        auto* self = static_cast<MiricsSource*>(t->user_data);
        if (t->status == LIBUSB_TRANSFER_COMPLETED && !self->stopping_) {
            auto& out = self->tmp_;
            out.clear();
            if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                for (int i = 0; i < t->num_iso_packets; i++) {
                    const libusb_iso_packet_descriptor& pd = t->iso_packet_desc[i];
                    if (pd.status != LIBUSB_TRANSFER_COMPLETED || pd.actual_length == 0) continue;
                    self->unpack_.feed(libusb_get_iso_packet_buffer_simple(t, (unsigned)i), pd.actual_length, out, true);
                }
            } else if (t->actual_length > 0) {
                self->unpack_.feed(t->buffer, (size_t)t->actual_length, out, false);
            }
            if (!out.empty()) self->ring_->write(out.data(), out.size());
            self->lost_ = self->unpack_.lostSamples();
        }
        if (t->status == LIBUSB_TRANSFER_NO_DEVICE) self->gone_ = true;
        if (self->stopping_ || self->gone_ || t->status == LIBUSB_TRANSFER_CANCELLED) { self->inflight_--; return; }
        if (libusb_submit_transfer(t) != 0) { self->inflight_--; self->gone_ = true; }
    }

    void eventLoop() {
        setThreadPriority(ThreadPriority::Realtime);
        timeval tv{0, 100000};
        while (!stopping_ && !gone_ && inflight_ > 0) libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }

    void stopStream() {
        stopping_ = true;
        for (auto* t : xfers_) if (t) libusb_cancel_transfer(t);
        if (events_.joinable()) events_.join();
        timeval tv{0, 50000};
        for (int i = 0; i < 100 && inflight_ > 0 && ctx_; i++) libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
        for (auto*& t : xfers_) if (t) { libusb_free_transfer(t); t = nullptr; }
        xfers_.clear(); bufs_.clear();
        if (h_) request(mirics::kReqStopStream);
    }

    void finish() {
        stopStream();
        if (h_) {
            write(mirics::adcStopReg());
            libusb_release_interface(h_, 0);
            libusb_close(h_);   // a wrapped Android descriptor stays open (the app's)
            h_ = nullptr;
        }
        if (ctx_) { releaseContext(); ctx_ = nullptr; }
    }

    std::string where_;
    uint16_t vid_, pid_;
    mirics::Flavour defFlavour_;
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* h_ = nullptr;
    IqRing* ring_ = nullptr;
    mirics::Format fmt_ = mirics::Format::S14;
    mirics::Unpacker unpack_;
    std::vector<cf32> tmp_;
    double rate_ = 0, askedRate_ = 0;
    bool bulk_ = false;
    std::vector<libusb_transfer*> xfers_;
    std::vector<std::vector<uint8_t>> bufs_;
    std::thread events_;
    std::atomic<bool> stopping_{false}, gone_{false};
    std::atomic<int> inflight_{0};
    std::atomic<uint64_t> lost_{0};
};

DeviceInfo entry(const mirics::UsbModel& m, const std::string& where, const std::string& serial) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "mirics";
    d.serial = serial;
    char ids[16];
    std::snprintf(ids, sizeof ids, "%04x:%04x", m.vid, m.pid);
    d.nativeArgs = where + "#" + ids;
    d.name = std::string(m.name) + (serial.empty() ? "" : " " + serial) + kUntested;
    d.minRateHz = mirics::kMinRateHz; d.maxRateHz = mirics::kMaxRateHz;
    d.rateRanges = {{mirics::kMinRateHz, mirics::kMaxRateHz}};
    if (usbFdPlatform()) d.steadyRateHz = 8.064e6;   // 12-bit samples: what a phone's USB port keeps up with most surely
    d.gainMinDb = 0; d.gainMaxDb = mirics::kMaxGainDb;
    d.gainNote = "LNA 24 dB + mixer 19 dB + IF 0-59 dB (the LNA and mixer switch on as the gain rises: see the radio settings)";
    d.minFreqHz = 10e3; d.maxFreqHz = 2e9;   // the RSP1's specification
    d.settings = miricsSettings(m.flavour);
    return d;
}

} // namespace

void listMirics(std::vector<DeviceInfo>& out) {
    if (usbFdPlatform()) {
        for (const UsbFdRadio& r : usbFdList("mirics")) {
            const mirics::UsbModel* m = mirics::usbModel((uint16_t)r.vid, (uint16_t)r.pid);
            if (m) out.push_back(entry(*m, r.name, r.serial));
        }
        return;
    }
    std::string err;
    libusb_context* ctx = acquireContext(err);
    if (!ctx) return;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        const mirics::UsbModel* m = mirics::usbModel(d.idVendor, d.idProduct);
        if (!m) continue;
        char key[32];
        std::snprintf(key, sizeof key, "usb:%u-%u", (unsigned)libusb_get_bus_number(list[i]), (unsigned)libusb_get_device_address(list[i]));
        out.push_back(entry(*m, key, ""));   // not opened to read a serial: the SDRplay service may hold it, and these chips have none
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    releaseContext();
}

std::unique_ptr<IqSource> makeMirics(const DeviceInfo& d) {
    // nativeArgs = "<where>#vvvv:pppp"
    const size_t h = d.nativeArgs.rfind('#');
    const std::string where = h == std::string::npos ? d.nativeArgs : d.nativeArgs.substr(0, h);
    unsigned vid = 0x1df7, pid = 0x2500;
    if (h != std::string::npos) std::sscanf(d.nativeArgs.c_str() + h + 1, "%x:%x", &vid, &pid);
    return std::make_unique<MiricsSource>(where, (uint16_t)vid, (uint16_t)pid);
}

} // namespace native
} // namespace dect2
