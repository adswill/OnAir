// Native driver for the Airspy R2 / Mini: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include <map>

namespace dect2 {
namespace native {


struct AirspyApi {
    struct airspy_device;
    struct transfer_t {   // airspy_transfer_t
        airspy_device* device;
        void* ctx;
        void* samples;
        int sample_count;
        uint64_t dropped_samples;
        int sample_type;
    };
    typedef int (DECT2_CALL* cb_t)(transfer_t*);
    enum { SAMPLE_FLOAT32_IQ = 0 };
    int (DECT2_CALL* init)(void) = nullptr;
    int (DECT2_CALL* list_devices)(uint64_t*, int) = nullptr;   // optional (newer libraries)
    int (DECT2_CALL* open_sn)(airspy_device**, uint64_t) = nullptr;
    int (DECT2_CALL* open)(airspy_device**) = nullptr;
    int (DECT2_CALL* close)(airspy_device*) = nullptr;
    int (DECT2_CALL* get_samplerates)(airspy_device*, uint32_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_samplerate)(airspy_device*, uint32_t) = nullptr;
    int (DECT2_CALL* set_sample_type)(airspy_device*, int) = nullptr;
    int (DECT2_CALL* set_freq)(airspy_device*, uint32_t) = nullptr;
    int (DECT2_CALL* set_linearity_gain)(airspy_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_rf_bias)(airspy_device*, uint8_t) = nullptr;   // optional
    int (DECT2_CALL* set_sensitivity_gain)(airspy_device*, uint8_t) = nullptr;   // optional (libairspy 1.0.6 and later)
    int (DECT2_CALL* start_rx)(airspy_device*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_rx)(airspy_device*) = nullptr;
    int (DECT2_CALL* is_streaming)(airspy_device*) = nullptr;   // optional
    int (DECT2_CALL* version_string_read)(airspy_device*, char*, uint8_t) = nullptr;   // optional: tells a Mini ("AirSpy MINI ...") from an R2
    bool ok = false;
    DynLib lib;
    AirspyApi() {
        if (nativeDisabled() || !lib.open(libNames("airspy", {".0"}, {"airspy.dll", "libairspy.dll"}))) return;
        ok = lib.get(init, "airspy_init") && lib.get(open_sn, "airspy_open_sn") && lib.get(open, "airspy_open") && lib.get(close, "airspy_close") &&
             lib.get(get_samplerates, "airspy_get_samplerates") && lib.get(set_samplerate, "airspy_set_samplerate") &&
             lib.get(set_sample_type, "airspy_set_sample_type") && lib.get(set_freq, "airspy_set_freq") &&
             lib.get(set_linearity_gain, "airspy_set_linearity_gain") && lib.get(start_rx, "airspy_start_rx") && lib.get(stop_rx, "airspy_stop_rx");
        lib.opt(list_devices, "airspy_list_devices");   // optional: get() would log "radio disabled" for an older library that works
        lib.opt(set_rf_bias, "airspy_set_rf_bias");
        lib.opt(set_sensitivity_gain, "airspy_set_sensitivity_gain");
        lib.opt(is_streaming, "airspy_is_streaming");
        lib.opt(version_string_read, "airspy_version_string_read");
        if (ok) ok = init() == 0;
    }
};
AirspyApi& airspy() { static AirspyApi a; return a; }

namespace {
using SteadyClock = std::chrono::steady_clock;

// libairspy 1.0.10 closes in the wrong order: airspy_close() ends its libusb context (airspy_open_exit) and only then frees its USB transfers
// (free_transfers). Every transfer that was submitted holds a reference to the USB device, so the last libusb_free_transfer() releases the device
// after its context is gone. libusb 1.0.27 on Windows has no hotplug support, and there the final release calls usbi_disconnect_device() with a
// null context: OnAir crashed in airspy_close() the first time a radio that had streamed was stopped (the receiver stopped for a scan, or the Stop
// button). Linux and macOS have hotplug in libusb and are not affected.
// So a handle that has streamed is not closed on Windows: it is kept open, stopped, and the next open of the same radio takes it back. The radio
// stays claimed by OnAir until the program ends. DECT2_AIRSPY_KEEP=0 closes it anyway (an airspy.dll built with the two calls in the right
// order, see tools/package/patches), =1 keeps handles on other systems (tests).
bool keepHandles() {
    if (const char* e = getenv("DECT2_AIRSPY_KEEP")) return *e && *e != '0';
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

// What is shared by every Airspy handle of the program (the receiver, then the scanner opening the same radio, the device list):
// - the library is not meant to be entered by several threads on one radio, and opening, closing and listing all go through the USB stack of
//   the computer, so they take turns
// - a radio that is open cannot be opened again, and on Windows (one WinUSB handle per device) airspy_list_devices cannot look at it either,
//   so the list of the radios found is kept: a rescan while one is in use then still shows it
// - when the radio was closed a moment ago the system may still be letting go of it: an open that fails right after a close is repeated
struct IdleHandle { AirspyApi::airspy_device* dev; uint64_t serial; };
std::mutex gDevMu;
int gOpenHandles = 0;              // handles that exist: in use, kept open, or given up (never closed)
std::vector<IdleHandle> gIdle;     // kept open, stopped, waiting for the next open of the same radio
std::vector<DeviceInfo> gKnown;
SteadyClock::time_point gLastClose = SteadyClock::now() - std::chrono::hours(1);
// per serial: the sample rates and whether it is a Mini (its firmware version string says "AirSpy MINI"), read at a listing (under gDevMu)
struct AirspyModel { std::vector<std::pair<double, double>> rates; double minRate = 0, maxRate = 0; bool mini = false; };
std::map<uint64_t, AirspyModel> gModels;
void probe(uint64_t sn, AirspyApi::airspy_device* d) {
    AirspyModel m;
    uint32_t count = 0;
    if (airspy().get_samplerates(d, &count, 0) == 0 && count > 0) {
        std::vector<uint32_t> rates(std::min<uint32_t>(count, 16));
        if (airspy().get_samplerates(d, rates.data(), (uint32_t)rates.size()) == 0) {
            for (uint32_t r : rates) if (r > 0) { m.rates.emplace_back((double)r, (double)r); m.maxRate = std::max(m.maxRate, (double)r); m.minRate = m.minRate > 0 ? std::min(m.minRate, (double)r) : r; }
        }
    }
    if (airspy().version_string_read) {
        char v[128] = {0};
        if (airspy().version_string_read(d, v, (uint8_t)(sizeof v - 1)) == 0) {
            std::string s(v, strnlen(v, sizeof v));
            for (auto& c : s) c = (char)toupper((unsigned char)c);
            m.mini = s.find("MINI") != std::string::npos;
        }
    }
    gModels[sn] = m;
}
} // namespace

class AirspySource : public NativeSource {
public:
    explicit AirspySource(uint64_t serial) : serial_(serial) {}
    ~AirspySource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!openHandle()) { err = "Airspy: cannot open the radio (in use by another program, or the driver is missing)"; return false; }
        if (airspy().set_sample_type(dev_, AirspyApi::SAMPLE_FLOAT32_IQ) != 0 && reused_) {
            // a handle that was kept open and the radio is gone (unplugged): it cannot be closed safely either, and it stays claimed until the program ends
            dev_ = nullptr;
            err = "Airspy: the radio does not answer (unplugged?). Plug it in again; if it stays away, restart OnAir";
            return false;
        }
        if (!configure(s, err, false)) return false;
        // Streaming starts here, not on the stream thread: the first retune after start() must not meet a radio that is still being started,
        // and a radio that does not start is reported to the caller instead of sending nothing.
        if (!startRx()) { err = "Airspy: the radio did not start streaming"; return false; }
        return true;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return false;
        if (!live) {
            // the radio offers a short list (10 / 2.5 Msps on an R2, 6 / 3 Msps on a Mini): the smallest one that is fast enough
            uint32_t count = 0;
            airspy().get_samplerates(dev_, &count, 0);
            std::vector<uint32_t> rates(std::min<uint32_t>(count, 16));
            if (!rates.empty()) airspy().get_samplerates(dev_, rates.data(), (uint32_t)rates.size());
            std::sort(rates.begin(), rates.end());
            double pick = rates.empty() ? 10e6 : rates.back();
            for (uint32_t r : rates) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            airspy().set_samplerate(dev_, (uint32_t)pick);
            rate_ = pick;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        // libairspy has no frequency correction (the R2 and Mini have a 0.5 ppm TCXO): a correction the user set is taken out of the frequency
        freq_ = (uint32_t)std::llround(std::min(std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0), 4.29e9));
        airspy().set_freq(dev_, freq_);
        // the 0..21 gain steps of the library's linearity table (the default), or of its sensitivity table (TuneSettings "gainmode")
        const uint8_t g = (uint8_t)std::min(21.0, std::max(0.0, std::round(s.gainDb)));
        if (radioOption(s, "gainmode", "linearity") == "sensitivity" && airspy().set_sensitivity_gain) airspy().set_sensitivity_gain(dev_, g);
        else airspy().set_linearity_gain(dev_, g);
        if (airspy().set_rf_bias && (!live || s.biasTee != bias_)) {
            if (airspy().set_rf_bias(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
        }
        return true;
    }
    void closeDevice() override {
        stopRx();
        std::lock_guard<std::mutex> lk(gDevMu);
        std::lock_guard<std::mutex> lk2(api_);
        if (!dev_) return;
        if (bias_ && airspy().set_rf_bias && !bad_) airspy().set_rf_bias(dev_, 0);   // antenna power never stays on, also for a handle that is kept open
        bias_ = false;
        if (bad_) {   // a start that failed half way leaves the library in a state only airspy_close() can clear, and that is the call that crashes
            fprintf(stderr, "Airspy: the radio is given up (not closed: closing a handle that streamed can crash the library)\n");
            fflush(stderr);
        } else if (streamed_ && keepHandles()) {
            gIdle.push_back({dev_, serial_});
        } else {
            airspy().close(dev_);
            gOpenHandles--;
            gLastClose = SteadyClock::now();
        }
        dev_ = nullptr;
        streamed_ = reused_ = bad_ = false;
    }
    void streamLoop() override {
        if (!startRx()) return;   // the first start is in openDevice; this one follows a restart for another sample rate
        // libairspy stops streaming when a USB transfer fails (the radio unplugged): returning then lets the base class open it again
        while (run_ && streaming()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopRx();
    }

private:
    static int DECT2_CALL callback(AirspyApi::transfer_t* t) {
        auto* self = static_cast<AirspySource*>(t->ctx);
        if (!self->run_ || !t->samples || t->sample_count <= 0) return 0;
        self->push(static_cast<const cf32*>(t->samples), (size_t)t->sample_count);   // float32 I/Q pairs = std::complex<float>
        return 0;
    }

    bool openHandle() {
        for (int attempt = 0;; attempt++) {
            {
                std::lock_guard<std::mutex> lk(gDevMu);
                for (size_t i = 0; i < gIdle.size(); i++)
                    if (gIdle[i].serial == serial_) {   // the handle of an earlier session
                        dev_ = gIdle[i].dev;
                        gIdle.erase(gIdle.begin() + (long)i);
                        streamed_ = reused_ = true;
                        return true;
                    }
                const int r = serial_ ? airspy().open_sn(&dev_, serial_) : airspy().open(&dev_);
                if (r == 0 && dev_) { gOpenHandles++; return true; }
                dev_ = nullptr;
                // only just closed (the receiver was stopped for the scanner): give the system a moment to release the radio
                if (attempt >= 8 || SteadyClock::now() - gLastClose > std::chrono::seconds(4)) return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }
    bool startRx() {
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return false;
        if (rx_) return true;
        // A handle that streamed before still has its cancelled USB transfers waiting to be collected, and the library cannot start again until
        // they are: the control transfers of a few harmless requests collect them.
        if (streamed_) for (int i = 0; i < 3; i++) { airspy().set_freq(dev_, freq_); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        streamed_ = true;
        run_ = true;   // the callback drops what arrives before this
        // One try: a start that fails leaves the library believing it streams, and its stop_rx() then joins threads that were never made
        if (airspy().start_rx(dev_, &AirspySource::callback, this) == 0) { rx_ = true; return true; }
        run_ = false;
        bad_ = true;
        return false;
    }
    bool streaming() {
        std::lock_guard<std::mutex> lk(api_);
        return !dev_ || !rx_ || !airspy().is_streaming || airspy().is_streaming(dev_) != 0;
    }
    void stopRx() {
        {
            std::lock_guard<std::mutex> lk(api_);
            if (!dev_ || !rx_) return;
            airspy().stop_rx(dev_);   // returns when the library's threads are gone: no callback after this
            rx_ = false;
        }
        // the library cancels its USB transfers and the USB stack finishes them in the background: this gives it time
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    uint64_t serial_;
    AirspyApi::airspy_device* dev_ = nullptr;
    std::mutex api_;      // every call into the library for this handle
    bool bias_ = false;   // the antenna power is on (api_)
    bool rx_ = false;     // start_rx() done, stop_rx() not yet (under api_)
    bool streamed_ = false, reused_ = false, bad_ = false;   // start_rx() was tried on this handle (ever); it was kept from an earlier session; its start failed
    uint32_t freq_ = 0;
};

void listAirspy(std::vector<DeviceInfo>& out) {
    if (!airspy().ok) return;
    {
        std::lock_guard<std::mutex> lk(gDevMu);
        if (gOpenHandles > 0 && !gKnown.empty()) {   // the radio is in use: the library could not open it to read its serial number
            for (const auto& d : gKnown) out.push_back(d);
            return;
        }
    }
    std::vector<uint64_t> serials;
    int found = 0;
    {
        std::lock_guard<std::mutex> lk(gDevMu);
        if (airspy().list_devices) {
            uint64_t sn[16] = {0};
            const int n = airspy().list_devices(sn, 16);
            found = n;
            for (int i = 0; i < n && i < 16; i++) serials.push_back(sn[i]);
        } else {   // older library: is there at least one?
            AirspyApi::airspy_device* d = nullptr;
            if (airspy().open(&d) == 0 && d) { probe(0, d); airspy().close(d); gLastClose = SteadyClock::now(); serials.push_back(0); }
            found = (int)serials.size();
        }
        // The sample rates and the model come from the radio (an R2: 10 / 2.5 Msps to 1.8 GHz; a Mini: 6 / 3 Msps, 10 with newer firmware, to
        // 1.7 GHz): each one is opened for a moment, as the HF+ driver does. A handle that never streamed closes safely on Windows too (only
        // a handle that streamed is kept, see keepHandles). A radio that cannot be opened (another program has it) keeps what was read last.
        if (airspy().list_devices && gOpenHandles == 0)
            for (uint64_t sn : serials) {
                AirspyApi::airspy_device* d = nullptr;
                if (airspy().open_sn(&d, sn) == 0 && d) { probe(sn, d); airspy().close(d); gLastClose = SteadyClock::now(); }
            }
    }
    // in the log file as well: when a radio does not show up this says whether the library saw it (a negative number is a library error)
    fprintf(stderr, "Airspy: the library lists %d radio(s)%s\n", found, found < 0 ? " (an error)" : "");
    fflush(stderr);
    std::vector<DeviceInfo> list;
    for (uint64_t sn : serials) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "airspy";
        char b[32];
        snprintf(b, sizeof b, "%016llX", (unsigned long long)sn);
        d.serial = sn ? b : "";
        d.nativeArgs = std::to_string((unsigned long long)sn);
        d.name = std::string("Airspy") + (sn ? " " + d.serial.substr(8) : "") + " (native, experimental)";
        d.maxRateHz = 10e6; d.minRateHz = 2.5e6;
        d.gainMinDb = 0; d.gainMaxDb = 21;   // linearity gain steps
        d.minFreqHz = 24e6; d.maxFreqHz = 1.8e9;   // the R820T tuner: 24 MHz to 1.8 GHz (airspy.com; the Mini to 1.7 GHz)
        {   // what the radio said when it was last opened (probe)
            std::lock_guard<std::mutex> lk(gDevMu);
            const auto it = gModels.find(sn);
            if (it != gModels.end()) {
                if (it->second.maxRate > 0) { d.minRateHz = it->second.minRate; d.maxRateHz = it->second.maxRate; d.rateRanges = it->second.rates; }
                if (it->second.mini) d.maxFreqHz = 1.7e9;
            }
        }
        d.hasBiasTee = airspy().set_rf_bias != nullptr;
        d.settings = {ppmSetting(0.1)};
        if (airspy().set_sensitivity_gain)
            d.settings.push_back(choiceSetting("gainmode", "Gain mode",
                                               "How the gain steps share out over the LNA, mixer and IF amplifiers (libairspy's two tables).\n"
                                               "Linearity: strong signals overload later (the default). Sensitivity: more front-end gain for weak signals.",
                                               {"linearity", "sensitivity"}, {"Linearity", "Sensitivity"}, "linearity"));
        list.push_back(d);
    }
    {
        std::lock_guard<std::mutex> lk(gDevMu);
        gKnown = list;
    }
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeAirspy(const DeviceInfo& d) {
    return std::make_unique<AirspySource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
