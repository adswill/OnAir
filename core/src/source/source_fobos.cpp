// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for the RigExpert Fobos SDR running the "agile" firmware: the radio's own library (libfobos-sdr-agile: fobos_sdr.dll,
// libfobos_sdr.so) is loaded at run time when it is installed. See native_common.h.
// The library only sees radios with the agile firmware (USB device release 0x0101; hardware rev. 4 has it from the factory, rev. 2 and 3 after
// the firmware update from RigExpert). A Fobos SDR with the stock firmware is not listed here.
//
// The radio has two paths:
// - the RF input, 50 MHz to 6 GHz, through the tuner: complex samples at one of the converter's rates (8 to 50 Msps; 64 on rev. 4)
// - the HF1 and HF2 inputs, sampled directly by the two converters (HF1 arrives as the I samples, HF2 as the Q samples). Each is a real
//   signal from 0 to half the converter's rate, so this driver shifts the wanted frequency to zero, filters and decimates (RealDdc, native_realddc.h).
// Below 50 MHz the HF input chosen in the radio settings is used (HF1 unless set otherwise).
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "native_common.h"
#include "native_realddc.h"
#include "dect2/dsp/dsp_compat.h"
#include <map>

namespace dect2 {
namespace native {

struct FobosApi {
    struct dev_t;
    typedef void (*cb_t)(float* buf, uint32_t n, dev_t* sender, void* user);   // fobos_sdr_cb_t: n complex samples (float I/Q pairs)
    int (DECT2_CALL* list_devices)(char* serials) = nullptr;                    // the serials, each followed by a space; returns the count
    int (DECT2_CALL* open)(dev_t**, uint32_t index) = nullptr;
    int (DECT2_CALL* close)(dev_t*) = nullptr;
    int (DECT2_CALL* get_board_info)(dev_t*, char* hw, char* fw, char* manufacturer, char* product, char* serial) = nullptr;
    int (DECT2_CALL* set_frequency)(dev_t*, double hz) = nullptr;
    int (DECT2_CALL* set_direct_sampling)(dev_t*, unsigned enabled) = nullptr;
    int (DECT2_CALL* set_lna_gain)(dev_t*, unsigned step) = nullptr;            // 0, 1: 0 dB, 2: +16 dB, 3: +33 dB
    int (DECT2_CALL* set_vga_gain)(dev_t*, unsigned step) = nullptr;            // 0..31, 2 dB each
    int (DECT2_CALL* get_samplerates)(dev_t*, double* values, unsigned* count) = nullptr;
    int (DECT2_CALL* set_samplerate)(dev_t*, double hz) = nullptr;
    int (DECT2_CALL* read_async)(dev_t*, cb_t, void* user, uint32_t bufCount, uint32_t bufLength) = nullptr;   // blocks until cancel_async()
    int (DECT2_CALL* cancel_async)(dev_t*) = nullptr;
    int (DECT2_CALL* get_api_info)(char* lib, char* drv) = nullptr;             // optional
    int (DECT2_CALL* set_clk_source)(dev_t*, int external) = nullptr;           // optional
    const char* (DECT2_CALL* error_name)(int) = nullptr;                        // optional
    bool ok = false;
    DynLib lib;
    FobosApi() {
        if (nativeDisabled() || !lib.open(libNames("fobos_sdr", {".0"}, {"fobos_sdr.dll", "libfobos_sdr.dll"}))) return;
        ok = lib.get(list_devices, "fobos_sdr_list_devices") && lib.get(open, "fobos_sdr_open") && lib.get(close, "fobos_sdr_close") &&
             lib.get(get_board_info, "fobos_sdr_get_board_info") && lib.get(set_frequency, "fobos_sdr_set_frequency") &&
             lib.get(set_direct_sampling, "fobos_sdr_set_direct_sampling") && lib.get(set_lna_gain, "fobos_sdr_set_lna_gain") &&
             lib.get(set_vga_gain, "fobos_sdr_set_vga_gain") && lib.get(get_samplerates, "fobos_sdr_get_samplerates") &&
             lib.get(set_samplerate, "fobos_sdr_set_samplerate") && lib.get(read_async, "fobos_sdr_read_async") &&
             lib.get(cancel_async, "fobos_sdr_cancel_async");
        lib.opt(get_api_info, "fobos_sdr_get_api_info");
        lib.opt(set_clk_source, "fobos_sdr_set_clk_source");
        lib.opt(error_name, "fobos_sdr_error_name");
        if (ok && get_api_info) {
            char v[256] = {0}, drv[256] = {0};
            get_api_info(v, drv);
            fprintf(stderr, "Fobos SDR: library %s (%s)\n", trimmed(v, sizeof v).c_str(), trimmed(drv, sizeof drv).c_str());
            fflush(stderr);
        }
    }
};
FobosApi& fobos() { static FobosApi a; return a; }

namespace {

constexpr double kRfMinHz = 50e6, kRfMaxHz = 6e9;   // the tuner path (rigexpert.com; the library's band plan starts at 50 MHz)
constexpr double kHfBelowHz = 49.5e6;               // below this the HF inputs are used (the band plan counts in whole MHz: 49.5 is its 50)
constexpr double kHfMinHz = 0.1e6;                  // the HF inputs are transformer coupled
constexpr double kGainMaxDb = 95;                   // LNA 33 dB + VGA 62 dB
constexpr double kLnaDb[4] = {0, 0, 16, 33};
// The library scales the 14-bit samples as if they were 16-bit: full scale arrives as 0.25. OnAir's level displays and AGC count 1.0 as full scale.
constexpr float kScale = 4.f;
// what the library uses when the radio could not be asked (hardware rev. 2 and 3)
const double kDefaultRates[] = {8e6, 10e6, 12.5e6, 16e6, 20e6, 25e6, 32e6, 40e6, 50e6};
constexpr uint32_t kBufCount = 16, kBufLength = 65536;   // as RigExpert's recorder: 16 USB transfers of 65536 samples

std::string mhzText(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.6g MHz", hz / 1e6);
    return b;
}

std::string errorText(int code) {
    const char* n = fobos().error_name ? fobos().error_name(code) : nullptr;
    return n && *n ? std::string(n) : "error " + std::to_string(code);
}

// a serial the library could read (it reports XXXXXXXXXXXX for a radio it cannot open: in use, or no access)
bool readable(const std::string& serial) { return !serial.empty() && serial.find_first_not_of('X') != std::string::npos; }

// Everything that goes through the computer's USB stack for these radios takes turns: the library opens a libusb context of its own for each
// listing and each open, and a listing opens every radio for a moment to read its serial.
std::mutex gMu;
int gOpen = 0;                        // radios this program has open
std::vector<DeviceInfo> gKnown;       // the last listing
struct FobosModel { std::string hw, fw, product; std::vector<double> rates; };
std::map<std::string, FobosModel> gModels;   // per serial, read once (opening the radio reprograms its synthesizers: not at every rescan)

// (under gMu) the serials in the library's order, which is also the order fobos_sdr_open() counts in
std::vector<std::string> readSerials() {
    std::vector<char> buf(16384, 0);   // the library appends without a limit: 13 characters per radio
    const int n = fobos().list_devices(buf.data());
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = buf.data(); *p; p++) {
        if (*p == ' ') { out.push_back(cur); cur.clear(); }   // (an empty one too: a radio without a serial string keeps its place)
        else cur += *p;
    }
    if (!cur.empty()) out.push_back(cur);
    out.resize((size_t)std::clamp(n, 0, 64));
    return out;
}

std::vector<double> ratesOf(FobosApi::dev_t* dev) {
    std::vector<double> v;
    unsigned count = 0;
    if (fobos().get_samplerates(dev, nullptr, &count) == 0 && count > 0 && count <= 64) {
        v.resize(count);
        if (fobos().get_samplerates(dev, v.data(), &count) != 0) v.clear();
        else v.resize(std::min<size_t>(count, v.size()));
    }
    v.erase(std::remove_if(v.begin(), v.end(), [](double r) { return !(r > 0); }), v.end());
    std::sort(v.begin(), v.end());
    if (v.empty()) v.assign(std::begin(kDefaultRates), std::end(kDefaultRates));
    return v;
}

std::vector<RadioSetting> fobosSettings(bool clock) {
    std::vector<RadioSetting> v = {
        ppmSetting(0.01),
        choiceSetting("hf", "HF input (below 50 MHz)",
                      "The tuner starts at 50 MHz. Below that the radio samples one of its two HF inputs directly (0.1 MHz to half the\n"
                      "converter's rate, 25 MHz at 50 Msps), and OnAir shifts and filters the wanted part.\n"
                      "HF1 / HF2: the connector the HF antenna is on. Off: the RF input only.",
                      {"hf1", "hf2", "off"}, {"HF1", "HF2", "Off"}, "hf1"),
        choiceSetting("hfadc", "HF converter rate",
                      "The rate the converter runs at for the HF inputs. Fastest (the default): the widest range and the least folding of\n"
                      "signals from above half the rate. A lower rate takes less of the computer, and the HF range ends at half of it.",
                      {"auto", "40", "32", "25", "20"}, {"Fastest", "40 Msps", "32 Msps", "25 Msps", "20 Msps"}, "auto"),
        choiceSetting("lna", "LNA",
                      "The low-noise amplifier of the RF input has three steps. Auto: it follows the gain slider (0 dB below 16 dB, +16 dB up to\n"
                      "33 dB, +33 dB above), and the slider's rest is the VGA in 2 dB steps. A fixed step: for strong signals nearby (0 dB)\n"
                      "or weak ones (+33 dB); the slider then moves the VGA alone. The HF inputs have no amplifier.",
                      {"auto", "0", "16", "33"}, {"Auto (with the gain)", "0 dB", "+16 dB", "+33 dB"}, "auto")};
    if (clock)
        v.push_back(choiceSetting("clock", "Reference clock",
                                  "Internal: the radio's own oscillator (the default). External: a 10 MHz reference on the radio's clock input;\n"
                                  "without one the radio does not run.",
                                  {"internal", "external"}, {"Internal", "External (10 MHz)"}, "internal"));
    return v;
}

} // namespace

class FobosSource : public NativeSource {
public:
    FobosSource(uint32_t index, std::string serial) : index_(index), serial_(std::move(serial)) {}
    ~FobosSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!fobos().ok) { DIAG_ERROR(417803, "The Fobos SDR library (fobos_sdr) could not be loaded, so the radio cannot be opened."); err = "Fobos SDR: the radio's library (fobos_sdr) is missing"; openFailure_ = OpenFailure::Final; return false; }
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (dev_) return apply(s, err, false);   // (not reached: closeDevice() comes first)
        // The library opens by position among the radios with the agile firmware: an unplug or a second radio shifts it. The serial of the
        // listing finds the radio again.
        uint32_t index = index_;
        if (readable(serial_)) {
            const std::vector<std::string> now = readSerials();
            int at = -1;
            bool unread = false;
            for (size_t i = 0; i < now.size(); i++) {
                if (!readable(now[i])) unread = true;
                else if (now[i] == serial_ && (at < 0 || i == index_)) at = (int)i;
            }
            if (at >= 0) index = (uint32_t)at;
            else if (!unread) { DIAG_ERROR(922526, "The Fobos SDR with serial %s is not in the device list, so it cannot be opened (unplugged?).", serial_.c_str()); err = "Fobos SDR " + serial_ + " not found (unplugged?)"; return false; }
        }
        const int r = fobos().open(&dev_, index);
        if (r != 0 || !dev_) {
            dev_ = nullptr;
            DIAG_ERROR(802197, "Opening the Fobos SDR failed with library code %d (%s), so the radio stays closed.", r, errorText(r).c_str());
            err = "Fobos SDR: cannot open the radio (" + errorText(r) + "): it is in use by another program, unplugged, or has no WinUSB / libusb access";
            return false;
        }
        gOpen++;
        char hw[256] = {0}, fw[256] = {0}, man[256] = {0}, prod[256] = {0}, ser[256] = {0};
        fobos().get_board_info(dev_, hw, fw, man, prod, ser);
        const std::string got = trimmed(ser, sizeof ser);
        if (readable(serial_) && readable(got) && got != serial_) {   // the list changed between the look and the open
            fobos().close(dev_);
            dev_ = nullptr;
            gOpen--;
            DIAG_ERROR(387273, "The Fobos SDR with serial %s was not found because another Fobos SDR answered in its place, so it is closed.", serial_.c_str());
            err = "Fobos SDR " + serial_ + " not found (another Fobos SDR is in its place)";
            return false;
        }
        fprintf(stderr, "Fobos SDR: opened %s, hardware %s, firmware %s\n", got.c_str(), trimmed(hw, sizeof hw).c_str(), trimmed(fw, sizeof fw).c_str());
        fflush(stderr);
        rates_ = ratesOf(dev_);
        // what the library and the firmware start with: the RF input, no gain, the internal clock
        direct_ = false;
        lna_ = vga_ = -1;
        clockExt_ = false;
        adcHz_ = 0;
        return apply(s, err, false);
    }
    // A retune that moves between the RF and the HF inputs, or needs another converter rate or decimation, stops the stream for it, as a
    // change of the sample rate does (NativeSource::retune): the firmware is not asked to change its rate while it streams.
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (live) {
            bool restart = false;
            {
                std::lock_guard<std::mutex> lk(api_);
                if (!dev_) { DIAG_DEBUG(630161, "The Fobos SDR was asked to retune while no device is open, so nothing is changed."); return false; }
                const Plan p = plan(s);
                restart = p.direct != direct_ || p.adcHz != adcHz_ || p.decim != decim_ || p.channel != channel_;
            }
            if (restart) {
                stopStream();
                bool ok;
                { std::lock_guard<std::mutex> lk(api_); ok = apply(s, err, false); }
                startStream();
                return ok;
            }
        }
        std::lock_guard<std::mutex> lk(api_);
        return apply(s, err, live);
    }
    // safe twice, after a failed open and after an unplug (the stream has ended by then: stop() and the stream thread see to that)
    void closeDevice() override {
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return;
        fobos().close(dev_);
        dev_ = nullptr;
        gOpen--;
    }
    void streamLoop() override {
        FobosApi::dev_t* dev;
        { std::lock_guard<std::mutex> lk(api_); dev = dev_; }
        if (!dev || !run_) return;
        // fobos_sdr_cancel_async() only acts once the library's stream has reached its running state: a stop that comes while it is still
        // starting would be lost and read_async would never return. So the cancel is repeated until read_async is back. This also ends a
        // stream that delivers nothing (no callback to notice the stop in).
        std::atomic<bool> reading{true};
        std::thread guard([&] {
            dect2::setThreadName("onair-fobos-g");
            while (reading) {
                if (!run_) fobos().cancel_async(dev);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        const int r = fobos().read_async(dev, &FobosSource::callback, this, kBufCount, kBufLength);   // blocks
        reading = false;
        guard.join();
        if (run_) { DIAG(987379, "The Fobos SDR sample stream ended unexpectedly with library code %d (%s), so the source stops.", r, errorText(r).c_str()); fprintf(stderr, "Fobos SDR: the stream ended (%s)\n", errorText(r).c_str()); fflush(stderr); }
    }
    void interrupt() override { if (dev_) fobos().cancel_async(dev_); }

private:
    struct Plan {
        bool direct = false;   // an HF input, sampled directly
        int channel = 0;       // 0: HF1 (the I samples), 1: HF2 (the Q samples)
        double adcHz = 0;      // the converter's rate
        int decim = 1;         // HF: output rate = adcHz / decim
        double outHz = 0;
    };
    Plan plan(const TuneSettings& s) const {
        Plan p;
        const std::string hf = radioOption(s, "hf", "hf1");
        p.direct = hf != "off" && s.centerHz < kHfBelowHz;
        p.channel = p.direct && hf == "hf2" ? 1 : 0;
        const double want = s.sampleRate > 0 ? s.sampleRate : rates_.front();
        if (!p.direct) {
            // the radio offers a list (8 to 50 Msps): the smallest one that is fast enough
            p.adcHz = rates_.back();
            for (double r : rates_) if (r >= want * 0.999) { p.adcHz = r; break; }
            p.outHz = p.adcHz;
            return p;
        }
        // HF: the converter as fast as it goes (or as the "hfadc" setting allows), and the largest decimation that still gives the rate asked for
        const double cap = radioNumber(s, "hfadc", 0) * 1e6;
        p.adcHz = rates_.back();
        if (cap > 0) { p.adcHz = rates_.front(); for (double r : rates_) if (r <= cap * 1.001) p.adcHz = r; }
        p.decim = (int)std::clamp(std::floor(p.adcHz / want * 1.001), 2.0, 2048.0);
        p.outHz = p.adcHz / p.decim;
        return p;
    }
    // (under api_) live: the stream is running, only the frequency, the gain and the clock may change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        if (!dev_) { DIAG_DEBUG(885117, "The Fobos SDR was asked to configure itself while no device is open, so nothing is changed."); return false; }
        auto& api = fobos();
        const Plan p = plan(s);
        const double f = std::max(s.centerHz, 0.0);
        if (!live) {
            if (const int r = api.set_samplerate(dev_, p.adcHz); r != 0) { DIAG_ERROR(310057, "The Fobos SDR refused a sample rate of %.0f Hz with library code %d, so the radio is not started.", (double)p.adcHz, r); err = "Fobos SDR: the sample rate of " + mhzText(p.adcHz) + " was refused (" + errorText(r) + ")"; return false; }
            if (p.direct != direct_) {
                if (const int r = api.set_direct_sampling(dev_, p.direct ? 1 : 0); r != 0) { DIAG_ERROR(496825, "The Fobos SDR could not switch between its HF and RF inputs (library code %d), so the tune fails.", r); err = "Fobos SDR: the HF inputs could not be switched (" + errorText(r) + ")"; return false; }
                fprintf(stderr, "Fobos SDR: %s at %s\n", p.direct ? (p.channel ? "HF2 input (direct sampling)" : "HF1 input (direct sampling)") : "RF input", mhzText(f).c_str());
                fflush(stderr);
            }
            std::lock_guard<std::mutex> dl(ddcMu_);
            direct_ = p.direct; channel_ = p.channel; adcHz_ = p.adcHz; decim_ = p.decim;
            if (direct_) ddc_.design(decim_);
            rate_ = p.outHz;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        // the library has no frequency correction: a correction the user set is taken out of the frequency
        const double tuneHz = ppmCorrectedHz(f, radioPpm(s));
        if (direct_) {
            {
                std::lock_guard<std::mutex> dl(ddcMu_);
                ddc_.tune(adcHz_, tuneHz, kScale);   // a mixer: above half the converter rate it aliases, as the hardware would
            }
            if (f > adcHz_ / 2 || f < kHfMinHz)
                addNote(err, "the HF inputs of this Fobos SDR are specified for " + mhzText(kHfMinHz) + " to " + mhzText(adcHz_ / 2) + " (half the converter's rate), the RF input starts at " +
                      mhzText(kRfMinHz) + "; the driver may not reach " + mhzText(f) + " or the signal may be weak or absent");
        } else {
            if (const int r = api.set_frequency(dev_, std::max(tuneHz, 0.0)); r != 0) { DIAG_ERROR(233572, "The Fobos SDR library refused a centre frequency of %.0f Hz with code %d, so the tune fails.", f, r); err = "Fobos SDR: the library refused " + mhzText(f) + (f < kRfMinHz || f > kRfMaxHz ? " (the RF input is specified for " + mhzText(kRfMinHz) + " to " + mhzText(kRfMaxHz) + ")" : std::string()) + " (" + errorText(r) + ")"; return false; }
            if (f < kHfBelowHz || f > kRfMaxHz)
                addNote(err, "the RF input of this Fobos SDR is specified for " + mhzText(kRfMinHz) + " to " + mhzText(kRfMaxHz) + "; the driver may not reach " + mhzText(f) +
                      " or the signal may be weak or absent" + (f < kHfBelowHz ? " (the HF input is switched off in the radio settings)" : ""));
        }
        // The gain slider (0..95 dB) -> the LNA step and the VGA in 2 dB steps. Auto: the LNA as high as the gain allows (the lowest noise);
        // a fixed LNA step (TuneSettings "lna") leaves the slider to the VGA.
        const double g = std::clamp(s.gainDb, 0.0, kGainMaxDb);
        const std::string lnaMode = radioOption(s, "lna", "auto");
        int lna = lnaMode == "0" ? 0 : lnaMode == "16" ? 2 : lnaMode == "33" ? 3 : g >= kLnaDb[3] ? 3 : g >= kLnaDb[2] ? 2 : 0;
        const int vga = (int)std::clamp(std::lround((g - kLnaDb[lna]) / 2), 0L, 31L);
        if (lna != lna_ && api.set_lna_gain(dev_, (unsigned)lna) == 0) lna_ = lna;
        if (vga != vga_ && api.set_vga_gain(dev_, (unsigned)vga) == 0) vga_ = vga;
        if (api.set_clk_source) {
            const bool ext = radioOption(s, "clock", "internal") == "external";
            if (ext != clockExt_ && api.set_clk_source(dev_, ext ? 1 : 0) == 0) clockExt_ = ext;
        }
        return true;
    }
    static void callback(float* buf, uint32_t n, FobosApi::dev_t*, void* user) {
        auto* self = static_cast<FobosSource*>(user);
        if (!self->run_ || !buf || !n) return;   // (the guard in streamLoop() ends the stream)
        self->convert(buf, n);
    }
    void convert(const float* buf, size_t n) {
        std::lock_guard<std::mutex> dl(ddcMu_);
        if (direct_) {
            conv_.clear();
            ddc_.process(buf + channel_, n, 2, conv_);
        } else {
            conv_.resize(n);
            float* o = reinterpret_cast<float*>(conv_.data());
            for (size_t i = 0; i < 2 * n; i++) o[i] = buf[i] * kScale;
        }
        push(conv_.data(), conv_.size());
    }

    uint32_t index_;
    std::string serial_;
    FobosApi::dev_t* dev_ = nullptr;
    std::mutex api_;                 // the control calls of this radio (retune, open, close); the stream thread is in read_async meanwhile
    std::vector<double> rates_ = std::vector<double>(std::begin(kDefaultRates), std::end(kDefaultRates));
    int lna_ = -1, vga_ = -1;        // what the radio is set to (-1: not yet)
    bool clockExt_ = false;
    std::mutex ddcMu_;               // the conversion state below: the stream's callback against a retune
    bool direct_ = false;
    int channel_ = 0, decim_ = 1;
    double adcHz_ = 0;
    RealDdc ddc_;
};

void listFobos(std::vector<DeviceInfo>& out) {
    if (!fobos().ok) return;
    std::lock_guard<std::mutex> g(gMu);
    // A radio this program has open cannot be opened again to read its serial (Windows: one WinUSB handle per device), and the library then
    // lists it as XXXXXXXXXXXX: the last listing stands while one is in use.
    if (gOpen > 0 && !gKnown.empty()) { for (const auto& d : gKnown) out.push_back(d); return; }
    const std::vector<std::string> serials = readSerials();
    // in the log file as well, when it changes: when the radio does not show up this says whether the library saw it
    static size_t logged = (size_t)-1;
    if (serials.size() != logged) {
        logged = serials.size();
        fprintf(stderr, "Fobos SDR: the library lists %zu radio(s) with the agile firmware\n", serials.size());
        fflush(stderr);
    }
    std::vector<DeviceInfo> list;
    for (size_t i = 0; i < serials.size() && i < 16; i++) {
        const std::string& sn = serials[i];
        // the sample rates and the revision come from the radio: it is opened once for them (not when another program has it)
        if (readable(sn) && !gModels.count(sn) && gOpen == 0) {
            FobosApi::dev_t* dev = nullptr;
            if (fobos().open(&dev, (uint32_t)i) == 0 && dev) {
                char hw[256] = {0}, fw[256] = {0}, man[256] = {0}, prod[256] = {0}, ser[256] = {0};
                fobos().get_board_info(dev, hw, fw, man, prod, ser);
                if (trimmed(ser, sizeof ser) == sn) gModels[sn] = {trimmed(hw, sizeof hw), trimmed(fw, sizeof fw), trimmed(prod, sizeof prod), ratesOf(dev)};
                fobos().close(dev);
            }
        }
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "fobos";
        d.serial = readable(sn) ? sn : std::string();
        d.nativeArgs = std::to_string(i);
        std::vector<double> rates(std::begin(kDefaultRates), std::end(kDefaultRates));
        std::string product = "Fobos SDR";
        const auto it = gModels.find(sn);
        if (it != gModels.end()) {
            rates = it->second.rates;
            if (!it->second.product.empty()) product = it->second.product;
        }
        d.name = product + (d.serial.empty() ? "" : " " + d.serial) + " (agile, experimental)";
        d.minRateHz = rates.front(); d.maxRateHz = rates.back();
        for (double r : rates) d.rateRanges.emplace_back(r, r);
        d.gainMinDb = 0; d.gainMaxDb = kGainMaxDb;
        d.minFreqHz = kHfMinHz; d.maxFreqHz = kRfMaxHz;   // with the HF inputs; the tuner alone starts at 50 MHz
        d.settings = fobosSettings(fobos().set_clk_source != nullptr);
        list.push_back(d);
    }
    gKnown = list;
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeFobos(const DeviceInfo& d) {
    return std::make_unique<FobosSource>((uint32_t)strtoul(d.nativeArgs.c_str(), nullptr, 10), d.serial);
}

} // namespace native
} // namespace dect2
