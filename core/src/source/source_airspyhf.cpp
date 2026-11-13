// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for the Airspy HF+ and HF+ Discovery: the radio's own library (libairspyhf) is loaded at run time when it is installed. See native_common.h.
// The radio is an HF/VHF receiver with a very narrow output (at most 912 kHz): it suits the narrow modes (DRM, AIS, marine, FM, ...), not TV.
#include "native_common.h"
#include "dect2/source/usb_fd.h"   // Android: radios handed over as descriptors
#include <chrono>
#include <map>
#include <set>

namespace dect2 {
namespace native {

namespace {
constexpr double kMinFreqHz = 0.009e6, kMaxFreqHz = 260e6;
constexpr double kGainMaxDb = 48;   // the attenuator: 0..48 dB in 6 dB steps; the top of the gain slider is 0 dB attenuation
constexpr double kAttStepDb = 6;

std::string rateText(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%g MHz", hz / 1e6);
    else snprintf(b, sizeof b, "%.0f kHz", hz / 1e3);
    return b;
}
}   // namespace

class AirspyHfSource;

struct AirspyHfApi {
    struct airspyhf_device;
    struct transfer_t {   // airspyhf_transfer_t (the samples are float32 I/Q pairs already)
        airspyhf_device* device;
        void* ctx;
        cf32* samples;
        int sample_count;
        uint64_t dropped_samples;
    };
    typedef int (DECT2_CALL* cb_t)(transfer_t*);
    int (DECT2_CALL* list_devices)(uint64_t*, int) = nullptr;
    int (DECT2_CALL* open_sn)(airspyhf_device**, uint64_t) = nullptr;
    int (DECT2_CALL* close)(airspyhf_device*) = nullptr;
    int (DECT2_CALL* get_samplerates)(airspyhf_device*, uint32_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_samplerate)(airspyhf_device*, uint32_t) = nullptr;
    int (DECT2_CALL* set_freq)(airspyhf_device*, uint32_t) = nullptr;
    int (DECT2_CALL* set_hf_agc)(airspyhf_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_hf_att)(airspyhf_device*, uint8_t) = nullptr;
    int (DECT2_CALL* start)(airspyhf_device*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop)(airspyhf_device*) = nullptr;
    int (DECT2_CALL* is_streaming)(airspyhf_device*) = nullptr;
    int (DECT2_CALL* set_hf_lna)(airspyhf_device*, uint8_t) = nullptr;     // optional (not in the oldest libraries)
    int (DECT2_CALL* set_lib_dsp)(airspyhf_device*, uint8_t) = nullptr;    // optional
    // optional: the radio's AGC threshold, the frequency calibration (ppb; the radio's own value from its flash is loaded at open, and
    // setting one only changes it until the radio is closed), antenna power (radios that have it: get_bias_tee_count > 0)
    int (DECT2_CALL* set_hf_agc_threshold)(airspyhf_device*, uint8_t) = nullptr;
    int (DECT2_CALL* get_calibration)(airspyhf_device*, int32_t*) = nullptr;
    int (DECT2_CALL* set_calibration)(airspyhf_device*, int32_t) = nullptr;
    int (DECT2_CALL* set_bias_tee)(airspyhf_device*, int8_t) = nullptr;
    int (DECT2_CALL* get_bias_tee_count)(airspyhf_device*, int32_t*) = nullptr;
    int (DECT2_CALL* open_fd)(airspyhf_device**, int) = nullptr;   // optional (libairspyhf built for Android): a descriptor the system opened
    bool ok = false;
    DynLib lib;
    AirspyHfApi() {
        if (nativeDisabled() || !lib.open(libNames("airspyhf", {".0"}, {"airspyhf.dll", "libairspyhf.dll"}))) return;
        ok = lib.get(list_devices, "airspyhf_list_devices") && lib.get(open_sn, "airspyhf_open_sn") && lib.get(close, "airspyhf_close") &&
             lib.get(get_samplerates, "airspyhf_get_samplerates") && lib.get(set_samplerate, "airspyhf_set_samplerate") &&
             lib.get(set_freq, "airspyhf_set_freq") && lib.get(set_hf_agc, "airspyhf_set_hf_agc") && lib.get(set_hf_att, "airspyhf_set_hf_att") &&
             lib.get(start, "airspyhf_start") && lib.get(stop, "airspyhf_stop") && lib.get(is_streaming, "airspyhf_is_streaming");
        lib.opt(set_hf_lna, "airspyhf_set_hf_lna");
        lib.opt(set_lib_dsp, "airspyhf_set_lib_dsp");
        lib.opt(set_hf_agc_threshold, "airspyhf_set_hf_agc_threshold");
        lib.opt(get_calibration, "airspyhf_get_calibration");
        lib.opt(set_calibration, "airspyhf_set_calibration");
        lib.opt(set_bias_tee, "airspyhf_set_bias_tee");
        lib.opt(get_bias_tee_count, "airspyhf_get_bias_tee_count");
        lib.opt(open_fd, "airspyhf_open_fd");
        if (ok && usbFdPlatform() && !open_fd) {   // Android: a bus scan is not allowed, only the descriptors the app hands over
            fprintf(stderr, "Airspy HF+: the bundled libairspyhf has no airspyhf_open_fd: HF+ radios are not available on this phone\n");
            ok = false;
        }
    }
    // Android: the HF+ the app handed over whose serial is sn (0 = the first one). 0 or a library error.
    int openFromFd(uint64_t sn, airspyhf_device** dev) {
        if (!open_fd) return -1;
        for (const UsbFdRadio& r : usbFdList("airspyhf"))
            if (!sn || usbFdAirspySerial(r.serial) == sn) return open_fd(dev, r.fd);
        return -1;
    }
    // open_sn, or the descriptor on Android
    int openSn(airspyhf_device** dev, uint64_t sn) { return usbFdPlatform() ? openFromFd(sn, dev) : open_sn(dev, sn); }
};
AirspyHfApi& airspyhf() { static AirspyHfApi a; return a; }

namespace {
// libairspyhf gives every handle its own libusb context, and its stop and close are not safe against each other or against a second call
// (crash report from 0.2.2: Windows, an access violation in ntdll under libusb under libairspyhf when Stop was pressed):
// - airspyhf_stop() cancels the USB transfers but waits only 50 us per transfer for them to come back; the cancelled ones are still in
//   flight when it returns. airspyhf_close() then calls airspyhf_stop() again, frees the transfers and calls libusb_exit() on the context
//   while libusb still lists them, and the next walk of that list writes into freed memory.
// - opening, closing and listing go through the computer's USB stack (libusb_init / libusb_exit), and a listing that opens the radio that
//   is streaming in this program fails on Windows (one WinUSB handle per device) and races its context.
// - (issue #78, 0.2.4: the same crash with an HF+ Discovery) the transfers were "collected" by setting the frequency again, but
//   airspyhf_set_freq() goes to the radio only when the frequency changes: nothing reached USB, so the close still met them in flight.
// - airspyhf_list_devices() opens every HF+ on the bus in a context of its own, the one that streams here included.
// So: every open, start, stop, close and listing takes turns on gHfMu; the stream is stopped once, by this driver, and the library is
// asked until it says it has stopped; the samples callback does nothing once a stop is under way; the cancelled transfers are collected
// before the close or the next start with control transfers that always reach the radio (the attenuator as it is); while a radio is
// open here no listing goes to USB at all: it reports what the last one read.
// - (issue #160, 0.2.5/0.2.6: Windows Stop) is_streaming() goes false while cancelled URBs are still in flight, so airspyhf_close()
//   still walks libusb's transfer list after libusb_exit() -> 0xc0000005 in ntdll. On Windows a handle that has streamed is kept open
//   (stopped) and reused, like libairspy (DECT2_AIRSPYHF_KEEP=0 closes it anyway).
bool hfKeepHandles() {
    if (const char* e = getenv("DECT2_AIRSPYHF_KEEP")) return *e && *e != '0';
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
// What the library's callback is given instead of the source itself (#160, #78): a handle parked after Stop can have transfers still in
// flight, so the library may call back after the source is gone (and its ring). The context is never freed (a few bytes per source);
// the source is detached under its lock before it goes, so a callback in progress is waited for and a later one returns at once.
struct HfCbCtx { std::mutex mu; AirspyHfSource* self = nullptr; };
struct HfIdleHandle { AirspyHfApi::airspyhf_device* dev; uint64_t serial; };
std::mutex gHfMu;
std::set<uint64_t> gHfOpen;                 // serials open in this program (including kept idle handles)
std::vector<HfIdleHandle> gHfIdle;          // stopped after a stream, not closed (Windows)
std::map<uint64_t, DeviceInfo> gHfKnown;    // what the last listing read, per serial
std::map<uint64_t, std::chrono::steady_clock::time_point> gHfCooling;   // closed recently: listing must not reopen USB
bool gHfUsed = false;                       // any HF+ was opened here this process (issue #78: list_devices opens every radio)
bool hfListingFrozen() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = gHfCooling.begin(); it != gHfCooling.end(); ) {
        if (it->second <= now) it = gHfCooling.erase(it); else ++it;
    }
    return gHfUsed || !gHfOpen.empty() || !gHfCooling.empty();
}
}   // namespace

class AirspyHfSource : public NativeSource {
public:
    explicit AirspyHfSource(uint64_t serial) : serial_(serial) {}
    ~AirspyHfSource() override { stop(); detachCallback(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!airspyhf().ok) { err = "Airspy HF+: the radio's library (libairspyhf) is missing"; return false; }
        std::lock_guard<std::mutex> g(gHfMu);
        std::lock_guard<std::mutex> lk(api_);
        if (dev_) return configureLocked(s, err, false);   // (not reached: closeDevice() comes first)
        reused_ = false;
        for (size_t i = 0; i < gHfIdle.size(); i++) {
            if (gHfIdle[i].serial == serial_) {   // kept open after Stop on Windows: no second open_sn
                dev_ = gHfIdle[i].dev;
                gHfIdle.erase(gHfIdle.begin() + (long)i);
                reused_ = true;
                streamed_ = true;
                streaming_ = false;
                stopping_ = true;
                break;
            }
        }
        if (!dev_) {
            if (airspyhf().openSn(&dev_, serial_) != 0 || !dev_) { dev_ = nullptr; err = "Airspy HF+: cannot open the radio (in use, or the driver is missing)"; return false; }
            if (airspyhf().set_lib_dsp) airspyhf().set_lib_dsp(dev_, 1);   // the library's own IQ correction and fine tuning
            gHfOpen.insert(serial_);
            gHfUsed = true;
            streamed_ = false;
            freq_ = 0;
            att_ = -1;
        }
        agc_ = radioOption(s, "agc", "off");
        airspyhf().set_hf_agc(dev_, agc_ != "off" ? 1 : 0);             // off: the gain slider is in charge (the default)
        if (agc_ != "off" && airspyhf().set_hf_agc_threshold) airspyhf().set_hf_agc_threshold(dev_, agc_ == "high" ? 1 : 0);
        cal0_ = 0;
        calOk_ = airspyhf().get_calibration && airspyhf().set_calibration && airspyhf().get_calibration(dev_, &cal0_) == 0;
        ppm_ = 0;
        bias_ = false;
        int32_t nb = 0;
        hasBias_ = airspyhf().set_bias_tee && airspyhf().get_bias_tee_count && airspyhf().get_bias_tee_count(dev_, &nb) == 0 && nb > 0;
        // the listing is frozen once an HF+ was opened here (#78): what the radio says at each open keeps that list up to date
        { auto k = gHfKnown.find(serial_); if (k != gHfKnown.end()) k->second.hasBiasTee = hasBias_; }
        if (!reused_) streaming_ = false;
        stopping_ = true;
        return configureLocked(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        return configureLocked(s, err, live);
    }
    bool configureLocked(const TuneSettings& s, std::string& err, bool live) {
        refusedUnchanged_ = false;
        if (!dev_) return false;
        auto& api = airspyhf();
        if (!live) {
            // 912 / 768 / 456 / 384 / 256 / 192 kHz depending on the model: the smallest one that is fast enough
            uint32_t count = 0;
            api.get_samplerates(dev_, &count, 0);
            std::vector<uint32_t> rates(std::min<uint32_t>(count, 16));
            if (!rates.empty()) api.get_samplerates(dev_, rates.data(), (uint32_t)rates.size());
            std::sort(rates.begin(), rates.end());
            if (rates.empty()) { err = "Airspy HF+: the radio reports no sample rates"; return false; }
            const double top = rates.back();
            double pick = top;
            for (uint32_t r : rates) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            if (s.sampleRate > top * 1.001) {
                // a channel that fits in the radio's output (DRM, AIS, a single FM station, ...) is fine at the fastest rate; a wide one (TV, DAB) cannot be received
                if (s.bandwidthMhz * 1e6 > top * 0.8) {
                    err = "Airspy HF+ gives at most " + rateText(top) + "; " + rateText(s.sampleRate) + " needs a faster radio";
                    refusedUnchanged_ = true;   // nothing was sent to the radio: the stream that ran goes on (retune())
                    return false;
                }
                err = "Airspy HF+ runs at " + rateText(top) + " (" + rateText(s.sampleRate) + " was requested; the channel fits)";
            }
            requested_ = s.sampleRate;
            if (api.set_samplerate(dev_, (uint32_t)pick) != 0) { err = "Airspy HF+: the radio does not accept " + rateText(pick); return false; }
            rate_ = pick;
        }
        // the HF+ is specified for 9 kHz..31 MHz and 60..260 MHz (airspy.com): outside that the library is asked all the same, with a note
        if (s.centerHz < kMinFreqHz || s.centerHz > kMaxFreqHz || (s.centerHz > 31e6 && s.centerHz < 60e6))
            addNote(err, "this Airspy HF+ is specified for 9 kHz to 31 MHz and 60 to 260 MHz; the driver may not reach " + rateText(s.centerHz) + " or the signal may be weak or absent");
        // Frequency correction: the library's calibration (parts per billion, applied to the tuned frequency: freq * (1 + ppb / 1e9)) is
        // the radio's factory value plus the user's correction; a clock that runs fast by ppm needs the tuned frequency that much lower.
        // A library without it: the frequency asked for.
        const double ppm = radioPpm(s);
        double tuneHz = s.centerHz;
        if (calOk_) {
            if (ppm != ppm_) {
                if (api.set_calibration(dev_, (int32_t)(cal0_ - std::lround(ppm * 1000))) == 0) ppm_ = ppm;
            }
        } else tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        // asked as is (the library takes 32-bit Hz: only a value beyond 4.29 GHz is cut to what the call can hold)
        freq_ = (uint32_t)std::llround(std::min(std::max(tuneHz, 0.0), 4.29e9));
        if (api.set_freq(dev_, freq_) != 0) { err = "Airspy HF+: the library refused " + rateText(s.centerHz); openFailure_ = OpenFailure::Final; return false; }
        // the radio's own AGC (TuneSettings "agc": off, low or high threshold); off is the default, with the slider in charge
        const std::string agc = radioOption(s, "agc", "off");
        if (agc != agc_) {
            api.set_hf_agc(dev_, agc != "off" ? 1 : 0);
            if (agc != "off" && api.set_hf_agc_threshold) api.set_hf_agc_threshold(dev_, agc == "high" ? 1 : 0);
            agc_ = agc;
        }
        // the gain slider (0..48 dB) -> attenuation in 6 dB steps; the preamp (+6 dB) in the top 6 dB of the slider, or always on / off
        // (TuneSettings "preamp"). The firmware compensates the preamp in its gain chain (hfplus changelog R1.1) and treats the attenuator as the HF gain
        // distribution (R1.3) under its own digital gain, so the output level need not follow the slider: it moves the gain distribution (noise figure against overload), see setAdcLevelCompensated()
        const double g = std::min(std::max(s.gainDb, 0.0), kGainMaxDb);
        att_ = (int)std::lround((kGainMaxDb - g) / kAttStepDb);
        api.set_hf_att(dev_, (uint8_t)att_);
        const std::string pre = radioOption(s, "preamp", "auto");
        if (api.set_hf_lna) api.set_hf_lna(dev_, pre == "on" ? 1 : pre == "off" ? 0 : g > kGainMaxDb - kAttStepDb ? 1 : 0);
        if (hasBias_ && (!live || s.biasTee != bias_)) {
            if (api.set_bias_tee(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
        }
        return true;
    }
    // safe twice, after a failed open, after an unplug, and with the stream still running (it is stopped first)
    void closeDevice() override {
        std::lock_guard<std::mutex> g(gHfMu);
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return;
        stopLocked();
        if (bias_ && hasBias_) airspyhf().set_bias_tee(dev_, 0);   // antenna power never stays on
        bias_ = false;
        collectTransfers();
        detachCallback();   // (a parked handle may still call back: it finds no source)
        // libairspyhf_close() calls libusb_exit while cancelled URBs can still be in flight (Windows 0xc0000005 in ntdll, #78/#160).
        if (streamed_ && hfKeepHandles()) {
            parkHandleLocked();
            return;
        }
        if (streamingLocked()) {
            fprintf(stderr, "Airspy HF+: stream still running after stop, keeping the handle (avoids a libusb crash on close)\n");
            fflush(stderr);
            parkHandleLocked();
            return;
        }
        airspyhf().close(dev_);
        dev_ = nullptr;
        gHfOpen.erase(serial_);
        gHfCooling[serial_] = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        streaming_ = streamed_ = false;
    }
    // a rate refused before anything reached the radio (too wide for it): the radio is as it was, so the stream resumes
    bool resumeAfterConfigureFailure() const override { return refusedUnchanged_; }
    bool streamingLocked() const {   // (under api_) libairspyhf is not re-entrant: retune/configure must not overlap is_streaming
        return dev_ && airspyhf().is_streaming && airspyhf().is_streaming(dev_) != 0;
    }
    void streamLoop() override {
        {
            std::lock_guard<std::mutex> g(gHfMu);
            std::lock_guard<std::mutex> lk(api_);
            if (!dev_ || !run_) return;
            if (streamed_) collectTransfers();   // a restart for another rate: the transfers of the last stream first
            streaming_ = streamed_ = true;       // also when the start fails half way: the library may have threads running, stop() joins them
            stopping_ = false;
            { std::lock_guard<std::mutex> cl(cb_->mu); cb_->self = this; }
            if (airspyhf().start(dev_, &AirspyHfSource::callback, cb_) != 0) { stopLocked(); return; }
        }
        while (run_) {
            bool on = false;
            {
                std::lock_guard<std::mutex> g(gHfMu);
                std::lock_guard<std::mutex> lk(api_);
                on = streamingLocked();
            }
            if (!on) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> g(gHfMu);
        std::lock_guard<std::mutex> lk(api_);
        stopLocked();
    }

private:
    void detachCallback() { std::lock_guard<std::mutex> cl(cb_->mu); cb_->self = nullptr; }   // waits for a callback in progress
    void parkHandleLocked() {   // (under gHfMu and api_) stopped; not closed — serial stays in gHfOpen
        gHfIdle.push_back({dev_, serial_});
        dev_ = nullptr;
        streaming_ = streamed_ = false;
    }
    void stopLocked() {   // (under api_) the library's stop, once per start; returns when the library says the stream is over (at most 1 s)
        stopping_ = true;   // the callback lets go at once
        if (!dev_ || !streaming_) return;
        streaming_ = false;
        airspyhf().stop(dev_);
        for (int i = 0; i < 400 && streamingLocked(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The cancelled USB transfers of a stream that was stopped come back only while libusb handles events, which each control transfer of
    // the library does: a few harmless ones (the attenuator as it is) collect them before the next start or the close. A radio that was
    // unplugged answers these at once with an error.
    void collectTransfers() {
        if (!dev_ || !streamed_) return;
        for (int i = 0; i < 25; i++) {
            if (att_ >= 0) airspyhf().set_hf_att(dev_, (uint8_t)att_);   // (set_freq of the same frequency does not reach the radio)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    static int DECT2_CALL callback(AirspyHfApi::transfer_t* t) {
        if (!t || !t->ctx) return 0;
        auto* cb = static_cast<HfCbCtx*>(t->ctx);
        std::lock_guard<std::mutex> cl(cb->mu);
        AirspyHfSource* self = cb->self;
        if (!self || self->stopping_ || !self->run_ || !t->samples || t->sample_count <= 0) return 0;
        self->push(t->samples, (size_t)t->sample_count);
        return 0;
    }
    uint64_t serial_;
    HfCbCtx* cb_ = new HfCbCtx;    // never freed: see HfCbCtx
    AirspyHfApi::airspyhf_device* dev_ = nullptr;
    std::mutex api_;               // the library is entered by one thread at a time for this radio (retune, the stream thread, stop)
    uint32_t freq_ = 0;            // the frequency the radio is set to
    int att_ = -1;                 // the attenuator step the radio is set to (-1: not yet)
    std::atomic<bool> stopping_{true};   // a stop is under way (or no stream): the samples callback returns at once
    bool streaming_ = false;       // started and not yet stopped
    bool streamed_ = false;        // has streamed since it was opened
    bool reused_ = false;          // this handle was kept open from an earlier session
    std::string agc_ = "off";      // the radio's AGC as set
    int32_t cal0_ = 0;             // the radio's own calibration (ppb), read at open
    bool calOk_ = false;           // the library can read and set it
    double ppm_ = 0;               // the user's correction in the library
    bool hasBias_ = false, bias_ = false;
    bool refusedUnchanged_ = false;   // the last configure() refused a rate before changing anything
};

void listAirspyHf(std::vector<DeviceInfo>& out) {
    if (!airspyhf().ok) return;
    std::lock_guard<std::mutex> g(gHfMu);
    if (hfListingFrozen()) {   // list_devices opens every HF+ (WinUSB one handle): that races stop/close and crashes (#78)
        for (const auto& k : gHfKnown) out.push_back(k.second);
        return;
    }
    uint64_t sn[16] = {0};
    int n = 0;
    if (usbFdPlatform()) {   // Android: the radios the app was given, never a bus scan
        for (const UsbFdRadio& r : usbFdList("airspyhf")) if (n < 16) sn[n++] = usbFdAirspySerial(r.serial);
    } else n = std::min(airspyhf().list_devices(sn, 16), 16);
    std::set<uint64_t> listed;
    for (int i = 0; i < n; i++) listed.insert(sn[i]);
    for (uint64_t s : gHfOpen) {   // a radio streaming here may be missing from the library's list (Windows): it is still there
        if (!listed.count(s) && gHfKnown.count(s)) out.push_back(gHfKnown[s]);
    }
    for (int i = 0; i < n; i++) {
        if (gHfOpen.count(sn[i]) && gHfKnown.count(sn[i])) { out.push_back(gHfKnown[sn[i]]); continue; }   // in use here: not opened again
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "airspyhf";
        char b[32];
        snprintf(b, sizeof b, "%016llX", (unsigned long long)sn[i]);
        d.serial = b;
        d.nativeArgs = std::to_string((unsigned long long)sn[i]);
        d.name = std::string("Airspy HF+ ") + b;
        d.minFreqHz = kMinFreqHz; d.maxFreqHz = kMaxFreqHz;
        d.gainMinDb = 0; d.gainMaxDb = kGainMaxDb;
        // the rates come from the radio (a Discovery has 912 kHz as well); when it is busy (streaming here, or in another program) the standard model's list
        d.minRateHz = 192e3; d.maxRateHz = 768e3;
        AirspyHfApi::airspyhf_device* dev = nullptr;
        if (!gHfOpen.count(sn[i]) && airspyhf().openSn(&dev, sn[i]) == 0 && dev) {
            uint32_t count = 0, rates[16] = {0};
            airspyhf().get_samplerates(dev, &count, 0);
            count = std::min<uint32_t>(count, 16);
            if (count && airspyhf().get_samplerates(dev, rates, count) == 0) {
                d.minRateHz = *std::min_element(rates, rates + count);
                d.maxRateHz = *std::max_element(rates, rates + count);
                for (uint32_t k = 0; k < count; k++) if (rates[k] > 0) d.rateRanges.emplace_back((double)rates[k], (double)rates[k]);
            }
            int32_t nb = 0;   // newer HF+ models power the antenna input; the library says how many switches the radio has
            d.hasBiasTee = airspyhf().set_bias_tee && airspyhf().get_bias_tee_count && airspyhf().get_bias_tee_count(dev, &nb) == 0 && nb > 0;
            airspyhf().close(dev);
        }
        d.settings = {ppmSetting(0.01),
                      choiceSetting("preamp", "Preamp",
                                    "The +6 dB LNA in front of the mixer (libairspyhf: compensated in digital, so it lowers the noise figure, not the level).\n"
                                    "Auto: on in the top 6 dB of the gain slider (the default).",
                                    {"auto", "off", "on"}, {"Auto (with the gain)", "Off", "On"}, "auto"),
                      choiceSetting("agc", "Radio AGC",
                                    "The HF+'s own fast AGC in front of the converter, for strong or fading HF signals. Off: the gain slider (and OnAir's AGC)\n"
                                    "set the attenuator (the default). Low / High: the AGC's threshold; the attenuator is then the radio's and the slider has no effect\n"
                                    "(Airspy's own default is On, high threshold).\n"
                                    "The firmware compensates the preamp and treats the attenuator as its gain distribution: the level in dBFS need not move with the slider,\n"
                                    "which trades noise figure (top: least attenuation) for overload margin. A low dBFS level is normal for this radio. On older\n"
                                    "firmware the attenuator acts only below 31 MHz.",
                                    {"off", "low", "high"}, {"Off", "On, low threshold", "On, high threshold"}, "off")};
        gHfKnown[sn[i]] = d;
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeAirspyHf(const DeviceInfo& d) {
    return std::make_unique<AirspyHfSource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
