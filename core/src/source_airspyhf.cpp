// Native driver for the Airspy HF+ and HF+ Discovery: the radio's own library (libairspyhf) is loaded at run time when it is installed. See native_common.h.
// The radio is an HF/VHF receiver with a very narrow output (at most 912 kHz): it suits the narrow modes (DRM, AIS, marine, FM, ...), not TV.
#include "native_common.h"

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
    }
};
AirspyHfApi& airspyhf() { static AirspyHfApi a; return a; }

class AirspyHfSource : public NativeSource {
public:
    explicit AirspyHfSource(uint64_t serial) : serial_(serial) {}
    ~AirspyHfSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!airspyhf().ok) { err = "Airspy HF+: the radio's library (libairspyhf) is missing"; return false; }
        if (airspyhf().open_sn(&dev_, serial_) != 0 || !dev_) { dev_ = nullptr; err = "Airspy HF+: cannot open the radio (in use, or the driver is missing)"; return false; }
        if (airspyhf().set_lib_dsp) airspyhf().set_lib_dsp(dev_, 1);   // the library's own IQ correction and fine tuning
        agc_ = radioOption(s, "agc", "off");
        airspyhf().set_hf_agc(dev_, agc_ != "off" ? 1 : 0);             // off: the gain slider is in charge (the default)
        if (agc_ != "off" && airspyhf().set_hf_agc_threshold) airspyhf().set_hf_agc_threshold(dev_, agc_ == "high" ? 1 : 0);
        cal0_ = 0;
        calOk_ = airspyhf().get_calibration && airspyhf().set_calibration && airspyhf().get_calibration(dev_, &cal0_) == 0;
        ppm_ = 0;
        bias_ = false;
        int32_t nb = 0;
        hasBias_ = airspyhf().set_bias_tee && airspyhf().get_bias_tee_count && airspyhf().get_bias_tee_count(dev_, &nb) == 0 && nb > 0;
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
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
            requested_ = s.sampleRate;
            double pick = top;
            for (uint32_t r : rates) if (r >= s.sampleRate * 0.999) { pick = r; break; }
            if (s.sampleRate > top * 1.001) {
                // a channel that fits in the radio's output (DRM, AIS, a single FM station, ...) is fine at the fastest rate; a wide one (TV, DAB) cannot be received
                if (s.bandwidthMhz * 1e6 > top * 0.8) {
                    err = "Airspy HF+ gives at most " + rateText(top) + "; " + rateText(s.sampleRate) + " needs a faster radio";
                    return false;
                }
                err = "Airspy HF+ runs at " + rateText(top) + " (" + rateText(s.sampleRate) + " was requested; the channel fits)";
            }
            if (api.set_samplerate(dev_, (uint32_t)pick) != 0) { err = "Airspy HF+: the radio does not accept " + rateText(pick); return false; }
            rate_ = pick;
        }
        // the HF+ tunes 9 kHz..31 MHz and 60..260 MHz (airspy.com): say so instead of silently listening somewhere else
        if ((s.centerHz < kMinFreqHz || s.centerHz > kMaxFreqHz || (s.centerHz > 31e6 && s.centerHz < 60e6)) && err.empty())
            err = "Airspy HF+ tunes 9 kHz to 31 MHz and 60 to 260 MHz; " + rateText(s.centerHz) + " is outside that";
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
        api.set_freq(dev_, (uint32_t)std::llround(std::min(std::max(tuneHz, kMinFreqHz), kMaxFreqHz)));
        // the radio's own AGC (TuneSettings "agc": off, low or high threshold); off is the default, with the slider in charge
        const std::string agc = radioOption(s, "agc", "off");
        if (agc != agc_) {
            api.set_hf_agc(dev_, agc != "off" ? 1 : 0);
            if (agc != "off" && api.set_hf_agc_threshold) api.set_hf_agc_threshold(dev_, agc == "high" ? 1 : 0);
            agc_ = agc;
        }
        // the gain slider (0..48 dB, more is louder) -> attenuation in 6 dB steps; the preamp (+6 dB) in the top 6 dB of the slider, or always
        // on / off (TuneSettings "preamp")
        const double g = std::min(std::max(s.gainDb, 0.0), kGainMaxDb);
        api.set_hf_att(dev_, (uint8_t)std::lround((kGainMaxDb - g) / kAttStepDb));
        const std::string pre = radioOption(s, "preamp", "auto");
        if (api.set_hf_lna) api.set_hf_lna(dev_, pre == "on" ? 1 : pre == "off" ? 0 : g > kGainMaxDb - kAttStepDb ? 1 : 0);
        if (hasBias_ && (!live || s.biasTee != bias_)) {
            if (api.set_bias_tee(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
        }
        return true;
    }
    void closeDevice() override {
        if (dev_ && bias_ && hasBias_) airspyhf().set_bias_tee(dev_, 0);   // antenna power never stays on
        bias_ = false;
        if (dev_) airspyhf().close(dev_);
        dev_ = nullptr;
    }
    void streamLoop() override {
        if (airspyhf().start(dev_, &AirspyHfSource::callback, this) != 0) return;
        while (run_ && airspyhf().is_streaming(dev_)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        airspyhf().stop(dev_);
    }

private:
    static int DECT2_CALL callback(AirspyHfApi::transfer_t* t) {
        auto* self = static_cast<AirspyHfSource*>(t->ctx);
        if (!self->run_ || !t->samples || t->sample_count <= 0) return 0;
        self->push(t->samples, (size_t)t->sample_count);
        return 0;
    }
    uint64_t serial_;
    AirspyHfApi::airspyhf_device* dev_ = nullptr;
    std::string agc_ = "off";      // the radio's AGC as set
    int32_t cal0_ = 0;             // the radio's own calibration (ppb), read at open
    bool calOk_ = false;           // the library can read and set it
    double ppm_ = 0;               // the user's correction in the library
    bool hasBias_ = false, bias_ = false;
};

void listAirspyHf(std::vector<DeviceInfo>& out) {
    if (!airspyhf().ok) return;
    uint64_t sn[16] = {0};
    const int n = airspyhf().list_devices(sn, 16);
    for (int i = 0; i < n && i < 16; i++) {
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
        if (airspyhf().open_sn(&dev, sn[i]) == 0 && dev) {
            uint32_t count = 0, rates[16] = {0};
            airspyhf().get_samplerates(dev, &count, 0);
            count = std::min<uint32_t>(count, 16);
            if (count && airspyhf().get_samplerates(dev, rates, count) == 0) {
                d.minRateHz = *std::min_element(rates, rates + count);
                d.maxRateHz = *std::max_element(rates, rates + count);
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
                                    "set the attenuator (the default). Low / High: the AGC's threshold; the slider then has little effect.",
                                    {"off", "low", "high"}, {"Off", "On, low threshold", "On, high threshold"}, "off")};
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeAirspyHf(const DeviceInfo& d) {
    return std::make_unique<AirspyHfSource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
