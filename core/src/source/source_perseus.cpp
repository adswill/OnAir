// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for the Microtelecom Perseus HF receiver: the library libperseus-sdr (LGPL-3.0) is loaded at run time when it is installed
// and is not shipped with OnAir. See native_common.h. The radio delivers complex 24-bit samples at 48 kHz to 2 Msps from its digital
// down-converter, 10 kHz to 40 MHz. The library holds the firmware and the FPGA images itself.
#include "native_common.h"
#include "perseus_logic.h"
#include "dect2/common/diag.h"
#include <map>

namespace dect2 {
namespace native {

struct PerseusApi {
    struct descr_t;
    struct prodid_t {   // eeprom_prodid (packed)
        uint16_t sn, prodcode;
        uint8_t hwrel, hwver, signature[6];
    } __attribute__((packed));
    typedef int (DECT2_CALL* cb_t)(void* buf, int bytes, void* extra);
    int (DECT2_CALL* init)() = nullptr;                                              // returns the number of radios
    int (DECT2_CALL* exit_)() = nullptr;
    descr_t* (DECT2_CALL* open)(int index) = nullptr;
    int (DECT2_CALL* close)(descr_t*) = nullptr;
    int (DECT2_CALL* firmware_download)(descr_t*, char* file) = nullptr;            // nullptr: the library's own firmware
    int (DECT2_CALL* get_product_id)(descr_t*, prodid_t*) = nullptr;
    int (DECT2_CALL* set_attenuator_n)(descr_t*, int index) = nullptr;
    int (DECT2_CALL* set_adc)(descr_t*, int dither, int preamp) = nullptr;
    int (DECT2_CALL* set_ddc_center_freq)(descr_t*, double hz, int presel) = nullptr;
    int (DECT2_CALL* start_async_input)(descr_t*, uint32_t bufSize, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_async_input)(descr_t*) = nullptr;
    int (DECT2_CALL* set_sampling_rate)(descr_t*, int rate) = nullptr;
    int (DECT2_CALL* get_sampling_rates)(descr_t*, int* buf, unsigned size) = nullptr;
    char* (DECT2_CALL* error_str)() = nullptr;   // optional
    bool ok = false;
    DynLib lib;
    PerseusApi() {
        if (nativeDisabled() || !lib.open(libNames("perseus-sdr", {".0"}, {"perseus-sdr.dll", "libperseus-sdr.dll"}))) return;
        ok = lib.get(init, "perseus_init") && lib.get(exit_, "perseus_exit") && lib.get(open, "perseus_open") && lib.get(close, "perseus_close") &&
             lib.get(firmware_download, "perseus_firmware_download") && lib.get(get_product_id, "perseus_get_product_id") &&
             lib.get(set_attenuator_n, "perseus_set_attenuator_n") && lib.get(set_adc, "perseus_set_adc") &&
             lib.get(set_ddc_center_freq, "perseus_set_ddc_center_freq") && lib.get(start_async_input, "perseus_start_async_input") &&
             lib.get(stop_async_input, "perseus_stop_async_input") && lib.get(set_sampling_rate, "perseus_set_sampling_rate") &&
             lib.get(get_sampling_rates, "perseus_get_sampling_rates");
        lib.opt(error_str, "perseus_errorstr");
    }
};
PerseusApi& perseus() { static PerseusApi a; return a; }

namespace {

constexpr double kMinFreqHz = 10e3, kMaxFreqHz = 40e6;   // the DDC covers 0 to half the 80 MHz converter clock
constexpr double kGainMaxDb = 30;                        // the attenuator: 0, -10, -20, -30 dB; the top of the gain slider is no attenuation
const int kDefaultRates[] = {48000, 95000, 96000, 125000, 192000, 250000, 500000, 1000000, 1600000, 2000000};
constexpr uint32_t kBufBytes = 6 * 1024 * 6;             // the library suggests 6 * 1024 (bytes); a multiple of one sample (6 bytes)

std::string rateText(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%g MHz", hz / 1e6);
    else snprintf(b, sizeof b, "%.0f kHz", hz / 1e3);
    return b;
}
std::string libError() {
    const char* e = perseus().error_str ? perseus().error_str() : nullptr;
    return e && *e ? std::string(e) : std::string("unknown error");
}

// Everything that goes through the library's USB handling takes turns. perseus_init() scans the bus once; a rescan is perseus_exit() and
// perseus_init() again, only while no radio is open here.
std::mutex gMu;
int gOpen = 0;                       // radios this program has open
int gCount = -1;                     // radios the library counted at its last init (-1: not initialised)
struct PerseusModel { uint16_t sn = 0; std::vector<double> rates; };
std::map<int, PerseusModel> gModels; // per index, read once (a radio is opened and its firmware loaded to read them)
std::vector<DeviceInfo> gKnown;      // the last listing

std::vector<double> ratesOf(PerseusApi::descr_t* d) {
    int buf[32] = {0};
    std::vector<double> v;
    if (perseus().get_sampling_rates(d, buf, 32) == 0)
        for (int r : buf) if (r > 0) v.push_back(r);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.empty()) v.assign(std::begin(kDefaultRates), std::end(kDefaultRates));
    return v;
}

std::vector<RadioSetting> perseusSettings() {
    return {ppmSetting(0.01),
            choiceSetting("preamp", "Preamp",
                          "The radio's +10 dB preamplifier in front of the converter. Off (the default); turn it on for weak signals when there is\n"
                          "no strong one nearby.",
                          {"off", "on"}, {"Off", "On"}, "off"),
            choiceSetting("dither", "Dither",
                          "Noise the radio adds in front of the converter to spread its distortion. Off (the default); on can help with strong\n"
                          "signals at the price of a little more noise.",
                          {"off", "on"}, {"Off", "On"}, "off"),
            choiceSetting("presel", "Preselector",
                          "The radio's own band filters in front of the converter, switched automatically with the frequency. On (the default).\n"
                          "Off: the whole band reaches the converter.",
                          {"on", "off"}, {"On", "Off"}, "on")};
}

} // namespace

class PerseusSource : public NativeSource {
public:
    PerseusSource(int index, uint16_t sn) : index_(index), sn_(sn) {}
    ~PerseusSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!perseus().ok) { DIAG_ERROR(514286, "The libperseus-sdr library could not be loaded, so the Perseus cannot be opened."); err = "Perseus: the radio's library (libperseus-sdr) is missing"; openFailure_ = OpenFailure::Final; return false; }
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (dev_) return apply(s, err, false);   // (not reached: closeDevice() comes first)
        dev_ = perseus().open(index_);
        if (!dev_) {
            DIAG_ERROR(604213, "Opening the Perseus failed (%s), so the radio stays closed.", libError().c_str());
            err = "Perseus: cannot open the radio (" + libError() + "): it is in use by another program, unplugged, or has no USB access";
            return false;
        }
        gOpen++;
        if (const int r = perseus().firmware_download(dev_, nullptr); r != 0) {
            perseus().close(dev_);
            dev_ = nullptr;
            gOpen--;
            DIAG_ERROR(268357, "Loading the firmware into the Perseus failed with library code %d (%s), so the radio stays closed.", r, libError().c_str());
            err = "Perseus: the firmware could not be loaded (" + libError() + ")";
            return false;
        }
        PerseusApi::prodid_t id = {};
        if (sn_ && perseus().get_product_id(dev_, &id) == 0 && id.sn != sn_) {   // the list changed between the look and the open
            perseus().close(dev_);
            dev_ = nullptr;
            gOpen--;
            DIAG_ERROR(849162, "The Perseus with serial %u was not found because another Perseus answered in its place, so it is closed.", (unsigned)sn_);
            err = "Perseus " + std::to_string(sn_) + " not found (another Perseus is in its place)";
            return false;
        }
        rates_ = ratesOf(dev_);
        att_ = -1; preamp_ = dither_ = -1;
        return apply(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        return apply(s, err, live);
    }
    // safe twice, after a failed open and after an unplug
    void closeDevice() override {
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return;
        if (started_) { stopping_ = true; perseus().stop_async_input(dev_); started_ = false; }
        perseus().close(dev_);
        dev_ = nullptr;
        gOpen--;
    }
    void streamLoop() override {
        {
            std::lock_guard<std::mutex> lk(api_);
            if (!dev_ || !run_) return;
            stopping_ = false;
            if (const int r = perseus().start_async_input(dev_, kBufBytes, &PerseusSource::callback, this); r != 0) {
                DIAG(630418, "Starting the Perseus sample stream failed with library code %d (%s), so the source stops.", r, libError().c_str());
                return;
            }
            started_ = true;
        }
        while (run_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(api_);
        stopping_ = true;
        if (dev_ && started_) { perseus().stop_async_input(dev_); started_ = false; }
    }

private:
    // (under api_) live: the stream is running, only the frequency, the attenuator and the ADC switches may change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        if (!dev_) { DIAG_DEBUG(472095, "The Perseus was asked to configure itself while no device is open, so nothing is changed."); return false; }
        auto& api = perseus();
        if (!live) {
            // the smallest rate that is fast enough, else the fastest
            double pick = rates_.back();
            for (double r : rates_) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            if (s.sampleRate > rates_.back() * 1.001) err = "Perseus runs at " + rateText(rates_.back()) + " at most (" + rateText(s.sampleRate) + " was requested)";
            if (const int r = api.set_sampling_rate(dev_, (int)pick); r != 0) {
                DIAG_ERROR(395716, "The Perseus did not accept a sample rate of %.0f samples per second (library code %d), so the radio is not started.", pick, r);
                err = "Perseus: the radio does not accept " + rateText(pick) + " (" + libError() + ")";
                return false;
            }
            rate_ = pick;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        if (s.centerHz < kMinFreqHz || s.centerHz > kMaxFreqHz)
            addNote(err, "the Perseus is specified for 10 kHz to 40 MHz; the driver may not reach " + rateText(s.centerHz) + " or the signal may be weak or absent");
        const int dither = radioOption(s, "dither", "off") == "on" ? 1 : 0, preamp = radioOption(s, "preamp", "off") == "on" ? 1 : 0;
        if (dither != dither_ || preamp != preamp_) {
            if (api.set_adc(dev_, dither, preamp) == 0) { dither_ = dither; preamp_ = preamp; }
        }
        const int att = perseusAttStep(s.gainDb);
        if (att != att_ && api.set_attenuator_n(dev_, att) == 0) att_ = att;
        const bool presel = radioOption(s, "presel", "on") != "off";
        const double tuneHz = std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0);
        if (const int r = api.set_ddc_center_freq(dev_, tuneHz, presel ? 1 : 0); r != 0) {
            DIAG_ERROR(918463, "The Perseus library refused a centre frequency of %.0f Hz with code %d, so the tune fails.", s.centerHz, r);
            err = "Perseus: the library refused " + rateText(s.centerHz) + " (" + libError() + ")";
            return false;
        }
        return true;
    }
    static int DECT2_CALL callback(void* buf, int bytes, void* user) {
        auto* self = static_cast<PerseusSource*>(user);
        if (!self || self->stopping_ || !self->run_ || !buf || bytes < 6) return 0;
        const size_t n = (size_t)bytes / 6;
        self->conv_.resize(n);
        perseusConvert(static_cast<const uint8_t*>(buf), (size_t)bytes, reinterpret_cast<float*>(self->conv_.data()));
        self->push(self->conv_.data(), n);
        return 0;
    }

    int index_;
    uint16_t sn_;
    PerseusApi::descr_t* dev_ = nullptr;
    std::mutex api_;                     // the control calls of this radio (retune, open, close, start, stop)
    std::vector<double> rates_ = std::vector<double>(std::begin(kDefaultRates), std::end(kDefaultRates));
    int att_ = -1, preamp_ = -1, dither_ = -1;   // what the radio is set to (-1: not yet)
    std::atomic<bool> stopping_{true};   // a stop is under way (or no stream): the samples callback returns at once
    bool started_ = false;
    std::vector<cf32> conv_;             // (only the library's callback thread touches it)
};

void listPerseus(std::vector<DeviceInfo>& out) {
    if (!perseus().ok) return;
    std::lock_guard<std::mutex> g(gMu);
    // a radio this program has open cannot be opened again to read its serial: the last listing stands while one is in use
    if (gOpen > 0 && !gKnown.empty()) { for (const auto& d : gKnown) out.push_back(d); return; }
    if (gCount >= 0) perseus().exit_();
    const int n = std::clamp(perseus().init(), 0, 16);
    if (n != gCount) { gModels.clear(); fprintf(stderr, "Perseus: the library lists %d radio(s)\n", n); fflush(stderr); }
    gCount = n;
    std::vector<DeviceInfo> list;
    for (int i = 0; i < n; i++) {
        // the serial and the sample rates come from the radio: it is opened once for them (this loads its firmware)
        if (!gModels.count(i)) {
            PerseusApi::descr_t* dev = perseus().open(i);
            if (dev) {
                PerseusModel m;
                if (perseus().firmware_download(dev, nullptr) == 0) {
                    PerseusApi::prodid_t id = {};
                    if (perseus().get_product_id(dev, &id) == 0) m.sn = id.sn;
                    m.rates = ratesOf(dev);
                    gModels[i] = m;
                }
                perseus().close(dev);
            }
        }
        const auto it = gModels.find(i);
        const PerseusModel m = it != gModels.end() ? it->second : PerseusModel();
        std::vector<double> rates = m.rates;
        if (rates.empty()) rates.assign(std::begin(kDefaultRates), std::end(kDefaultRates));
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "perseus";
        d.serial = m.sn ? std::to_string(m.sn) : std::string();
        d.nativeArgs = std::to_string(i) + ":" + std::to_string(m.sn);
        d.name = std::string("Perseus") + (m.sn ? " " + d.serial : "");
        d.minRateHz = rates.front(); d.maxRateHz = rates.back();
        for (double r : rates) d.rateRanges.emplace_back(r, r);
        d.gainMinDb = 0; d.gainMaxDb = kGainMaxDb;
        d.minFreqHz = kMinFreqHz; d.maxFreqHz = kMaxFreqHz;
        d.settings = perseusSettings();
        list.push_back(d);
    }
    gKnown = list;
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makePerseus(const DeviceInfo& d) {
    const size_t c = d.nativeArgs.find(':');
    const int index = atoi(d.nativeArgs.c_str());
    const unsigned long sn = c == std::string::npos ? 0 : strtoul(d.nativeArgs.c_str() + c + 1, nullptr, 10);
    return std::make_unique<PerseusSource>(index, (uint16_t)sn);
}

} // namespace native
} // namespace dect2
