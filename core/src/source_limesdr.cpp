// Native driver for the LimeSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

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
    bool ok = false;
    DynLib lib;
    LimeApi() {
        if (nativeDisabled() || !lib.open(libNames("LimeSuite", {".23.11-1", ".22.09-1", ".20.10-1", ".20.01-1", ".19.04-1", ".18.06-1", ""}, {"LimeSuite.dll", "libLimeSuite.dll"}))) return;
        ok = lib.get(get_device_list, "LMS_GetDeviceList") && lib.get(open, "LMS_Open") && lib.get(close, "LMS_Close") && lib.get(init, "LMS_Init") &&
             lib.get(enable_channel, "LMS_EnableChannel") && lib.get(set_sample_rate, "LMS_SetSampleRate") &&
             lib.get(get_sample_rate_range, "LMS_GetSampleRateRange") && lib.get(set_lo_frequency, "LMS_SetLOFrequency") &&
             lib.get(get_antenna_list, "LMS_GetAntennaList") && lib.get(set_antenna, "LMS_SetAntenna") && lib.get(set_lpf_bw, "LMS_SetLPFBW") &&
             lib.get(set_gain_db, "LMS_SetGaindB") && lib.get(calibrate, "LMS_Calibrate") && lib.get(setup_stream, "LMS_SetupStream") &&
             lib.get(destroy_stream, "LMS_DestroyStream") && lib.get(start_stream, "LMS_StartStream") && lib.get(stop_stream, "LMS_StopStream") &&
             lib.get(recv_stream, "LMS_RecvStream");
    }
};
LimeApi& lime() { static LimeApi a; return a; }

class LimeSource : public NativeSource {
public:
    explicit LimeSource(std::string info) : info_(std::move(info)) {}
    ~LimeSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (lime().open(&dev_, info_.c_str(), nullptr) != 0 || !dev_) { dev_ = nullptr; err = "LimeSDR: cannot open the radio (in use, or the driver is missing)"; return false; }
        if (lime().init(dev_) != 0) { err = "LimeSDR: initialisation failed"; return false; }
        if (lime().enable_channel(dev_, false, 0, true) != 0) { err = "LimeSDR: cannot enable the receiver"; return false; }
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        if (!live) {
            if (streaming_) { lime().stop_stream(&stream_); lime().destroy_stream(dev_, &stream_); streaming_ = false; }
            LimeApi::range_t range{0.1e6, 61.44e6, 0};
            lime().get_sample_rate_range(dev_, false, &range);
            const double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, range.min), range.max);
            if (lime().set_sample_rate(dev_, want, 0) != 0) { err = "LimeSDR: the sample rate was refused"; return false; }
            rate_ = want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = std::max(s.centerHz, 1e6);
        lime().set_lo_frequency(dev_, false, 0, f);
        selectAntenna(f);
        lime().set_lpf_bw(dev_, false, 0, std::max(1.5e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95));
        lime().set_gain_db(dev_, false, 0, (unsigned)std::min(73.0, std::max(0.0, std::round(s.gainDb))));
        if (!live) {
            lime().calibrate(dev_, false, 0, std::max(2.5e6, rate_ * 0.9), 0);   // DC offset and I/Q balance (takes a moment)
            stream_ = LimeApi::stream_t{};
            stream_.channel = 0; stream_.isTx = false; stream_.fifoSize = 1024 * 1024; stream_.throughputVsLatency = 1.0f;
            stream_.dataFmt = 0;   // 32-bit float, full scale +-1
            if (lime().setup_stream(dev_, &stream_) != 0) { err = "LimeSDR: cannot set up the sample stream"; return false; }
            if (lime().start_stream(&stream_) != 0) { lime().destroy_stream(dev_, &stream_); err = "LimeSDR: cannot start the sample stream"; return false; }
            streaming_ = true;
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
        int failures = 0;
        LimeApi::meta_t meta{};
        while (run_) {
            const int n = lime().recv_stream(&stream_, conv_.data(), N, &meta, 1000);   // float32 I/Q pairs = std::complex<float>
            if (n > 0) { push(conv_.data(), (size_t)n); failures = 0; }
            else if (n < 0 && ++failures > 20) { fprintf(stderr, "LimeSDR: stream error %d, stopping the source\n", n); break; }
        }
    }

private:
    // The LimeSDR has several receive inputs for different frequency ranges: LNAL for the low bands (TV and radio), LNAH above about 1.5 GHz,
    // LNAW (wideband) as the fallback.
    void selectAntenna(double hz) {
        LimeApi::name_t names[16];
        std::memset(names, 0, sizeof names);
        const int n = lime().get_antenna_list(dev_, false, 0, names);
        int pick = -1, wide = -1;
        for (int i = 0; i < n && i < 16; i++) {
            const std::string a(names[i], strnlen(names[i], 16));
            if (hz < 1.5e9 && a == "LNAL") pick = i;
            if (hz >= 1.5e9 && a == "LNAH") pick = i;
            if (a == "LNAW") wide = i;
        }
        if (pick < 0) pick = wide;
        if (pick >= 0) lime().set_antenna(dev_, false, 0, (size_t)pick);
    }
    std::string info_;
    LimeApi::lms_device_t* dev_ = nullptr;
    LimeApi::stream_t stream_{};
    bool streaming_ = false;
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
        d.maxRateHz = 61.44e6; d.minRateHz = 0.1e6;
        d.gainMinDb = 0; d.gainMaxDb = 73;
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeLime(const DeviceInfo& d) {
    return std::make_unique<LimeSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
