// Native driver for the HydraSDR RFOne: libhydrasdr (github.com/hydrasdr/rfone_host, MIT) is loaded at run time when it is installed.
// The RFOne is the Airspy R2's successor by its designers: the same shape of API with the hydrasdr_ prefix, 24 MHz to 1.8 GHz, 10 Msps
// and decimated rates down to 156 ksps, float I/Q straight from the library, a bias tee on its ANT input, the 22-step linearity and
// sensitivity gain tables. The USB id depends on the firmware (1d50:60a1 borrowed from the Airspy before firmware 1.0.2, 38af:0001 since):
// the library's own device registry knows both, so nothing here matches ids. This follows source_airspy.cpp without its parts for
// libairspy's faults: handles are closed normally.
// See native_common.h for what every native driver shares.
#include "native_common.h"
#include <algorithm>
#include <map>

namespace dect2 {
namespace native {

struct HydraApi {
    struct hydrasdr_device;
    struct transfer_t {   // hydrasdr_transfer_t
        hydrasdr_device* device;
        void* ctx;
        void* samples;
        int sample_count;
        uint64_t dropped_samples;
        int sample_type;
    };
    typedef int (DECT2_CALL* cb_t)(transfer_t*);
    enum { SAMPLE_FLOAT32_IQ = 0 };
    int (DECT2_CALL* list_devices)(uint64_t*, int) = nullptr;
    int (DECT2_CALL* open_sn)(hydrasdr_device**, uint64_t) = nullptr;
    int (DECT2_CALL* open)(hydrasdr_device**) = nullptr;
    int (DECT2_CALL* close)(hydrasdr_device*) = nullptr;
    int (DECT2_CALL* get_samplerates)(hydrasdr_device*, uint32_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_samplerate)(hydrasdr_device*, uint32_t) = nullptr;
    int (DECT2_CALL* set_sample_type)(hydrasdr_device*, int) = nullptr;
    int (DECT2_CALL* set_freq)(hydrasdr_device*, uint64_t) = nullptr;   // 64-bit Hz (libairspy takes 32)
    int (DECT2_CALL* set_linearity_gain)(hydrasdr_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_sensitivity_gain)(hydrasdr_device*, uint8_t) = nullptr;   // optional
    int (DECT2_CALL* set_lna_gain)(hydrasdr_device*, uint8_t) = nullptr;           // optional: the three stages by hand (0..15 each)
    int (DECT2_CALL* set_mixer_gain)(hydrasdr_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_vga_gain)(hydrasdr_device*, uint8_t) = nullptr;
    int (DECT2_CALL* set_rf_bias)(hydrasdr_device*, uint8_t) = nullptr;            // optional
    int (DECT2_CALL* set_rf_port)(hydrasdr_device*, int) = nullptr;                // optional: ANT (bias tee), CABLE1, CABLE2
    int (DECT2_CALL* start_rx)(hydrasdr_device*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_rx)(hydrasdr_device*) = nullptr;
    int (DECT2_CALL* is_streaming)(hydrasdr_device*) = nullptr;                    // optional
    const char* (DECT2_CALL* error_name)(int) = nullptr;                           // optional
    int (DECT2_CALL* version_string_read)(hydrasdr_device*, char*, uint8_t) = nullptr;   // optional: the firmware's name and version
    bool ok = false;
    DynLib lib;
    HydraApi() {
        if (nativeDisabled() || !lib.open(libNames("hydrasdr", {".1", ".0"}, {"hydrasdr.dll", "libhydrasdr.dll"}))) return;
        ok = lib.get(open_sn, "hydrasdr_open_sn") && lib.get(open, "hydrasdr_open") && lib.get(close, "hydrasdr_close") &&
             lib.get(get_samplerates, "hydrasdr_get_samplerates") && lib.get(set_samplerate, "hydrasdr_set_samplerate") &&
             lib.get(set_sample_type, "hydrasdr_set_sample_type") && lib.get(set_freq, "hydrasdr_set_freq") &&
             lib.get(set_linearity_gain, "hydrasdr_set_linearity_gain") && lib.get(start_rx, "hydrasdr_start_rx") && lib.get(stop_rx, "hydrasdr_stop_rx");
        lib.opt(list_devices, "hydrasdr_list_devices");
        lib.opt(set_sensitivity_gain, "hydrasdr_set_sensitivity_gain");
        lib.opt(set_lna_gain, "hydrasdr_set_lna_gain");
        lib.opt(set_mixer_gain, "hydrasdr_set_mixer_gain");
        lib.opt(set_vga_gain, "hydrasdr_set_vga_gain");
        lib.opt(set_rf_bias, "hydrasdr_set_rf_bias");
        lib.opt(set_rf_port, "hydrasdr_set_rf_port");
        lib.opt(is_streaming, "hydrasdr_is_streaming");
        lib.opt(error_name, "hydrasdr_error_name");
        lib.opt(version_string_read, "hydrasdr_version_string_read");
    }
};
HydraApi& hydra() { static HydraApi a; return a; }
struct HydraStage { const char* key; const char* label; int (DECT2_CALL* HydraApi::*fn)(HydraApi::hydrasdr_device*, uint8_t); };
const HydraStage kHydraStages[] = {{"gain.lna", "LNA", &HydraApi::set_lna_gain}, {"gain.mixer", "mixer", &HydraApi::set_mixer_gain}, {"gain.vga", "VGA (IF)", &HydraApi::set_vga_gain}};

namespace {
// opening, closing and listing go through the USB stack one at a time; the list is kept for a rescan while the radio is in use
struct HydraModel { std::vector<std::pair<double, double>> rates; double minRate = 0, maxRate = 0; std::string firmware; };
std::mutex gHydraMu;
int gHydraOpen = 0;
std::vector<DeviceInfo> gHydraKnown;
std::map<uint64_t, HydraModel> gHydraModels;

std::string hydraError(int rc) {
    std::string s = "libhydrasdr error " + std::to_string(rc);
    if (hydra().error_name) { const char* n = hydra().error_name(rc); if (n && *n) s += std::string(" ") + n; }
    return s;
}
std::string hydraMsps(double hz) { char b[24]; snprintf(b, sizeof b, "%g", hz / 1e6); return b; }
// the sample rates and the firmware name of a radio, read while it is open for a moment at a listing (under gHydraMu)
void hydraProbe(uint64_t sn, HydraApi::hydrasdr_device* d) {
    HydraModel m;
    uint32_t count = 0;
    if (hydra().get_samplerates(d, &count, 0) == 0 && count > 0) {
        std::vector<uint32_t> rates(std::min<uint32_t>(count, 32));
        if (hydra().get_samplerates(d, rates.data(), (uint32_t)rates.size()) == 0)
            for (uint32_t r : rates) if (r > 0) { m.rates.emplace_back((double)r, (double)r); m.maxRate = std::max(m.maxRate, (double)r); m.minRate = m.minRate > 0 ? std::min(m.minRate, (double)r) : r; }
    }
    if (hydra().version_string_read) {
        char v[128] = {0};
        if (hydra().version_string_read(d, v, (uint8_t)(sizeof v - 1)) == 0) m.firmware = trimmed(v, sizeof v);
    }
    gHydraModels[sn] = m;
}
} // namespace

class HydraSource : public NativeSource {
public:
    explicit HydraSource(uint64_t serial) : serial_(serial) {}
    ~HydraSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        {
            std::lock_guard<std::mutex> lk(gHydraMu);
            const int r = serial_ ? hydra().open_sn(&dev_, serial_) : hydra().open(&dev_);
            if (r != 0 || !dev_) { dev_ = nullptr; err = "HydraSDR: cannot open the radio (in use by another program, or the driver is missing): " + hydraError(r); return false; }
            gHydraOpen++;
        }
        if (hydra().set_sample_type(dev_, HydraApi::SAMPLE_FLOAT32_IQ) != 0) { err = "HydraSDR: the radio does not answer"; return false; }
        if (!configure(s, err, false)) return false;
        if (!startRx()) { err = "HydraSDR: the radio did not start streaming"; return false; }   // reported to the caller, not from the stream thread
        return true;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return false;
        if (!live) {
            // the radio lists its rates (10, 5, 2.5 ... Msps on an RFOne, the decimated ones included): the smallest one that is fast enough
            uint32_t count = 0;
            hydra().get_samplerates(dev_, &count, 0);
            std::vector<uint32_t> rates(std::min<uint32_t>(count, 32));
            if (!rates.empty()) hydra().get_samplerates(dev_, rates.data(), (uint32_t)rates.size());
            std::sort(rates.begin(), rates.end());
            double pick = rates.empty() ? 10e6 : rates.back();
            for (uint32_t r : rates) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            if (const int rc = hydra().set_samplerate(dev_, (uint32_t)pick); rc != 0) {
                err = std::string("HydraSDR: the radio refused ") + hydraMsps(pick) + " Msps (" + hydraError(rc) + ")";
                openFailure_ = OpenFailure::Final;
                return false;
            }
            rate_ = pick;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        // no frequency correction in the library: a correction the user set is taken out of the frequency asked
        freq_ = (uint64_t)std::llround(std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0));
        addNote(err, freqSpecNote("HydraSDR", s.centerHz, 24e6, 1.8e9));
        if (hydra().set_freq(dev_, freq_) != 0) { err = "HydraSDR: the library refused " + std::to_string((long long)std::llround(s.centerHz / 1e3)) + " kHz"; openFailure_ = OpenFailure::Final; return false; }
        // the 0..21 gain steps of the linearity table (the default) or of the sensitivity table (TuneSettings "gainmode")
        const uint8_t g = (uint8_t)std::min(21.0, std::max(0.0, std::round(s.gainDb)));
        if (radioOption(s, "gainmode", "linearity") == "sensitivity" && hydra().set_sensitivity_gain) hydra().set_sensitivity_gain(dev_, g);
        else hydra().set_linearity_gain(dev_, g);
        for (const auto& st : kHydraStages) {   // a stage set by hand (settings "gain.lna" / "gain.mixer" / "gain.vga", -1 = follow the slider)
            auto fn = hydra().*(st.fn);
            const int v = fn ? (int)std::lround(atof(radioOption(s, st.key, "-1").c_str())) : -1;
            if (v >= 0) fn(dev_, (uint8_t)std::min(15, v));
        }
        if (hydra().set_rf_port) {   // the RFOne has three inputs: ANT (the only one with the bias tee), CABLE1 and CABLE2 (TuneSettings "rfport")
            const std::string p = radioOption(s, "rfport", "ant");
            const int port = p == "cable1" ? 1 : p == "cable2" ? 2 : 0;
            if (!live || port != port_) { if (hydra().set_rf_port(dev_, port) == 0) port_ = port; }
        }
        if (hydra().set_rf_bias && (!live || s.biasTee != bias_)) {
            if (hydra().set_rf_bias(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
        }
        return true;
    }
    void closeDevice() override {
        stopRx();
        std::lock_guard<std::mutex> lk(gHydraMu);
        std::lock_guard<std::mutex> lk2(api_);
        if (!dev_) return;
        if (bias_ && hydra().set_rf_bias) hydra().set_rf_bias(dev_, 0);   // antenna power never stays on
        bias_ = false;
        hydra().close(dev_);
        gHydraOpen--;
        dev_ = nullptr;
    }
    void streamLoop() override {
        if (!startRx()) return;   // the first start is in openDevice; this one follows a restart for another sample rate
        // the library stops streaming when a USB transfer fails (the radio unplugged): returning lets the base class open it again
        while (run_ && streaming()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopRx();
    }

private:
    static int DECT2_CALL callback(HydraApi::transfer_t* t) {
        auto* self = static_cast<HydraSource*>(t->ctx);
        if (!self->run_ || !t->samples || t->sample_count <= 0) return 0;
        self->push(static_cast<const cf32*>(t->samples), (size_t)t->sample_count);   // float32 I/Q pairs = std::complex<float>
        return 0;
    }
    bool startRx() {
        std::lock_guard<std::mutex> lk(api_);
        if (!dev_) return false;
        if (rx_) return true;
        run_ = true;   // the callback drops what arrives before this
        if (hydra().start_rx(dev_, &HydraSource::callback, this) == 0) { rx_ = true; return true; }
        run_ = false;
        return false;
    }
    bool streaming() {
        std::lock_guard<std::mutex> lk(api_);
        return !dev_ || !rx_ || !hydra().is_streaming || hydra().is_streaming(dev_) != 0;
    }
    void stopRx() {
        {
            std::lock_guard<std::mutex> lk(api_);
            if (!dev_ || !rx_) return;
            hydra().stop_rx(dev_);   // returns when the library's threads are gone: no callback after this
            rx_ = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the USB stack finishes the cancelled transfers
    }

    uint64_t serial_;
    HydraApi::hydrasdr_device* dev_ = nullptr;
    std::mutex api_;      // every call into the library for this handle
    bool bias_ = false;   // the antenna power is on (api_)
    bool rx_ = false;     // start_rx() done, stop_rx() not yet (api_)
    int port_ = -1;       // the RF input selected (api_)
    uint64_t freq_ = 0;
};

void listHydra(std::vector<DeviceInfo>& out) {
    if (!hydra().ok) return;
    std::vector<uint64_t> serials;
    int found = 0;
    {
        std::lock_guard<std::mutex> lk(gHydraMu);
        if (gHydraOpen > 0 && !gHydraKnown.empty()) {   // the radio is in use: the library could not open it to read its rates
            for (const auto& d : gHydraKnown) out.push_back(d);
            return;
        }
        if (hydra().list_devices) {
            uint64_t sn[16] = {0};
            const int n = hydra().list_devices(sn, 16);
            found = n;
            for (int i = 0; i < n && i < 16; i++) serials.push_back(sn[i]);
            for (uint64_t s : serials) {   // each opened for a moment for its rates and firmware
                HydraApi::hydrasdr_device* d = nullptr;
                if (hydra().open_sn(&d, s) == 0 && d) { hydraProbe(s, d); hydra().close(d); }
            }
        } else {
            HydraApi::hydrasdr_device* d = nullptr;
            if (hydra().open(&d) == 0 && d) { hydraProbe(0, d); hydra().close(d); serials.push_back(0); }
            found = (int)serials.size();
        }
    }
    fprintf(stderr, "HydraSDR: the library lists %d radio(s)%s\n", found, found < 0 ? " (an error)" : "");   // in the log file: did the library see it
    fflush(stderr);
    std::vector<DeviceInfo> list;
    for (uint64_t sn : serials) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "hydrasdr";
        char b[32];
        snprintf(b, sizeof b, "%016llX", (unsigned long long)sn);
        d.serial = sn ? b : "";
        d.nativeArgs = std::to_string((unsigned long long)sn);
        d.name = std::string("HydraSDR RFOne") + (sn ? " " + d.serial.substr(8) : "") + " (native, experimental)";
        d.maxRateHz = 10e6; d.minRateHz = 2.5e6;
        d.gainMinDb = 0; d.gainMaxDb = 21;   // gain table steps
        d.minFreqHz = 24e6; d.maxFreqHz = 1.8e9;
        {
            std::lock_guard<std::mutex> lk(gHydraMu);
            const auto it = gHydraModels.find(sn);
            if (it != gHydraModels.end() && it->second.maxRate > 0) { d.minRateHz = it->second.minRate; d.maxRateHz = it->second.maxRate; d.rateRanges = it->second.rates; }
        }
        d.hasBiasTee = hydra().set_rf_bias != nullptr;
        d.settings = {ppmSetting(0.1)};
        if (hydra().set_rf_port)
            d.settings.push_back(choiceSetting("rfport", "RF input", "Which of the RFOne's three inputs is used. ANT is the one with the bias tee (antenna power).",
                                               {"ant", "cable1", "cable2"}, {"ANT", "CABLE1", "CABLE2"}, "ant"));
        if (hydra().set_sensitivity_gain)
            d.settings.push_back(choiceSetting("gainmode", "Gain mode",
                                               "How the gain steps share out over the LNA, mixer and IF amplifiers (the library's two tables).\n"
                                               "Linearity: strong signals overload later (the default). Sensitivity: more front-end gain for weak signals.",
                                               {"linearity", "sensitivity"}, {"Linearity", "Sensitivity"}, "linearity"));
        for (const auto& st : kHydraStages) {
            if (!(hydra().*(st.fn))) continue;
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
        std::lock_guard<std::mutex> lk(gHydraMu);
        gHydraKnown = list;
    }
    for (const auto& d : list) out.push_back(d);
}

std::unique_ptr<IqSource> makeHydra(const DeviceInfo& d) {
    return std::make_unique<HydraSource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
