// Native driver for the LimeSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include <map>

namespace dect2 {
namespace native {


struct LimeApi {
    typedef void lms_device_t;
    typedef char info_str_t[256];
    typedef char name_t[16];
    struct range_t { double min, max, step; };   // lms_range_t
    struct stream_t {                            // lms_stream_t
        size_t handle;
        bool isTx;
        uint32_t channel;
        uint32_t fifoSize;
        float throughputVsLatency;
        int dataFmt;   // LMS_FMT_F32 = 0
        int linkFmt;   // LMS_LINK_FMT_DEFAULT = 0
    };
    struct meta_t { uint64_t timestamp; bool waitForTimestamp; bool flushPartialPacket; };   // lms_stream_meta_t
    int (DECT2_CALL* get_device_list)(info_str_t*) = nullptr;
    int (DECT2_CALL* open)(lms_device_t**, const char*, void*) = nullptr;
    int (DECT2_CALL* close)(lms_device_t*) = nullptr;
    int (DECT2_CALL* init)(lms_device_t*) = nullptr;
    int (DECT2_CALL* enable_channel)(lms_device_t*, bool, size_t, bool) = nullptr;
    int (DECT2_CALL* set_sample_rate)(lms_device_t*, double, size_t) = nullptr;
    int (DECT2_CALL* get_sample_rate_range)(lms_device_t*, bool, range_t*) = nullptr;
    int (DECT2_CALL* set_lo_frequency)(lms_device_t*, bool, size_t, double) = nullptr;
    int (DECT2_CALL* get_antenna_list)(lms_device_t*, bool, size_t, name_t*) = nullptr;
    int (DECT2_CALL* set_antenna)(lms_device_t*, bool, size_t, size_t) = nullptr;
    int (DECT2_CALL* set_lpf_bw)(lms_device_t*, bool, size_t, double) = nullptr;
    int (DECT2_CALL* set_gain_db)(lms_device_t*, bool, size_t, unsigned) = nullptr;
    int (DECT2_CALL* calibrate)(lms_device_t*, bool, size_t, double, unsigned) = nullptr;
    int (DECT2_CALL* setup_stream)(lms_device_t*, stream_t*) = nullptr;
    int (DECT2_CALL* destroy_stream)(lms_device_t*, stream_t*) = nullptr;
    int (DECT2_CALL* start_stream)(stream_t*) = nullptr;
    int (DECT2_CALL* stop_stream)(stream_t*) = nullptr;
    int (DECT2_CALL* recv_stream)(stream_t*, void*, size_t, meta_t*, unsigned) = nullptr;
    int (DECT2_CALL* get_clock_freq)(lms_device_t*, size_t, double*) = nullptr;   // optional
    const char* (DECT2_CALL* last_error)(void) = nullptr;                          // optional
    int (DECT2_CALL* get_lo_range)(lms_device_t*, bool, range_t*) = nullptr;      // optional
    int (DECT2_CALL* get_sample_rate)(lms_device_t*, bool, size_t, double*, double*) = nullptr;   // optional
    bool ok = false;
    DynLib lib;
    LimeApi() {
        if (nativeDisabled() || !lib.open(libNames("LimeSuite", {".23.11-1", ".22.09-1", ".20.10-1", ".20.01-1", ".19.04-1", ".18.06-1", ""}, {"libLimeSuite.dll", "LimeSuite.dll"}))) return;
        ok = lib.get(get_device_list, "LMS_GetDeviceList") && lib.get(open, "LMS_Open") && lib.get(close, "LMS_Close") && lib.get(init, "LMS_Init") &&
             lib.get(enable_channel, "LMS_EnableChannel") && lib.get(set_sample_rate, "LMS_SetSampleRate") &&
             lib.get(get_sample_rate_range, "LMS_GetSampleRateRange") && lib.get(set_lo_frequency, "LMS_SetLOFrequency") &&
             lib.get(get_antenna_list, "LMS_GetAntennaList") && lib.get(set_antenna, "LMS_SetAntenna") && lib.get(set_lpf_bw, "LMS_SetLPFBW") &&
             lib.get(set_gain_db, "LMS_SetGaindB") && lib.get(calibrate, "LMS_Calibrate") && lib.get(setup_stream, "LMS_SetupStream") &&
             lib.get(destroy_stream, "LMS_DestroyStream") && lib.get(start_stream, "LMS_StartStream") && lib.get(stop_stream, "LMS_StopStream") &&
             lib.get(recv_stream, "LMS_RecvStream");
        if (ok) { lib.opt(get_clock_freq, "LMS_GetClockFreq"); lib.opt(last_error, "LMS_GetLastErrorMessage"); lib.opt(get_lo_range, "LMS_GetLOFrequencyRange"); lib.opt(get_sample_rate, "LMS_GetSampleRate"); }
    }
    std::string text() const {   // LimeSuite's own words for the last failure
        const char* t = last_error ? last_error() : nullptr;
        return t && *t ? std::string(": ") + t : std::string();
    }
};
LimeApi& lime() { static LimeApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

// LMS_GetLOFrequencyRange needs an open radio: what each radio said when it was opened is kept for the list
std::mutex gRangeMu;
std::map<std::string, std::pair<double, double>> gRange;

} // namespace

class LimeSource : public NativeSource {
public:
    // args: the LMS_Open() string, then "#port=" and an input name when the user picked one (listLime); none = by frequency
    explicit LimeSource(std::string args) {
        const size_t cut = args.rfind("#port=");
        if (cut != std::string::npos) { port_ = args.substr(cut + 6); args.erase(cut); }
        info_ = std::move(args);
    }
    ~LimeSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (lime().open(&dev_, info_.c_str(), nullptr) != 0 || !dev_) {
            dev_ = nullptr;
            err = "LimeSDR: cannot open the radio" + lime().text();
            if (kOnWindows) err += std::string(". If it is plugged in: ") + windowsUsbDriverHint();
            return false;
        }
        // On some USB controllers a LimeSDR-USB misdetects its reference clock (10 MHz instead of 30.72 MHz) on every other open, and
        // LMS_Init then fails. Closing and opening once more clears it; a board that really runs on a 10 MHz reference just opens twice.
        double refHz = 0;
        if (lime().get_clock_freq && lime().get_clock_freq(dev_, 0 /* LMS_CLOCK_REF */, &refHz) == 0 && refHz == 10e6) {
            lime().close(dev_);
            dev_ = nullptr;
            if (lime().open(&dev_, info_.c_str(), nullptr) != 0 || !dev_) { dev_ = nullptr; err = "LimeSDR: cannot open the radio again after a reference clock misdetection" + lime().text(); return false; }
        }
        if (lime().init(dev_) != 0) { err = "LimeSDR: initialisation failed" + lime().text(); return false; }
        if (lime().enable_channel(dev_, false, 0, true) != 0) { err = "LimeSDR: cannot enable the receiver" + lime().text(); return false; }
        lo_ = hi_ = 0;
        LimeApi::range_t fr{0, 0, 0};
        if (lime().get_lo_range && lime().get_lo_range(dev_, false, &fr) == 0 && fr.max > fr.min) {
            lo_ = fr.min; hi_ = fr.max;
            std::lock_guard<std::mutex> lk(gRangeMu);
            gRange[info_] = {lo_, hi_};
        }
        calLo_ = 0;
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        if (!live) {
            if (streaming_) { lime().stop_stream(&stream_); lime().destroy_stream(dev_, &stream_); streaming_ = false; }
            LimeApi::range_t range{0.1e6, 61.44e6, 0};
            lime().get_sample_rate_range(dev_, false, &range);
            const double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, range.min), range.max);
            if (lime().set_sample_rate(dev_, want, 0) != 0) { err = "LimeSDR: the sample rate was refused" + lime().text(); return false; }
            rate_ = want;
            // the rate the clock generator really gives ("Use LMS_GetSampleRate() to obtain actual sample rate values", LimeSuite.h)
            double host = 0, rf = 0;
            if (lime().get_sample_rate && lime().get_sample_rate(dev_, false, 0, &host, &rf) == 0 && host > 0) rate_ = host;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = hi_ > 0 ? s.centerHz : std::max(s.centerHz, 1e6);
        const std::string range = hi_ > 0 ? " (range " + mhz(lo_) + "-" + mhz(hi_) + " MHz)" : std::string();
        if (hi_ > 0 && (f < lo_ || f > hi_)) { err = "LimeSDR could not tune to " + mhz(f) + " MHz" + range; openFailure_ = OpenFailure::Final; return false; }
        // LimeSuite has no frequency correction in Hz (only the VCTCXO trim DAC): a correction the user set is taken out of the frequency
        if (lime().set_lo_frequency(dev_, false, 0, ppmCorrectedHz(f, radioPpm(s))) != 0) { err = "LimeSDR could not tune to " + mhz(f) + " MHz" + range + lime().text(); openFailure_ = OpenFailure::Final; return false; }
        selectAntenna(f);
        lime().set_lpf_bw(dev_, false, 0, std::max(1.5e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95));
        lime().set_gain_db(dev_, false, 0, (unsigned)std::min(73.0, std::max(0.0, std::round(s.gainDb))));
        if (!live) {
            lime().calibrate(dev_, false, 0, std::max(2.5e6, rate_ * 0.9), 0);   // DC offset and I/Q balance (takes a moment)
            calLo_ = f;
            stream_ = LimeApi::stream_t{};
            stream_.channel = 0; stream_.isTx = false; stream_.fifoSize = 1024 * 1024; stream_.throughputVsLatency = 1.0f;
            stream_.dataFmt = 0;   // 32-bit float, full scale +-1
            if (lime().setup_stream(dev_, &stream_) != 0) { err = "LimeSDR: cannot set up the sample stream" + lime().text(); return false; }
            if (lime().start_stream(&stream_) != 0) { lime().destroy_stream(dev_, &stream_); err = "LimeSDR: cannot start the sample stream" + lime().text(); return false; }
            streaming_ = true;
        } else if (calLo_ > 0 && (std::fabs(f - calLo_) > 0.1 * calLo_ || (f >= highHz()) != (calLo_ >= highHz()))) {
            // The DC offset and I/Q balance found at start hold near that frequency only (and differ between the low and the LNAH input):
            // after a big move the centre spike and the image come back. Calibrating blocks for a moment, so it runs here in the retune
            // call with the sample thread stopped, not in that thread.
            fprintf(stderr, "LimeSDR: recalibrating after the retune from %s to %s MHz\n", mhz(calLo_).c_str(), mhz(f).c_str());
            fflush(stderr);
            stopStream();
            if (streaming_) lime().stop_stream(&stream_);
            if (lime().calibrate(dev_, false, 0, std::max(2.5e6, rate_ * 0.9), 0) != 0) fprintf(stderr, "LimeSDR: calibration failed%s\n", lime().text().c_str());
            calLo_ = f;
            if (streaming_) lime().start_stream(&stream_);
            startStream();
        }
        return true;
    }
    void closeDevice() override {
        if (!dev_) return;
        if (streaming_) { lime().stop_stream(&stream_); lime().destroy_stream(dev_, &stream_); streaming_ = false; }
        lime().close(dev_);
        dev_ = nullptr;
    }
    void streamLoop() override {
        const size_t N = 16384;
        conv_.resize(N);
        int failures = 0, empty = 0;
        LimeApi::meta_t meta{};
        while (run_) {
            const int n = lime().recv_stream(&stream_, conv_.data(), N, &meta, 1000);   // float32 I/Q pairs = std::complex<float>
            if (n > 0) { push(conv_.data(), (size_t)n); failures = empty = 0; }
            else if (n < 0 && ++failures > 20) { fprintf(stderr, "LimeSDR: stream error %d, stopping the source\n", n); break; }
            // LMS_RecvStream returns 0 (nothing in its FIFO within the timeout), never an error, when the radio is unplugged: a stream
            // that brings nothing for kStallS seconds is treated as lost, so that the radio is opened again
            else if (n == 0 && run_ && ++empty >= kStallS) { fprintf(stderr, "LimeSDR: no samples for %d s, stopping the source\n", kStallS); break; }
        }
    }

private:
    static constexpr int kStallS = 5;
    // The LimeSDR has several receive inputs for different frequency ranges: LNAL for the low bands (TV and radio), LNAH above about 1.5 GHz,
    // LNAW (wideband) as the fallback.
    // The Mini has no LNAL wired to a socket (its list says "LNAL_NC"): LNAW below 1.7 GHz and LNAH above, the crossover LimeSuite's own
    // band switching uses for it (LMS7_LimeSDR_mini::AutoRFPath)
    double highHz() const { return info_.find("Mini") != std::string::npos ? 1.7e9 : 1.5e9; }
    void selectAntenna(double hz) {
        LimeApi::name_t names[16];
        std::memset(names, 0, sizeof names);
        const int n = lime().get_antenna_list(dev_, false, 0, names);
        int pick = -1, wide = -1;
        const double highHz = this->highHz();
        for (int i = 0; i < n && i < 16; i++) {   // the input the user picked (antenna on that socket), whatever the frequency
            const std::string a(names[i], strnlen(names[i], 16));
            if (!port_.empty() && a == port_) { lime().set_antenna(dev_, false, 0, (size_t)i); return; }
        }
        if (!port_.empty()) fprintf(stderr, "LimeSDR: no input %s on this radio, choosing by frequency\n", port_.c_str());
        for (int i = 0; i < n && i < 16; i++) {
            const std::string a(names[i], strnlen(names[i], 16));
            if (hz < highHz && a == "LNAL") pick = i;
            if (hz >= highHz && a == "LNAH") pick = i;
            if (a == "LNAW") wide = i;
        }
        if (pick < 0) pick = wide;
        if (pick >= 0) lime().set_antenna(dev_, false, 0, (size_t)pick);
    }
    std::string info_;
    std::string port_;   // "LNAL", "LNAH", "LNAW" or empty (by frequency)
    LimeApi::lms_device_t* dev_ = nullptr;
    LimeApi::stream_t stream_{};
    bool streaming_ = false;
    double lo_ = 0, hi_ = 0;   // LO range the library reports (0 = not known)
    double calLo_ = 0;         // LO frequency of the last calibration
};

void listLime(std::vector<DeviceInfo>& out) {
    if (!lime().ok) return;
    const int n = lime().get_device_list(nullptr);
    if (n <= 0) return;
    std::vector<char> raw((size_t)n * sizeof(LimeApi::info_str_t), 0);   // n strings of 256 characters
    const int got = lime().get_device_list(reinterpret_cast<LimeApi::info_str_t*>(raw.data()));
    for (int i = 0; i < got && i < n && i < 16; i++) {
        const char* p = raw.data() + (size_t)i * sizeof(LimeApi::info_str_t);
        const std::string info(p, strnlen(p, sizeof(LimeApi::info_str_t)));
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "lime";
        d.nativeArgs = info;   // the string LMS_Open() wants
        const size_t comma = info.find(',');
        d.name = (comma == std::string::npos ? info : info.substr(0, comma)) + " (native, experimental)";
        d.maxRateHz = info.find("Mini") != std::string::npos ? 30.72e6 : 61.44e6;   // the Mini's FT601 USB link carries 30.72 Msps at most
        d.minRateHz = 0.1e6;
        d.gainMinDb = 0; d.gainMaxDb = 73;
        d.settings = {ppmSetting(0.01)};
        {   // the range the library gave when this radio was last opened, else the data sheet's (the Mini's tuner starts higher)
            std::lock_guard<std::mutex> lk(gRangeMu);
            auto it = gRange.find(info);
            if (it != gRange.end()) { d.minFreqHz = it->second.first; d.maxFreqHz = it->second.second; }
            else if (info.find("Mini") != std::string::npos) { d.minFreqHz = 10e6; d.maxFreqHz = 3.5e9; }
            else { d.minFreqHz = 0.1e6; d.maxFreqHz = 3.8e9; }
        }
        out.push_back(d);
        // one entry per receive socket as well, for an antenna that is not on the input the automatic choice would take (the Mini has
        // no LNAL socket); the first entry stays the automatic one
        const bool mini = info.find("Mini") != std::string::npos;
        for (const char* port : mini ? std::vector<const char*>{"LNAW", "LNAH"} : std::vector<const char*>{"LNAL", "LNAH", "LNAW"}) {
            DeviceInfo e = d;
            e.nativeArgs = info + "#port=" + port;
            e.name = (comma == std::string::npos ? info : info.substr(0, comma)) + " " + port + " (native, experimental)";
            out.push_back(e);
        }
    }
}

std::unique_ptr<IqSource> makeLime(const DeviceInfo& d) {
    return std::make_unique<LimeSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
