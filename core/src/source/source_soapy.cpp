// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Generic radios through SoapySDR: Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, BladeRF, USRP, ... (whatever drivers are installed).
// The HackRF keeps its own native source (source.cpp); a "hackrf" Soapy entry is hidden so a radio is listed once.
#include "dect2/tx/tx_control.h"
#include "dect2/common/thread_name.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/source/source.h"
#include "dect2/source/hw_time.h"
#include "soapy_logic.h"
#include "dect2/net/netradio.h"
#include "dect2/common/ring.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <set>
#include <SoapySDR/Types.h>
#include <SoapySDR/Version.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <stdexcept>
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
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
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
#elif defined(__APPLE__)
    // macOS app bundle: the SoapyRemote client module is bundled in Contents/Frameworks/SoapySDR/modules (tools/package/bundle_macos.py; not "modules0.8": codesign reads that name as a bundle). Homebrew's own
    // module folder is still searched by SoapySDR, so a development build without the bundle keeps working.
    static const bool once = [] {
        char path[4096] = {0};
        uint32_t n = sizeof path;
        if (_NSGetExecutablePath(path, &n) == 0) {
            std::string dir = path;
            const size_t cut = dir.find_last_of('/');
            if (cut != std::string::npos) {
                dir.resize(cut);   // .../OnAir.app/Contents/MacOS
                const std::string mods = dir + "/../Frameworks/SoapySDR/modules";
                if (access(mods.c_str(), R_OK) == 0) {
                    const char* old = getenv("SOAPY_SDR_PLUGIN_PATH");
                    const std::string v = old && *old ? mods + ":" + old : mods;
                    setenv("SOAPY_SDR_PLUGIN_PATH", v.c_str(), 1);
                }
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
struct SoapyRanges { std::vector<std::pair<double, double>> rates; double minRate = 0, maxRate = 0, gainMin = 0, gainMax = 0, minFreq = 0, maxFreq = 0; std::string biasKey; std::vector<std::string> antennas; std::vector<RadioSetting> settings;
    bool txKnown = false, canTx = false; int nTx = 0; double txMinRate = 0, txMaxRate = 0, txGainMin = 0, txGainMax = 0; };   // transmit: known once the radio has been opened
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
// The clock and time (PPS) sources the driver lists (setClockSource / setTimeSource), offered only when there is a choice. The default is what the
// radio has now. Applied when the radio is started (the setting restarts it): switching the reference under a running stream would glitch it.
const char kClockKey[] = "clocksrc", kTimeKey[] = "timesrc";
RadioSetting sourceSetting(const char* key, const char* label, const char* help, const std::vector<std::string>& list, const std::string& now) {
    std::vector<std::string> names;
    for (const auto& n : list) names.push_back(n == "gpsdo" ? "GPSDO" : n == "mimo" ? "MIMO cable" : n == "external" ? "External" : n == "internal" ? "Internal" : n);
    return choiceSetting(key, label, help, list, names, std::find(list.begin(), list.end(), now) != list.end() ? now : list.front(), true);
}
void sourceSettings(SoapySDR::Device* dev, std::vector<RadioSetting>& out) {
    try {
        const auto l = dev->listClockSources();
        if (l.size() > 1) out.push_back(sourceSetting(kClockKey, "Reference clock", "Where the radio takes its reference clock from (internal oscillator, an external 10 MHz input, a GPSDO ...), as the SoapySDR driver lists it.\nTakes effect when the radio is started.", l, dev->getClockSource()));
    } catch (...) {}
    try {
        const auto l = dev->listTimeSources();
        if (l.size() > 1) out.push_back(sourceSetting(kTimeKey, "Time source (PPS)", "Where the radio takes its one-pulse-per-second time reference from, as the SoapySDR driver lists it.\nTakes effect when the radio is started.", l, dev->getTimeSource()));
    } catch (...) {}
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

// The overall gain range for the slider: SoapySDRPlay can report a degenerate overall range (a span of a dB or so, the slider stuck at 1 dB)
// although its elements (IFGR, RFGR) span tens of dB: then the range is the sum of the elements' spans, from the first element's minimum.
SoapySDR::Range effectiveGainRange(SoapySDR::Device* dev, int ch) {
    SoapySDR::Range g = dev->getGainRange(SOAPY_SDR_RX, ch);
    try {
        if (g.maximum() - g.minimum() < 10 && gainIsReduction(dev, ch)) {
            double span = 0;
            for (const auto& n : dev->listGains(SOAPY_SDR_RX, ch)) { const auto r = dev->getGainRange(SOAPY_SDR_RX, ch, n); span += r.maximum() - r.minimum(); }
            if (span > g.maximum() - g.minimum()) return SoapySDR::Range(g.minimum(), g.minimum() + span);
        }
    } catch (...) {}
    return g;
}

// The RF gain stage of a gain-reduction driver (SoapySDRPlay3: the gain element "RFGR", the LNA state 0-9, 0 = most gain): offered as a
// setting of its own ("rfgr": auto = the overall gain slider drives it as before, or a state), so the ADC can be kept from saturating
// with the IF gain (IFGR) left to the slider. Not offered when the driver has no such element.
const char kRfKey[] = "rfgr";
bool hasGainElement(SoapySDR::Device* dev, const char* want) {
    try { for (const auto& n : dev->listGains(SOAPY_SDR_RX, 0)) if (n == want) return true; } catch (...) {}
    return false;
}
bool rfStage(SoapySDR::Device* dev) { return gainIsReduction(dev, 0) && hasGainElement(dev, "RFGR") && hasGainElement(dev, "IFGR"); }

// Soapy's overall setGain() fills gain-reduction elements from their minimum (RFGR 0 = most RF gain first), which leaves LNA at maximum
// until IFGR is exhausted and often overloads the ADC. Spread reduction across IFGR then RFGR like the native SDRplay source.
static void soapyRfIfFromOverall(double g, const SoapySDR::Range& gr, const SoapySDR::Range& ir, const SoapySDR::Range& rr, double& rf, double& ifg) {
    const double t = gr.maximum() > gr.minimum() ? std::min(1.0, std::max(0.0, (g - gr.minimum()) / (gr.maximum() - gr.minimum()))) : 0;
    ifg = ir.maximum() - t * (ir.maximum() - ir.minimum());
    rf = rr.maximum() - t * (rr.maximum() - rr.minimum());
    ifg = std::min(std::max(ifg, ir.minimum()), ir.maximum());
    rf = std::min(std::max(std::lround(rf), (long)std::lround(rr.minimum())), (long)std::lround(rr.maximum()));
    ifg = std::lround(ifg);
}
RadioSetting rfSetting(SoapySDR::Device* dev) {
    RadioSetting r;
    r.type = RadioSetting::Choice; r.key = kRfKey; r.label = "RF gain (LNA state)";
    r.help = "The radio's RF gain stage. 0 is the MOST gain, higher states reduce it: raise it when the ADC saturates on a strong signal. Auto leaves it to the gain slider, which then sets both stages; any state makes the slider drive the IF gain only.";
    r.values = {"auto"}; r.names = {"Auto (with the gain slider)"};
    int lo = 0, hi = 9;
    try { const auto g = dev->getGainRange(SOAPY_SDR_RX, 0, "RFGR"); lo = (int)std::lround(g.minimum()); hi = (int)std::lround(g.maximum()); } catch (...) {}
    for (int i = lo; i <= hi; i++) { r.values.push_back(std::to_string(i)); r.names.push_back(std::to_string(i) + (i == lo ? " (maximum gain)" : i == hi ? " (least gain)" : "")); }
    r.def = "auto";
    return r;
}

// The driver's own gain elements (LNA, VGA, TIA, PGA, ...) as one setting each ("gain.<element>"), offered when the radio has several. Left alone
// (not in TuneSettings::radio) the overall gain slider drives them as before; once one is set by hand it is applied after the slider's gain,
// so the slider and the AGC still move the others. Not for the RFGR / IFGR pair, which has its own setting above.
const char kGainPrefix[] = "gain.";
std::vector<RadioSetting> gainStageSettings(SoapySDR::Device* dev) {
    std::vector<RadioSetting> out;
    try {
        const auto names = dev->listGains(SOAPY_SDR_RX, 0);
        if (names.size() < 2) return out;
        const bool skip = rfStage(dev);
        for (const auto& n : names) {
            if (skip && (n == "RFGR" || n == "IFGR")) continue;
            const auto r = dev->getGainRange(SOAPY_SDR_RX, 0, n);
            if (!(r.maximum() > r.minimum())) continue;
            RadioSetting x;
            x.type = RadioSetting::Number; x.key = kGainPrefix + n;
            x.label = soapylogic::stageName(n).label;
            x.help = "One gain element of the radio's driver (" + n + "), in dB. Set by hand it overrides what the gain slider gives this stage; the other stages still follow the slider. Takes effect on the next gain change or retune.";
            x.minV = r.minimum(); x.maxV = r.maximum(); x.step = r.step() > 0 ? r.step() : 0; x.unit = soapylogic::stageName(n).unit;
            double now = r.minimum();
            try { now = dev->getGain(SOAPY_SDR_RX, 0, n); } catch (...) {}
            char b[32]; snprintf(b, sizeof b, "%g", now);
            x.def = b;
            out.push_back(x);
        }
    } catch (...) {}
    return out;
}

std::string rangesKey(const std::string& serial, const std::string& args) { return serial.empty() ? args : "serial=" + serial; }

// An entry for one antenna input carries its name after the device arguments (listSoapyDevices): "<args>#antenna=<name>"
const char kAntennaTag[] = "#antenna=";
std::string deviceArgs(const std::string& a) { const size_t p = a.rfind(kAntennaTag); return p == std::string::npos ? a : a.substr(0, p); }

// Device::make on a network radio (SoapyRemote) can block for good (no server, no route, a local driver service that is not running): the open
// runs on a helper thread and is given up after 20 s, so Start never hangs the app. A late result is unmade by the helper itself.
SoapySDR::Device* makeDevice(const std::string& args, std::string& err) {
    struct Box { std::mutex m; std::condition_variable cv; bool done = false, abandoned = false; SoapySDR::Device* dev = nullptr; std::string what; };
    auto box = std::make_shared<Box>();
    std::thread([box, args] {
        SoapySDR::Device* d = nullptr; std::string w;
        try { d = SoapySDR::Device::make(args); } catch (const std::exception& e) { w = e.what(); } catch (...) { w = "unknown error"; }
        std::lock_guard<std::mutex> lk(box->m);
        if (box->abandoned) { try { if (d) SoapySDR::Device::unmake(d); } catch (...) {} return; }
        box->dev = d; box->what = w; box->done = true; box->cv.notify_all();
    }).detach();
    std::unique_lock<std::mutex> lk(box->m);
    if (!box->cv.wait_for(lk, std::chrono::seconds(20), [&] { return box->done; })) {
        box->abandoned = true;
        err = "the radio did not answer within 20 s (" + args + "). Check the address, that the server is running, and the radio's driver on it";
        return nullptr;
    }
    if (!box->what.empty()) throw std::runtime_error(box->what);
    return box->dev;
}
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
        for (const auto& x : dev->getSampleRateRange(SOAPY_SDR_RX, 0)) { if (x.step() <= 0 || x.maximum() <= x.minimum()) r.rates.emplace_back(x.minimum(), x.maximum()); r.maxRate = std::max(r.maxRate, x.maximum()); r.minRate = r.minRate == 0 ? x.minimum() : std::min(r.minRate, x.minimum()); }
        const auto g = effectiveGainRange(dev, 0);
        r.gainMin = g.minimum(); r.gainMax = g.maximum();
        for (const auto& x : dev->getFrequencyRange(SOAPY_SDR_RX, 0)) { r.maxFreq = std::max(r.maxFreq, x.maximum()); r.minFreq = r.minFreq == 0 ? x.minimum() : std::min(r.minFreq, x.minimum()); }
    } catch (...) {}
    try {   // the transmit side (a radio with a TX channel), so that the list can offer Transmit for it
        r.txKnown = true;
        r.nTx = (int)dev->getNumChannels(SOAPY_SDR_TX);
        r.canTx = r.nTx > 0;
        if (r.canTx) {
            for (const auto& x : dev->getSampleRateRange(SOAPY_SDR_TX, 0)) { r.txMaxRate = std::max(r.txMaxRate, x.maximum()); r.txMinRate = r.txMinRate == 0 ? x.minimum() : std::min(r.txMinRate, x.minimum()); }
            const auto g = dev->getGainRange(SOAPY_SDR_TX, 0);
            r.txGainMin = g.minimum(); r.txGainMax = g.maximum();
        }
    } catch (...) {}
    r.biasKey = biasSetting(dev);
    try { r.antennas = dev->listAntennas(SOAPY_SDR_RX, 0); } catch (...) {}
    r.settings = soapySettings(dev, r.biasKey);
    if (rfStage(dev)) r.settings.insert(r.settings.begin(), rfSetting(dev));
    sourceSettings(dev, r.settings);
    for (auto& x : gainStageSettings(dev)) r.settings.push_back(std::move(x));
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
            dev_ = makeDevice(args_, err);
            if (!dev_ && !err.empty()) return false;
            if (!dev_) { err = "cannot open " + args_; const std::string h = soapyRemoteFailureHelp(args_); if (!h.empty()) err = h; return false; }
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
            stream_ = dev_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0});
            if (!stream_) { err = "setupStream failed"; close(); return false; }
            armHardwareTime();
            if (dev_->activateStream(stream_) != 0) { err = "activateStream failed"; close(); return false; }
        } catch (const std::exception& e) {
            err = std::string("SoapySDR: ") + e.what();
            const std::string h = soapyRemoteFailureHelp(args_);
            if (!h.empty()) err = h + " (" + e.what() + ")";
            else { const std::string b = soapylogic::busyHint(e.what()); if (!b.empty()) err = b + " (" + e.what() + ")"; }
            close();
            return false;
        }
        run_ = true;
        th_ = dect2::namedThread("onair-soapy", [this] { loop(); });
        return true;
    }

    void stop() override {
        run_ = false;
        if (th_.joinable()) th_.join();
        if (ppsTime_) { hwtime::board().clear(ring_); ppsTime_ = false; }
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
    std::string statusNote() const override { return soapylogic::streamStatus(overflows_, 0, 0); }

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

    double lastTuneHz_ = -1;
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        const int ch = 0;
        if (!live || std::fabs(s.sampleRate - requestedRate_) > 1.0) {
            const auto ranges = dev_->getSampleRateRange(SOAPY_SDR_RX, ch);
            std::vector<soapylogic::Span> table;
            for (const auto& x : ranges) table.push_back({x.minimum(), x.maximum(), x.step()});
            const auto pick = soapylogic::chooseRate(table, s.sampleRate);
            dev_->setSampleRate(SOAPY_SDR_RX, ch, ranges.empty() ? s.sampleRate : pick.hz);
            rate_ = dev_->getSampleRate(SOAPY_SDR_RX, ch);
            requestedRate_ = s.sampleRate;
            if (!pick.note.empty() && rate_ < s.sampleRate * 0.97) err = pick.note;   // reported to the user; the receiver may still work for narrower channels
            // analogue filter: as wide as the signal, within what the radio offers
            try {
                const auto bws = dev_->getBandwidthRange(SOAPY_SDR_RX, ch);
                if (!bws.empty()) {
                    const double want = s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95;
                    dev_->setBandwidth(SOAPY_SDR_RX, ch, nearestRate(bws, want));
                }
            } catch (...) {}
        }
        if (!live) {   // the clock and time sources the user picked; the radio's own choice stays when none was
            std::vector<RadioSetting> src;
            sourceSettings(dev_, src);
            for (const auto& r : src) {
                const std::string want = radioOption(s, r.key, r.def);
                if (want == r.def) continue;
                try {
                    if (r.key == kClockKey) dev_->setClockSource(want); else dev_->setTimeSource(want);
                } catch (const std::exception& e) {
                    err = std::string("SoapySDR: the ") + (r.key == kClockKey ? "reference clock" : "time source") + " \"" + want + "\" was refused: " + e.what();
                    return false;
                }
            }
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
        // as asked, whatever the driver lists: outside its range the frequency is still passed on (the driver throws if it cannot), with a note
        try {
            double lo = 0, hi = 0;
            for (const auto& x : dev_->getFrequencyRange(SOAPY_SDR_RX, ch)) { hi = std::max(hi, x.maximum()); lo = lo == 0 ? x.minimum() : std::min(lo, x.minimum()); }
            addNote(err, freqSpecNote("radio", s.centerHz, lo, hi));
        } catch (...) {}
        if (!live || tuneHz != lastTuneHz_) { dev_->setFrequency(SOAPY_SDR_RX, ch, std::max(tuneHz, 0.0)); lastTuneHz_ = tuneHz; }   // (a gain-only retune leaves the LO alone)
        try { if (dev_->hasGainMode(SOAPY_SDR_RX, ch)) dev_->setGainMode(SOAPY_SDR_RX, ch, false); } catch (...) {}   // we run our own AGC
        const auto gr = effectiveGainRange(dev_, ch);
        const double g = std::min(std::max(s.gainDb, gr.minimum()), gr.maximum());
        const std::string rfv = radioOption(s, kRfKey, "auto");
        if (reduction_ && rfv != "auto" && !rfv.empty() && rfStage(dev_)) {
            // the RF stage picked by hand: the slider drives the IF gain alone (its top is the least reduction)
            const auto rr = dev_->getGainRange(SOAPY_SDR_RX, ch, "RFGR"), ir = dev_->getGainRange(SOAPY_SDR_RX, ch, "IFGR");
            const double rf = std::min(std::max(atof(rfv.c_str()), rr.minimum()), rr.maximum());
            const double ifg = std::min(std::max(ir.maximum() - (g - gr.minimum()), ir.minimum()), ir.maximum());
            dev_->setGain(SOAPY_SDR_RX, ch, "RFGR", rf);
            dev_->setGain(SOAPY_SDR_RX, ch, "IFGR", ifg);
        } else if (reduction_ && rfStage(dev_)) {
            const auto rr = dev_->getGainRange(SOAPY_SDR_RX, ch, "RFGR"), ir = dev_->getGainRange(SOAPY_SDR_RX, ch, "IFGR");
            double rf, ifg;
            soapyRfIfFromOverall(g, gr, ir, rr, rf, ifg);
            dev_->setGain(SOAPY_SDR_RX, ch, "RFGR", rf);
            dev_->setGain(SOAPY_SDR_RX, ch, "IFGR", ifg);
        } else
            dev_->setGain(SOAPY_SDR_RX, ch, reduction_ ? gr.maximum() - (g - gr.minimum()) : g);   // a gain-reduction driver: turned around
        for (const auto& kv : s.radio) {   // gain stages set by hand (gainStageSettings), after the overall gain
            if (kv.first.compare(0, sizeof kGainPrefix - 1, kGainPrefix) != 0 || kv.second.empty()) continue;
            const std::string el = kv.first.substr(sizeof kGainPrefix - 1);
            try {
                const auto r = dev_->getGainRange(SOAPY_SDR_RX, ch, el);
                dev_->setGain(SOAPY_SDR_RX, ch, el, std::min(std::max(atof(kv.second.c_str()), r.minimum()), r.maximum()));
            } catch (...) {}   // not an element of this radio (the setting of another one)
        }
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
            int flags = 0;
            long long ts = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                void* b[1] = {buf.data()};
                ret = dev_->readStream(stream_, b, buf.size(), flags, ts, 50000);
            }
            if (ret > 0) {
                if (ppsTime_ && (flags & SOAPY_SDR_HAS_TIME) && std::chrono::steady_clock::now() >= ppsFrom_) {
                    const int64_t sys = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    hwtime::board().publish(ring_, written_, hwtime::ppsTimeToUtcNs(ts, sys), rate_);
                }
                written_ += (uint64_t)ret;
                ring_->write(buf.data(), (size_t)ret); failures = 0;
            }
            else if (ret == SOAPY_SDR_TIMEOUT || ret == SOAPY_SDR_OVERFLOW) { if (ret == SOAPY_SDR_OVERFLOW) overflows_++; }
            else if (ret == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // no samples yet: a count, not an error code
            else if (++failures > 50) { fprintf(stderr, "SoapySDR: stream error %d, stopping the source\n", ret); break; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Hardware time, only with a PPS-type time source (GPSDO, external): the time register is zeroed at the next PPS edge; marks start once that has passed
    void armHardwareTime() {
        ppsTime_ = false;
        written_ = 0;
        try {
            std::string t = dev_->getTimeSource();
            for (char& c : t) c = (char)std::tolower((unsigned char)c);
            if (t.find("gps") == std::string::npos && t.find("ext") == std::string::npos && t.find("pps") == std::string::npos) return;
            if (!dev_->hasHardwareTime()) return;
            dev_->setHardwareTime(0, "PPS");
            ppsTime_ = true;
            ppsFrom_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
            hwtime::board().arm(ring_);
        } catch (...) { ppsTime_ = false; }   // a driver without it: this radio just has no hardware time
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
    bool ppsTime_ = false;
    uint64_t written_ = 0;
    std::chrono::steady_clock::time_point ppsFrom_;
};


// ---- transmit -----------------------------------------------------------------------------------------------------------------------
// A SoapySDR radio that has a transmit channel (LimeSDR, BladeRF, USRP, PlutoSDR, ... through their modules): setupStream(TX, CF32) and
// writeStream(). OnAir applies no limit of its own to the frequency or the gain: the frequency goes to the driver as given and a driver that
// refuses it (it throws) is quoted; the gain is clamped to the range the radio itself reports. NOT tested on a radio: the tests use a fake SoapySDR
// driver that records what writeStream receives.
class SoapyTx : public TxSink {
public:
    SoapyTx(const std::string& args, std::string key) : args_(deviceArgs(args)), key_(std::move(key)) {}
    ~SoapyTx() override { close(); }

    bool open(const TxTune& t, std::string& err) override {
        close();
        try {
            dev_ = makeDevice(args_, err);
            if (!dev_ && !err.empty()) return false;
            if (!dev_) { err = "cannot open " + args_; const std::string h = soapyRemoteFailureHelp(args_); if (!h.empty()) err = h; return false; }
            rememberRanges(key_, dev_);
            const int nTx = (int)dev_->getNumChannels(SOAPY_SDR_TX);
            if (nTx <= 0) { err = "this radio has no transmit channel"; shut(); return false; }
            ch_ = t.channel < 0 ? 0 : t.channel;
            if (ch_ >= nTx) {
                err = nTx == 1 ? "this radio has one transmit output (TX1)"
                               : "this radio has " + std::to_string(nTx) + " transmit outputs (TX1 to TX" + std::to_string(nTx) + ")";
                shut();
                return false;
            }
            const auto ranges = dev_->getSampleRateRange(SOAPY_SDR_TX, (size_t)ch_);
            const double want = ranges.empty() ? t.sampleRate : rateAtLeast(ranges, t.sampleRate);
            dev_->setSampleRate(SOAPY_SDR_TX, (size_t)ch_, want);
            rate_ = dev_->getSampleRate(SOAPY_SDR_TX, (size_t)ch_);
            try {
                const auto bws = dev_->getBandwidthRange(SOAPY_SDR_TX, (size_t)ch_);
                if (!bws.empty()) dev_->setBandwidth(SOAPY_SDR_TX, (size_t)ch_, nearestRate(bws, plan(t).filterHz));
            } catch (...) {}
            if (!tuneAndGain(t, err)) { shut(); return false; }
            stream_ = dev_->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {(size_t)ch_});
            if (!stream_) { err = "setupStream (transmit) failed"; shut(); return false; }
            if (dev_->activateStream(stream_) != 0) { err = "activateStream (transmit) failed"; shut(); return false; }
        } catch (const std::exception& e) {
            err = std::string("SoapySDR: ") + e.what();
            shut();
            return false;
        }
        return true;
    }

    bool write(const cf32* x, size_t n, std::string& err) override {
        if (!dev_ || !stream_) { err = "the radio is not open for transmitting"; return false; }
        size_t done = 0;
        int idle = 0;
        while (done < n) {
            const void* b[1] = {x + done};
            int flags = 0;
            int ret;
            try { ret = dev_->writeStream(stream_, b, n - done, flags, 0, 100000); }
            catch (const std::exception& e) { err = std::string("SoapySDR: ") + e.what(); return false; }
            if (ret > 0) { done += (size_t)ret; idle = 0; }
            else if (ret == SOAPY_SDR_TIMEOUT || ret == 0) { if (++idle > 50) { err = "the radio stopped taking samples"; return false; } }
            else if (ret == SOAPY_SDR_UNDERFLOW) { underflows_++; }
            else if (ret == SOAPY_SDR_NOT_SUPPORTED || ret == SOAPY_SDR_STREAM_ERROR || ret == SOAPY_SDR_CORRUPTION) { err = std::string("the radio's driver refused the samples: ") + SoapySDR_errToStr(ret); return false; }
            else { err = std::string("sending samples failed: ") + SoapySDR_errToStr(ret); return false; }
        }
        return true;
    }

    bool retune(const TxTune& t, std::string& err) override {
        if (!dev_) return false;
        try { return tuneAndGain(t, err); } catch (const std::exception& e) { err = std::string("SoapySDR: ") + e.what(); return false; }
    }

    void close() override { shut(); }
    double sampleRate() const override { return rate_; }
    std::string name() const override { return "SoapySDR radio"; }

private:
    // the frequency, the overall gain and the typed gain stages as soapyTxPlan (tx_control.h) works them out with the ranges the driver reports
    TxHwPlan plan(const TxTune& t) {
        TxHwRange overall;
        std::map<std::string, TxHwRange> stages;
        try { const auto gr = dev_->getGainRange(SOAPY_SDR_TX, (size_t)ch_); overall = {gr.minimum(), gr.maximum()}; } catch (...) {}
        try {
            for (const std::string& n : dev_->listGains(SOAPY_SDR_TX, (size_t)ch_)) {
                const auto r = dev_->getGainRange(SOAPY_SDR_TX, (size_t)ch_, n);
                stages[n] = {r.minimum(), r.maximum()};
            }
        } catch (...) {}
        return soapyTxPlan(t, rate_, overall, stages);
    }
    bool tuneAndGain(const TxTune& t, std::string& err) {
        const TxHwPlan p = plan(t);
        if (!p.note.empty()) fprintf(stderr, "transmit: %s\n", p.note.c_str());
        const double tuneHz = p.loHz;
        try { dev_->setFrequency(SOAPY_SDR_TX, (size_t)ch_, tuneHz); }
        catch (const std::exception& e) {
            char b[400];
            snprintf(b, sizeof b, "the radio's driver refused %.6g MHz as a transmit frequency: %s", txRadioHz(t) / 1e6, e.what());
            err = b;
            return false;
        }
        for (const auto& g : p.gains) {   // the overall gain (only the radio's own scale clamps), then each typed stage
            try {
                if (g.first.empty()) dev_->setGain(SOAPY_SDR_TX, (size_t)ch_, g.second);
                else dev_->setGain(SOAPY_SDR_TX, (size_t)ch_, g.first, g.second);
            } catch (const std::exception& e) {
                err = std::string("the radio's driver refused the TX gain") + (g.first.empty() ? "" : " " + g.first) + ": " + e.what();
                return false;
            }
        }
        return true;
    }
    void shut() {
        if (dev_ && stream_) {
            try { dev_->deactivateStream(stream_); } catch (...) {}
            try { dev_->closeStream(stream_); } catch (...) {}
        }
        stream_ = nullptr;
        try { if (dev_) SoapySDR::Device::unmake(dev_); } catch (...) {}
        dev_ = nullptr;
    }
    std::string args_, key_;
    int ch_ = 0;
    SoapySDR::Device* dev_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    double rate_ = 0;
    std::atomic<uint64_t> underflows_{0};
};

// What the list says about transmitting: a radio that has been opened says so exactly; before that, the drivers known to have a transmit
// channel (the radio is opened to find out when Transmit starts, and says "no transmit channel" if the guess was wrong)
void applyTxInfo(DeviceInfo& d, const std::string& driver, const SoapyRanges* r) {
    static const char* const kHinted[] = {"lime", "bladerf", "uhd", "plutosdr", "xtrx", "redpitaya", "sx", "iris", "remote"};
    bool hinted = false;
    for (const char* h : kHinted) if (driver == h) hinted = true;
    if (r && r->txKnown) {
        d.canTransmit = r->canTx;
        d.txChannels = r->nTx > 0 ? r->nTx : 1;
        d.txMinRateHz = r->txMinRate; d.txMaxRateHz = r->txMaxRate; d.txGainMinDb = r->txGainMin; d.txGainMaxDb = r->txGainMax;
    } else {
        d.canTransmit = hinted;
        // LimeSDR-USB, bladeRF 2.0 and a USRP often have two or more outputs; the exact count is filled in once the radio has been opened
        if (driver == "lime" || driver == "limesdr" || driver == "bladerf" || driver == "uhd" || driver == "xtrx" || driver == "iris" || driver == "redpitaya" || driver == "remote") d.txChannels = 0;
    }
    if (d.canTransmit) d.txGainNote = "the driver's own TX gain scale (known after the radio has been opened once)";
}

} // namespace

bool soapySupported() { return true; }

std::vector<DeviceInfo> listSoapyDevices(std::string& err) {
    soapyLocateModules();
    std::vector<DeviceInfo> out;
    try {
        // Without network discovery (the default, #215) each module is asked on its own (enumerate() with a driver calls only that module's
        // find), and the ones that search the network are left out: no device on the LAN is contacted just by listing
        SoapySDR::KwargsList found;
        if (networkDiscovery()) found = SoapySDR::Device::enumerate();
        else {
            // enumerate() loads the modules on its first call; a driver name no module has loads them without calling any find()
            SoapySDR::Device::enumerate("driver=onair-load-modules-only");
            for (const auto& fn : SoapySDR::Registry::listFindFunctions()) {
                const std::string a = soapyLocalEnumerateArgs(fn.first);
                if (a.empty()) {
                    static std::set<std::string> told;   // once per module, not at every refresh of the list
                    if (told.insert(fn.first).second) fprintf(stderr, "SoapySDR: module %s not asked (it searches the network; \"Search the network for radios\" is off)\n", fn.first.c_str());
                    continue;
                }
                for (auto& kw : SoapySDR::Device::enumerate(a)) found.push_back(kw);
            }
        }
        for (const auto& kw : found) {
            auto get = [&](const char* k) { auto it = kw.find(k); return it == kw.end() ? std::string() : it->second; };
            const std::string driver = get("driver");
            if (driver == "hackrf") continue;   // native source
            // "miri" (libmirisdr) lists an SDRplay RSP a second time as a bare Mirics chip: opening it detaches the kernel driver and streams
            // garbage or hangs, and the SDRplay API loses the radio meanwhile (issue #17). "audio" lists the sound cards, which are no radios,
            // and its probing alone printed a screen of RtAudio errors.
            if (driver == "miri" || driver == "audio") continue;
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
                    d.minRateHz = r.minRate; d.maxRateHz = r.maxRate; d.rateRanges = r.rates; d.gainMinDb = r.gainMin; d.gainMaxDb = r.gainMax; d.minFreqHz = r.minFreq; d.maxFreqHz = r.maxFreq;
                    d.hasBiasTee = !r.biasKey.empty();
                    antennas = r.antennas;
                    d.settings = r.settings;
                    applyTxInfo(d, driver, &r);
                } else applyTxInfo(d, driver, nullptr);
            }
            d.settings.insert(d.settings.begin(), ppmSetting(0.01));   // the driver's own correction, or OnAir's
            if (driver == "plutosdr") {   // SoapyPlutoSDR on the USB cable (or its USB-gadget address): the same ~4 Msps of 16-bit samples
                const std::string uri = get("uri");
                if (uri.empty() || uri.rfind("usb:", 0) == 0) d.steadyRateHz = 4e6;
                else if (uri == "ip:192.168.2.1" || uri == "ip:pluto.local") {
                    d.steadyRateHz = 3e6;   // the USB-gadget network carries less (#59); a Pluto+ on Ethernet says so with "Link" (#96)
                    d.settings.push_back(choiceSetting("link", "Link", "How this Pluto is connected at its default address. Auto: taken as the USB cable's gadget network (about 3 Msps).\n"
                                                       "Ethernet: a Pluto+ with a real network link: no limit but the converter's. USB cable: held to the USB rate.",
                                                       {"auto", "usb", "ethernet"}, {"Auto", "USB cable", "Ethernet"}, "auto"));
                }
            }
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
        {   // two radios of one model share a label: tell them apart (the serial is what selects them, the name is only shown)
            std::vector<std::string> labels;
            for (const auto& d : out) labels.push_back(d.soapyArgs.find(kAntennaTag) == std::string::npos ? d.name : std::string());
            std::map<std::string, int> count;
            for (const auto& l : labels) if (!l.empty()) count[l]++;
            std::vector<std::string> uniq = soapylogic::uniqueLabels(labels);
            for (size_t i = 0; i < out.size(); i++) if (!labels[i].empty() && count[labels[i]] > 1) out[i].name = out[i].serial.empty() ? uniq[i] : uniq[i] + " [" + out[i].serial + "]";
        }
        fprintf(stderr, "SoapySDR %s: %zu radio(s) besides HackRF\n", SoapySDR::getAPIVersion().c_str(), out.size());
        fflush(stderr);
    } catch (const std::exception& e) {
        err = std::string("SoapySDR: ") + e.what();
    }
    return out;
}

// A SoapySDR device given by its arguments that is not enumerated (a SoapyRemote server the user added): its ranges once it was started
DeviceInfo soapyDeviceFor(const std::string& args, const std::string& board, const std::string& name) {
    DeviceInfo d;
    d.kind = DeviceInfo::Soapy;
    d.board = board;
    d.soapyArgs = args;
    d.name = name;
    {
        std::lock_guard<std::mutex> lk(gRangesMu);
        const auto it = gRanges.find(rangesKey(d.serial, d.soapyArgs));
        if (it != gRanges.end()) {
            const SoapyRanges& r = it->second;
            d.minRateHz = r.minRate; d.maxRateHz = r.maxRate; d.rateRanges = r.rates; d.gainMinDb = r.gainMin; d.gainMaxDb = r.gainMax; d.minFreqHz = r.minFreq; d.maxFreqHz = r.maxFreq;
            d.hasBiasTee = !r.biasKey.empty();
            d.settings = r.settings;
            applyTxInfo(d, board, &r);
        } else applyTxInfo(d, board, nullptr);
    }
    d.settings.insert(d.settings.begin(), ppmSetting(0.01));
    return d;
}

bool soapyLearnRanges(DeviceInfo& d) {
    if (d.kind != DeviceInfo::Soapy) return false;
    std::lock_guard<std::mutex> lk(gRangesMu);
    const auto it = gRanges.find(rangesKey(d.serial, deviceArgs(d.soapyArgs)));
    if (it == gRanges.end()) return false;
    const SoapyRanges& r = it->second;
    if (!(r.gainMax > r.gainMin) && !(r.maxRate > 0)) return false;
    bool ch = false;
    auto set = [&](double& f, double v) { if (v != 0 && f != v) { f = v; ch = true; } };
    set(d.minRateHz, r.minRate); set(d.maxRateHz, r.maxRate);
    if (r.gainMax > r.gainMin && (d.gainMinDb != r.gainMin || d.gainMaxDb != r.gainMax)) { d.gainMinDb = r.gainMin; d.gainMaxDb = r.gainMax; ch = true; }
    set(d.minFreqHz, r.minFreq); set(d.maxFreqHz, r.maxFreq);
    if (ch) {
        d.rateRanges = r.rates;
        d.hasBiasTee = !r.biasKey.empty();
        // the radio's own settings (RF gain stage, gain elements ...) after the ones the listing put first (ppm), without doubling any
        for (const RadioSetting& s : r.settings) {
            bool have = false;
            for (const RadioSetting& o : d.settings) have |= o.key == s.key;
            if (!have) d.settings.push_back(s);
        }
    }
    return ch;
}

std::unique_ptr<IqSource> makeSoapySource(const DeviceInfo& d) { return std::make_unique<SoapySource>(d.soapyArgs, rangesKey(d.serial, deviceArgs(d.soapyArgs))); }
std::unique_ptr<TxSink> makeSoapyTxSink(const DeviceInfo& d) { return std::make_unique<SoapyTx>(d.soapyArgs, rangesKey(d.serial, deviceArgs(d.soapyArgs))); }

} // namespace dect2
