// Generic radios through SoapySDR: Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, BladeRF, USRP, ... (whatever drivers are installed).
// The HackRF keeps its own native source (source.cpp); a "hackrf" Soapy entry is hidden so a radio is listed once.
#include "dect2/source.h"
#include "dect2/ring.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dect2 {
namespace {

// Windows: the driver modules are shipped next to the program (<program folder>\lib\SoapySDR\modules0.8), so SoapySDR is told where its root is.
// Always, even when the computer already has SOAPY_SDR_ROOT or SOAPY_SDR_PLUGIN_PATH set by other SDR software (PothosSDR does): their modules
// are built for another SoapySDR and fail to load in OnAir's, and then no radio is found at all.
void soapyLocateModules() {
#ifdef _WIN32
    static const bool once = [] {
        char path[MAX_PATH * 2] = {0};
        if (GetModuleFileNameA(nullptr, path, (DWORD)sizeof path - 1)) {
            std::string dir = path;
            const size_t cut = dir.find_last_of("\\/");
            if (cut != std::string::npos) {
                dir.resize(cut);
                if (const char* old = getenv("SOAPY_SDR_ROOT")) if (dir != old) fprintf(stderr, "SoapySDR: ignoring SOAPY_SDR_ROOT=%s, using the modules in %s\n", old, dir.c_str());
                if (const char* old = getenv("SOAPY_SDR_PLUGIN_PATH")) fprintf(stderr, "SoapySDR: ignoring SOAPY_SDR_PLUGIN_PATH=%s\n", old);
                _putenv_s("SOAPY_SDR_ROOT", dir.c_str());
                _putenv_s("SOAPY_SDR_PLUGIN_PATH", "");   // an empty value removes it
            }
        }
        return true;
    }();
    (void)once;
#endif
}

// The supported rate closest to `want` (continuous ranges are clamped, stepped ranges rounded)
double nearestRate(const std::vector<SoapySDR::Range>& ranges, double want) {
    double best = 0, bestD = 1e30;
    for (const auto& r : ranges) {
        double c = std::min(std::max(want, r.minimum()), r.maximum());
        if (r.step() > 0 && c > r.minimum()) c = r.minimum() + std::round((c - r.minimum()) / r.step()) * r.step();
        if (std::fabs(c - want) < bestD) { bestD = std::fabs(c - want); best = c; }
    }
    return best;
}

class SoapySource : public IqSource {
public:
    explicit SoapySource(std::string args) : args_(std::move(args)) {}
    ~SoapySource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        try {
            dev_ = SoapySDR::Device::make(args_);
            if (!dev_) { err = "cannot open " + args_; return false; }
            if (!apply(s, err, false)) { close(); return false; }
            stream_ = dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
            if (!stream_) { err = "setupStream failed"; close(); return false; }
            if (dev_->activateStream(stream_) != 0) { err = "activateStream failed"; close(); return false; }
        } catch (const std::exception& e) {
            err = std::string("SoapySDR: ") + e.what();
            close();
            return false;
        }
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        run_ = false;
        if (th_.joinable()) th_.join();
        close();
    }

    bool retune(const TuneSettings& s, std::string& err) override {
        if (!dev_) return false;
        std::lock_guard<std::mutex> lk(mu_);   // the reader waits at most one read timeout
        try {
            const bool rateChange = s.sampleRate > 0 && std::fabs(s.sampleRate - requestedRate_) > 1.0;
            if (rateChange) dev_->deactivateStream(stream_);
            const bool ok = apply(s, err, true);
            if (rateChange) dev_->activateStream(stream_);
            return ok;
        } catch (const std::exception& e) {
            err = std::string("SoapySDR: ") + e.what();
            return false;
        }
    }

    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

private:
    void close() {
        try {
            if (dev_ && stream_) { dev_->deactivateStream(stream_); dev_->closeStream(stream_); }
            stream_ = nullptr;
            if (dev_) SoapySDR::Device::unmake(dev_);
        } catch (...) {}
        dev_ = nullptr;
    }

    bool apply(const TuneSettings& s, std::string& err, bool live) {
        const int ch = 0;
        if (!live || std::fabs(s.sampleRate - requestedRate_) > 1.0) {
            const auto ranges = dev_->getSampleRateRange(SOAPY_SDR_RX, ch);
            const double r = ranges.empty() ? s.sampleRate : nearestRate(ranges, s.sampleRate);
            dev_->setSampleRate(SOAPY_SDR_RX, ch, r);
            rate_ = dev_->getSampleRate(SOAPY_SDR_RX, ch);
            requestedRate_ = s.sampleRate;
            if (rate_ < s.sampleRate * 0.97) {
                char b[160];
                snprintf(b, sizeof b, "this radio only offers %.2f Msps, %.2f Msps were requested for this channel width", rate_ / 1e6, s.sampleRate / 1e6);
                err = b;   // reported to the user; the receiver may still work for narrower channels
            }
            // analogue filter: as wide as the signal, within what the radio offers
            try {
                const auto bws = dev_->getBandwidthRange(SOAPY_SDR_RX, ch);
                if (!bws.empty()) {
                    const double want = s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95;
                    dev_->setBandwidth(SOAPY_SDR_RX, ch, nearestRate(bws, want));
                }
            } catch (...) {}
        }
        dev_->setFrequency(SOAPY_SDR_RX, ch, s.centerHz);
        try { if (dev_->hasGainMode(SOAPY_SDR_RX, ch)) dev_->setGainMode(SOAPY_SDR_RX, ch, false); } catch (...) {}   // we run our own AGC
        const auto gr = dev_->getGainRange(SOAPY_SDR_RX, ch);
        dev_->setGain(SOAPY_SDR_RX, ch, std::min(std::max(s.gainDb, gr.minimum()), gr.maximum()));
        return true;
    }

    void loop() {
        std::vector<cf32> buf(1 << 15);
        int failures = 0;
        while (run_) {
            int ret;
            {
                std::lock_guard<std::mutex> lk(mu_);
                void* b[1] = {buf.data()};
                int flags = 0;
                long long ts = 0;
                ret = dev_->readStream(stream_, b, buf.size(), flags, ts, 50000);
            }
            if (ret > 0) { ring_->write(buf.data(), (size_t)ret); failures = 0; }
            else if (ret == SOAPY_SDR_TIMEOUT || ret == SOAPY_SDR_OVERFLOW) { if (ret == SOAPY_SDR_OVERFLOW) overflows_++; }
            else if (++failures > 50) { fprintf(stderr, "SoapySDR: stream error %d, stopping the source\n", ret); break; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::string args_;
    SoapySDR::Device* dev_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    IqRing* ring_ = nullptr;
    std::mutex mu_;
    std::thread th_;
    std::atomic<bool> run_{false};
    double rate_ = 0, requestedRate_ = 0;
    std::atomic<uint64_t> overflows_{0};
};

} // namespace

bool soapySupported() { return true; }

std::vector<DeviceInfo> listSoapyDevices(std::string& err) {
    soapyLocateModules();
    std::vector<DeviceInfo> out;
    try {
        for (const auto& kw : SoapySDR::Device::enumerate()) {
            auto get = [&](const char* k) { auto it = kw.find(k); return it == kw.end() ? std::string() : it->second; };
            const std::string driver = get("driver");
            if (driver == "hackrf") continue;   // native source
            DeviceInfo d;
            d.kind = DeviceInfo::Soapy;
            d.board = driver;
            d.soapyArgs = SoapySDR::KwargsToString(kw);
            std::string label = get("label");
            if (label.empty()) label = driver + (get("serial").empty() ? "" : " " + get("serial"));
            d.name = label;
            d.serial = get("serial");
            try {   // ranges, from a short open
                std::unique_ptr<SoapySDR::Device, void (*)(SoapySDR::Device*)> dev(SoapySDR::Device::make(kw), [](SoapySDR::Device* x) { SoapySDR::Device::unmake(x); });
                if (dev) {
                    const auto rr = dev->getSampleRateRange(SOAPY_SDR_RX, 0);
                    for (const auto& r : rr) { d.maxRateHz = std::max(d.maxRateHz, r.maximum()); d.minRateHz = d.minRateHz == 0 ? r.minimum() : std::min(d.minRateHz, r.minimum()); }
                    const auto gr = dev->getGainRange(SOAPY_SDR_RX, 0);
                    d.gainMinDb = gr.minimum(); d.gainMaxDb = gr.maximum();
                }
            } catch (...) {}
            out.push_back(d);
        }
    } catch (const std::exception& e) {
        err = std::string("SoapySDR: ") + e.what();
    }
    return out;
}

std::unique_ptr<IqSource> makeSoapySource(const DeviceInfo& d) { return std::make_unique<SoapySource>(d.soapyArgs); }

} // namespace dect2
