// Generic radios through SoapySDR: Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, BladeRF, USRP, ... (whatever drivers are installed).
// The HackRF keeps its own native source (source.cpp); a "hackrf" Soapy entry is hidden so a radio is listed once.
#include "dect2/source.h"
#include "dect2/ring.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.h>
#include <SoapySDR/Version.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
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

// The supported value closest to `want` (continuous ranges are clamped, stepped ranges rounded)
double nearestRate(const std::vector<SoapySDR::Range>& ranges, double want) {
    double best = 0, bestD = 1e30;
    for (const auto& r : ranges) {
        double c = std::min(std::max(want, r.minimum()), r.maximum());
        if (r.step() > 0 && c > r.minimum()) c = r.minimum() + std::round((c - r.minimum()) / r.step()) * r.step();
        if (std::fabs(c - want) < bestD) { bestD = std::fabs(c - want); best = c; }
    }
    return best;
}

// The sample rate to ask for: the slowest one the radio offers at or above `want` (a slower one cuts the channel), else its fastest.
// Radios that offer a few fixed rates (Airspy: 2.5 and 10 Msps) got the nearest before: 2.5 Msps for a 4 Msps request.
double rateAtLeast(const std::vector<SoapySDR::Range>& ranges, double want) {
    double above = 0, fastest = 0;
    for (const auto& r : ranges) {
        double c = std::min(std::max(want, r.minimum()), r.maximum());
        if (r.step() > 0 && c > r.minimum()) {
            c = r.minimum() + std::ceil((c - r.minimum()) / r.step() - 1e-9) * r.step();
            if (c > r.maximum()) c = r.minimum() + std::floor((r.maximum() - r.minimum()) / r.step() + 1e-9) * r.step();
        }
        fastest = std::max(fastest, c);
        if (c >= want - 1.0 && (above == 0 || c < above)) above = c;
    }
    return above > 0 ? above : fastest;
}

// The ranges of each radio (by serial, or its arguments when it has none), read when it is started: the list is built from the enumerate
// results alone, because opening every radio to ask makes a refresh slow and can take a radio from the program that is using it.
struct SoapyRanges { double minRate = 0, maxRate = 0, gainMin = 0, gainMax = 0, minFreq = 0, maxFreq = 0; std::string biasKey; std::vector<std::string> antennas; std::vector<RadioSetting> settings; };
std::mutex gRangesMu;
std::map<std::string, SoapyRanges> gRanges;

// The driver's own settings (getSettingInfo) become radio settings under "soapy.<key>": direct sampling and offset tuning of SoapyRTLSDR,
// the notch filters of SoapySDRPlay3, ... Their default is the value the radio had when it was opened (readSetting), not the ArgInfo's
// value, which some drivers fill with something else (SoapySDRPlay3 says "true" for notches that are off): OnAir writes a setting only
// when the user picked another value.
const char kSoapyPrefix[] = "soapy.";
std::string soapyBool(const std::string& v) {
    std::string l = v;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l == "true" || l == "1" || l == "on" || l == "yes" ? "1" : "0";
}
std::vector<RadioSetting> soapySettings(SoapySDR::Device* dev, const std::string& biasKey) {
    std::vector<RadioSetting> out;
    SoapySDR::ArgInfoList infos;
    try { infos = dev->getSettingInfo(); } catch (...) { return out; }
    for (const auto& info : infos) {
        if (info.key.empty() || info.key == biasKey) continue;   // the antenna power has its own control
        std::string now;
        try { now = dev->readSetting(info.key); } catch (...) {}
        if (now.empty()) now = info.value;
        RadioSetting r;
        r.key = kSoapyPrefix + info.key;
        r.label = info.name.empty() ? info.key : info.name;
        r.help = info.description.empty() ? "A setting of the radio's SoapySDR driver (" + info.key + ")." : info.description + " (SoapySDR driver setting " + info.key + ")";
        r.unit = info.units;
        if (info.type == SoapySDR::ArgInfo::BOOL) { r.type = RadioSetting::Bool; r.def = soapyBool(now); }
        else if (!info.options.empty()) {
            r.type = RadioSetting::Choice;
            r.values = info.options;
            r.names = info.optionNames.size() == info.options.size() ? info.optionNames : info.options;
            r.def = now;
            if (std::find(r.values.begin(), r.values.end(), r.def) == r.values.end()) r.def = r.values.front();
        } else if ((info.type == SoapySDR::ArgInfo::INT || info.type == SoapySDR::ArgInfo::FLOAT) && info.range.maximum() > info.range.minimum()) {
            r.type = RadioSetting::Number;
            r.minV = info.range.minimum(); r.maxV = info.range.maximum();
            r.step = info.range.step() > 0 ? info.range.step() : info.type == SoapySDR::ArgInfo::INT ? 1 : 0;
            r.def = now.empty() ? "0" : now;
        } else continue;   // free text: not offered
        out.push_back(r);
    }
    return out;
}

// Drivers whose gain elements are all gain reductions (SoapySDRPlay: IFGR 20-59 dB and RFGR, the LNA state, both "more is less"). SoapySDR's
// own overall setGain() fills the elements in order from their minimum up, so on such a radio a higher overall gain is LESS gain and OnAir's
// gain slider and AGC would work backwards: there the overall value is turned around.
bool gainIsReduction(SoapySDR::Device* dev, int ch) {
    try {
        const auto names = dev->listGains(SOAPY_SDR_RX, ch);
        if (names.empty()) return false;
        for (auto n : names) {
            for (auto& c : n) c = (char)toupper((unsigned char)c);
            if (n.size() < 3 || n.compare(n.size() - 2, 2, "GR") != 0) return false;
        }
        return true;
    } catch (...) { return false; }
}

std::string rangesKey(const std::string& serial, const std::string& args) { return serial.empty() ? args : "serial=" + serial; }

// An entry for one antenna input carries its name after the device arguments (listSoapyDevices): "<args>#antenna=<name>"
const char kAntennaTag[] = "#antenna=";
std::string deviceArgs(const std::string& a) { const size_t p = a.rfind(kAntennaTag); return p == std::string::npos ? a : a.substr(0, p); }
std::string antennaOf(const std::string& a) { const size_t p = a.rfind(kAntennaTag); return p == std::string::npos ? std::string() : a.substr(p + sizeof kAntennaTag - 1); }

// The name of the driver's antenna-power setting, if it lists one (the drivers do not agree on a name)
std::string biasSetting(SoapySDR::Device* dev) {
    try {
        for (const auto& info : dev->getSettingInfo()) {
            std::string k = info.key;
            for (auto& c : k) c = (char)tolower((unsigned char)c);
            if (k == "biastee" || k == "bias_tee" || k == "biast_ctrl") return info.key;
        }
    } catch (...) {}
    return {};
}

void rememberRanges(const std::string& key, SoapySDR::Device* dev) {
    SoapyRanges r;
    try {
        for (const auto& x : dev->getSampleRateRange(SOAPY_SDR_RX, 0)) { r.maxRate = std::max(r.maxRate, x.maximum()); r.minRate = r.minRate == 0 ? x.minimum() : std::min(r.minRate, x.minimum()); }
        const auto g = dev->getGainRange(SOAPY_SDR_RX, 0);
        r.gainMin = g.minimum(); r.gainMax = g.maximum();
        for (const auto& x : dev->getFrequencyRange(SOAPY_SDR_RX, 0)) { r.maxFreq = std::max(r.maxFreq, x.maximum()); r.minFreq = r.minFreq == 0 ? x.minimum() : std::min(r.minFreq, x.minimum()); }
    } catch (...) {}
    r.biasKey = biasSetting(dev);
    try { r.antennas = dev->listAntennas(SOAPY_SDR_RX, 0); } catch (...) {}
    r.settings = soapySettings(dev, r.biasKey);
    std::lock_guard<std::mutex> lk(gRangesMu);
    gRanges[key] = r;
}

class SoapySource : public IqSource {
public:
    SoapySource(const std::string& args, std::string key) : args_(deviceArgs(args)), antenna_(antennaOf(args)), key_(std::move(key)) {}
    ~SoapySource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        try {
            dev_ = SoapySDR::Device::make(args_);
            if (!dev_) { err = "cannot open " + args_; return false; }
            rememberRanges(key_, dev_);   // for the next listing
            biasKey_ = biasSetting(dev_);
            bias_ = false;
            reduction_ = gainIsReduction(dev_, 0);
            hasCorr_ = false;
            try { hasCorr_ = dev_->hasFrequencyCorrection(SOAPY_SDR_RX, 0); } catch (...) {}
            ppm_ = 0;
            current_.clear(); boolKeys_.clear();
            for (const auto& r : soapySettings(dev_, biasKey_)) {   // what the radio has now: only other values are written
                current_[r.key] = r.def;
                if (r.type == RadioSetting::Bool) boolKeys_.push_back(r.key);
            }
            if (!antenna_.empty()) dev_->setAntenna(SOAPY_SDR_RX, 0, antenna_);   // the default entry leaves the driver's choice alone
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
        bool stopped = false;
        try {
            const bool rateChange = s.sampleRate > 0 && std::fabs(s.sampleRate - requestedRate_) > 1.0;
            if (rateChange) { dev_->deactivateStream(stream_); stopped = true; }
            const bool ok = apply(s, err, true);
            if (stopped) { stopped = false; dev_->activateStream(stream_); }
            return ok;
        } catch (const std::exception& e) {
            err = std::string("SoapySDR: ") + e.what();
            // a setting the radio refused (a frequency out of its range) must not leave the stream switched off: the receiver would go silent
            if (stopped) try { dev_->activateStream(stream_); } catch (...) {}
            return false;
        }
    }

    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

private:
    void close() {
        try {
            if (dev_ && bias_ && !biasKey_.empty()) dev_->writeSetting(biasKey_, "false");   // antenna power never stays on
        } catch (...) {}
        bias_ = false;
        // each step on its own: a radio that was unplugged throws from deactivateStream(), and the device must still be let go of (unmake),
        // or it stays open and busy until OnAir quits
        if (dev_ && stream_) {
            try { dev_->deactivateStream(stream_); } catch (...) {}
            try { dev_->closeStream(stream_); } catch (...) {}
        }
        stream_ = nullptr;
        try { if (dev_) SoapySDR::Device::unmake(dev_); } catch (...) {}
        dev_ = nullptr;
    }

    bool apply(const TuneSettings& s, std::string& err, bool live) {
        const int ch = 0;
        if (!live || std::fabs(s.sampleRate - requestedRate_) > 1.0) {
            const auto ranges = dev_->getSampleRateRange(SOAPY_SDR_RX, ch);
            const double r = ranges.empty() ? s.sampleRate : rateAtLeast(ranges, s.sampleRate);
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
        // the driver's own settings the user changed (TuneSettings "soapy.<key>"), before the frequency: some move the tuning (direct sampling)
        for (const auto& kv : s.radio) {
            if (kv.first.compare(0, sizeof kSoapyPrefix - 1, kSoapyPrefix) != 0) continue;
            const auto cur = current_.find(kv.first);
            if (cur == current_.end() || cur->second == kv.second) continue;   // not a setting of this driver, or already so
            const bool isBool = std::find(boolKeys_.begin(), boolKeys_.end(), kv.first) != boolKeys_.end();
            const std::string key = kv.first.substr(sizeof kSoapyPrefix - 1);
            try {
                dev_->writeSetting(key, isBool ? (soapyBool(kv.second) == "1" ? "true" : "false") : kv.second);
                cur->second = kv.second;
            } catch (const std::exception& e) {
                fprintf(stderr, "SoapySDR: the setting %s=%s was refused: %s\n", key.c_str(), kv.second.c_str(), e.what());
                fflush(stderr);
            }
        }
        // frequency correction: the driver's own when it has one ("CORR"), else taken out of the frequency asked for
        const double ppm = radioPpm(s);
        double tuneHz = s.centerHz;
        if (hasCorr_) {
            if (ppm != ppm_) {
                try { dev_->setFrequencyCorrection(SOAPY_SDR_RX, ch, ppm); ppm_ = ppm; } catch (...) {}
            }
            if (ppm != ppm_) tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        } else tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        dev_->setFrequency(SOAPY_SDR_RX, ch, tuneHz);
        try { if (dev_->hasGainMode(SOAPY_SDR_RX, ch)) dev_->setGainMode(SOAPY_SDR_RX, ch, false); } catch (...) {}   // we run our own AGC
        const auto gr = dev_->getGainRange(SOAPY_SDR_RX, ch);
        const double g = std::min(std::max(s.gainDb, gr.minimum()), gr.maximum());
        dev_->setGain(SOAPY_SDR_RX, ch, reduction_ ? gr.maximum() - (g - gr.minimum()) : g);   // a gain-reduction driver: turned around
        if (!biasKey_.empty() && s.biasTee != bias_) {
            try { dev_->writeSetting(biasKey_, s.biasTee ? "true" : "false"); bias_ = s.biasTee; } catch (...) {}
        }
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
            else if (ret == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // no samples yet: a count, not an error code
            else if (++failures > 50) { fprintf(stderr, "SoapySDR: stream error %d, stopping the source\n", ret); break; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::string args_, antenna_, key_, biasKey_;   // antenna_: the input the user picked ("" = the driver's default)
    bool bias_ = false;
    bool reduction_ = false;   // the driver's gains are gain reductions (gainIsReduction)
    bool hasCorr_ = false;     // the driver has a frequency correction
    double ppm_ = 0;           // the correction set in the driver
    std::map<std::string, std::string> current_;   // "soapy.<key>" -> the value the radio has
    std::vector<std::string> boolKeys_;            // the ones of them that are BOOL
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
            std::vector<std::string> antennas;
            {   // the ranges are known once the radio has been started (0 = unknown until then); the radio is not opened here
                std::lock_guard<std::mutex> lk(gRangesMu);
                const auto it = gRanges.find(rangesKey(d.serial, d.soapyArgs));
                if (it != gRanges.end()) {
                    const SoapyRanges& r = it->second;
                    d.minRateHz = r.minRate; d.maxRateHz = r.maxRate; d.gainMinDb = r.gainMin; d.gainMaxDb = r.gainMax; d.minFreqHz = r.minFreq; d.maxFreqHz = r.maxFreq;
                    d.hasBiasTee = !r.biasKey.empty();
                    antennas = r.antennas;
                    d.settings = r.settings;
                }
            }
            d.settings.insert(d.settings.begin(), ppmSetting(0.01));   // the driver's own correction, or OnAir's
            out.push_back(d);
            fprintf(stderr, "SoapySDR: found %s (driver %s)\n", d.name.c_str(), driver.c_str());
            // A radio with several antenna inputs: one more entry per input, after the one that leaves the driver's default (what OnAir used
            // before; its name stays the same). The inputs are those listAntennas() gave when the radio was last started, like the ranges
            if (antennas.size() > 1) {
                for (const auto& a : antennas) {
                    DeviceInfo e = d;
                    e.soapyArgs = d.soapyArgs + kAntennaTag + a;
                    e.name = label + " antenna " + a;
                    out.push_back(e);
                }
            }
        }
        fprintf(stderr, "SoapySDR %s: %zu radio(s) besides HackRF\n", SoapySDR::getAPIVersion().c_str(), out.size());
        fflush(stderr);
    } catch (const std::exception& e) {
        err = std::string("SoapySDR: ") + e.what();
    }
    return out;
}

std::unique_ptr<IqSource> makeSoapySource(const DeviceInfo& d) { return std::make_unique<SoapySource>(d.soapyArgs, rangesKey(d.serial, deviceArgs(d.soapyArgs))); }

} // namespace dect2
