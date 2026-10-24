// Native driver for the Airspy R2 / Mini: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include "dect2/source/usb_fd.h"   // Android: radios handed over as descriptors
#include <algorithm>
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
    int (DECT2_CALL* set_lna_gain)(airspy_device*, uint8_t) = nullptr;     // optional: the three stages by hand (0..15 each)
    int (DECT2_CALL* set_mixer_gain)(airspy_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_vga_gain)(airspy_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_sensitivity_gain)(airspy_device*, uint8_t) = nullptr;   // optional (libairspy 1.0.6 and later)
    int (DECT2_CALL* start_rx)(airspy_device*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_rx)(airspy_device*) = nullptr;
    int (DECT2_CALL* is_streaming)(airspy_device*) = nullptr;   // optional
    const char* (DECT2_CALL* error_name)(int) = nullptr;   // optional: libairspy's name of an error code
    int (DECT2_CALL* version_string_read)(airspy_device*, char*, uint8_t) = nullptr;   // optional: tells a Mini ("AirSpy MINI ...") from an R2
    int (DECT2_CALL* open_fd)(airspy_device**, int) = nullptr;   // optional (libairspy 1.0.10+ built for Android): a descriptor the system opened
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
        lib.opt(set_lna_gain, "airspy_set_lna_gain");
        lib.opt(set_mixer_gain, "airspy_set_mixer_gain");
        lib.opt(set_vga_gain, "airspy_set_vga_gain");
        lib.opt(set_sensitivity_gain, "airspy_set_sensitivity_gain");
        lib.opt(is_streaming, "airspy_is_streaming");
        lib.opt(version_string_read, "airspy_version_string_read");
        lib.opt(error_name, "airspy_error_name");
        lib.opt(open_fd, "airspy_open_fd");
        if (ok && usbFdPlatform() && !open_fd) {   // Android without the descriptor entry point: it could only scan the bus, which it may not
            fprintf(stderr, "Airspy: the bundled libairspy has no airspy_open_fd: Airspy radios are not available on this phone\n");
            ok = false;
        }
        if (ok) ok = init() == 0;
    }
};
AirspyApi& airspy() { static AirspyApi a; return a; }
// the three amplifier stages the library lets one set by hand
struct StageFn { const char* key; const char* label; int (DECT2_CALL* AirspyApi::*fn)(AirspyApi::airspy_device*, uint8_t); };
const StageFn kStages[] = {{"gain.lna", "LNA", &AirspyApi::set_lna_gain}, {"gain.mixer", "mixer", &AirspyApi::set_mixer_gain}, {"gain.vga", "VGA (IF)", &AirspyApi::set_vga_gain}};

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
// "libairspy error -2 (AIRSPY_ERROR_INVALID_PARAM)": the code, and the library's own name for it when it has airspy_error_name
std::string airspyError(int rc) {
    std::string s = "libairspy error " + std::to_string(rc);
    if (airspy().error_name) { const char* n = airspy().error_name(rc); if (n && *n) s += std::string(" ") + n; }
    return s;
}
// Android (usb_fd.h): the Airspy the app handed over whose serial is `sn` (0 = the first one), opened through its descriptor. 0 or a libairspy error.
int openFromFd(uint64_t sn, AirspyApi::airspy_device** dev) {
    if (!airspy().open_fd) return -1;
    for (const UsbFdRadio& r : usbFdList("airspy"))   // the USB serial string is "AIRSPY SN:<16 hex>": match on the number
        if (!sn || usbFdAirspySerial(r.serial) == sn) return airspy().open_fd(dev, r.fd);
    return -1;
}
std::string mspsText(double hz) { char b[24]; snprintf(b, sizeof b, "%g", hz / 1e6); return b; }
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
            const std::vector<uint32_t> official = rates;   // what the radio lists
            bool mini = false;
            { std::lock_guard<std::mutex> lk(gDevMu); const auto it = gModels.find(serial_); mini = it != gModels.end() && it->second.mini; }
            // a Mini also runs at 10 Msps, which its firmware takes but does not list (SDR#'s "debug" rate; the radio runs warm): only when asked
            if (mini && radioOption(s, "mini10", "0") == "1" && std::find(rates.begin(), rates.end(), 10000000u) == rates.end()) rates.push_back(10000000u);
            double pick = rates.empty() ? 10e6 : rates.back();
            for (uint32_t r : rates) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            // The firmware decides whether a rate it does not list works (the 10 Msps of a Mini): the library's answer is reported, never ignored.
            // An unofficial rate that is refused falls back to the fastest listed one, so the radio still runs, with the reason in the message.
            std::string refused;
            if (const int rc = airspy().set_samplerate(dev_, (uint32_t)pick); rc != 0) {
                refused = std::string("Airspy: the radio refused ") + mspsText(pick) + " Msps (" + airspyError(rc) + ")";
                const bool listed = std::find(official.begin(), official.end(), (uint32_t)pick) != official.end();
                if (listed || official.empty() || airspy().set_samplerate(dev_, official.back()) != 0) { err = refused; openFailure_ = OpenFailure::Final; return false; }
                pick = official.back();
            }
            rate_ = pick;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            if (!refused.empty()) err = refused + "; running at " + mspsText(rate_) + " Msps instead. Turn off \"10 Msps (unofficial)\" in the radio settings";
            // this Mini can run at 10 Msps, but only with the unofficial rate switched on in the radio settings
            if (!err.empty() && mini && radioOption(s, "mini10", "0") != "1" && s.sampleRate > rate_ && s.sampleRate <= 10.0e6 * 1.001)
                err += ". This Airspy Mini can run at 10 Msps: turn on \"10 Msps (unofficial)\" in the radio settings";
        }
        // libairspy has no frequency correction (the R2 and Mini have a 0.5 ppm TCXO): a correction the user set is taken out of the frequency
        // asked as is (the library takes 32-bit Hz: only a value beyond 4.29 GHz is cut to what the call can hold)
        freq_ = (uint32_t)std::llround(std::min(std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0), 4.29e9));
        addNote(err, freqSpecNote("Airspy", s.centerHz, 24e6, 1.8e9));
        if (airspy().set_freq(dev_, freq_) != 0) { err = "Airspy: the library refused " + std::to_string((long long)std::llround(s.centerHz / 1e3)) + " kHz"; openFailure_ = OpenFailure::Final; return false; }
        // the 0..21 gain steps of the library's linearity table (the default), or of its sensitivity table (TuneSettings "gainmode")
        const uint8_t g = (uint8_t)std::min(21.0, std::max(0.0, std::round(s.gainDb)));
        if (radioOption(s, "gainmode", "linearity") == "sensitivity" && airspy().set_sensitivity_gain) airspy().set_sensitivity_gain(dev_, g);
        else airspy().set_linearity_gain(dev_, g);
        // a stage set by hand (settings "gain.lna" / "gain.mixer" / "gain.vga", -1 = follow the gain slider) is applied after the slider's gain
        for (const auto& st : kStages) {
            auto fn = airspy().*(st.fn);
            const int v = fn ? (int)std::lround(atof(radioOption(s, st.key, "-1").c_str())) : -1;
            if (v >= 0) fn(dev_, (uint8_t)std::min(15, v));
        }
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
                const int r = usbFdPlatform() ? openFromFd(serial_, &dev_)
                                              : serial_ ? airspy().open_sn(&dev_, serial_) : airspy().open(&dev_);
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
        if (usbFdPlatform()) {   // Android: the radios the app was given (no bus scan); each opened for a moment for its rates and model
            for (const UsbFdRadio& r : usbFdList("airspy")) {
                const uint64_t sn = usbFdAirspySerial(r.serial);
                serials.push_back(sn);
                AirspyApi::airspy_device* d = nullptr;
                if (gOpenHandles == 0 && airspy().open_fd(&d, r.fd) == 0 && d) { probe(sn, d); airspy().close(d); gLastClose = SteadyClock::now(); }
            }
            found = (int)serials.size();
        } else if (airspy().list_devices) {
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
                if (it->second.mini) {
                    d.maxFreqHz = 1.7e9;
                    if (d.maxRateHz < 10e6) { d.rateRanges.emplace_back(10e6, 10e6); d.maxRateHz = 10e6; }   // with "10 Msps (unofficial)" on, see configure
                }
            }
        }
        d.hasBiasTee = airspy().set_rf_bias != nullptr;
        d.settings = {ppmSetting(0.1)};
        {   std::lock_guard<std::mutex> lk(gDevMu);
            const auto it = gModels.find(sn);
            if (it != gModels.end() && it->second.mini)
                d.settings.push_back(boolSetting("mini10", "10 Msps (unofficial)",
                    "The Airspy Mini also runs at 10 Msps, as SDR#'s debug mode offers: enough for an 8 MHz DVB-T/T2 channel.\n"
                    "Airspy does not list it: the radio runs warm and may drop samples, so use it for short sessions. Without it a Mini runs at 6 Msps at most.",
                    false, true));
        }
        if (airspy().set_sensitivity_gain)
            d.settings.push_back(choiceSetting("gainmode", "Gain mode",
                                               "How the gain steps share out over the LNA, mixer and IF amplifiers (libairspy's two tables).\n"
                                               "Linearity: strong signals overload later (the default). Sensitivity: more front-end gain for weak signals.",
                                               {"linearity", "sensitivity"}, {"Linearity", "Sensitivity"}, "linearity"));
        for (const auto& st : kStages) {
            if (!(airspy().*(st.fn))) continue;
            RadioSetting r;
            r.type = RadioSetting::Number; r.key = st.key; r.label = std::string("Gain stage: ") + st.label;
            r.help = std::string("The ") + st.label + " amplifier by hand, 0 to 15 (the radio's own steps). -1: follow the gain slider (the default). "
                     "Set by hand it replaces what the slider gives this stage; the other stages still follow the slider. Takes effect on the next gain change or retune.";
            r.minV = -1; r.maxV = 15; r.step = 1; r.def = "-1";
            d.settings.push_back(r);
        }
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
