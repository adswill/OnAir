// Native driver for the Airspy R2 / Mini: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

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
    int (DECT2_CALL* start_rx)(airspy_device*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_rx)(airspy_device*) = nullptr;
    bool ok = false;
    DynLib lib;
    AirspyApi() {
        if (nativeDisabled() || !lib.open(libNames("airspy", {".0"}, {"airspy.dll", "libairspy.dll"}))) return;
        ok = lib.get(init, "airspy_init") && lib.get(open_sn, "airspy_open_sn") && lib.get(open, "airspy_open") && lib.get(close, "airspy_close") &&
             lib.get(get_samplerates, "airspy_get_samplerates") && lib.get(set_samplerate, "airspy_set_samplerate") &&
             lib.get(set_sample_type, "airspy_set_sample_type") && lib.get(set_freq, "airspy_set_freq") &&
             lib.get(set_linearity_gain, "airspy_set_linearity_gain") && lib.get(start_rx, "airspy_start_rx") && lib.get(stop_rx, "airspy_stop_rx");
        lib.get(list_devices, "airspy_list_devices");
        if (ok) ok = init() == 0;
    }
};
AirspyApi& airspy() { static AirspyApi a; return a; }

class AirspySource : public NativeSource {
public:
    explicit AirspySource(uint64_t serial) : serial_(serial) {}
    ~AirspySource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        const int r = serial_ ? airspy().open_sn(&dev_, serial_) : airspy().open(&dev_);
        if (r != 0 || !dev_) { dev_ = nullptr; err = "Airspy: cannot open the radio (in use, or the driver is missing)"; return false; }
        airspy().set_sample_type(dev_, AirspyApi::SAMPLE_FLOAT32_IQ);
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
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
        airspy().set_freq(dev_, (uint32_t)std::llround(std::min(std::max(s.centerHz, 0.0), 4.29e9)));
        airspy().set_linearity_gain(dev_, (uint8_t)std::min(21.0, std::max(0.0, std::round(s.gainDb))));
        return true;
    }
    void closeDevice() override { if (dev_) airspy().close(dev_); dev_ = nullptr; }
    void streamLoop() override {
        if (airspy().start_rx(dev_, &AirspySource::callback, this) != 0) return;
        while (run_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        airspy().stop_rx(dev_);
    }

private:
    static int DECT2_CALL callback(AirspyApi::transfer_t* t) {
        auto* self = static_cast<AirspySource*>(t->ctx);
        if (!self->run_ || !t->samples || t->sample_count <= 0) return 0;
        self->push(static_cast<const cf32*>(t->samples), (size_t)t->sample_count);   // float32 I/Q pairs = std::complex<float>
        return 0;
    }
    uint64_t serial_;
    AirspyApi::airspy_device* dev_ = nullptr;
};

void listAirspy(std::vector<DeviceInfo>& out) {
    if (!airspy().ok) return;
    std::vector<uint64_t> serials;
    if (airspy().list_devices) {
        uint64_t sn[16] = {0};
        const int n = airspy().list_devices(sn, 16);
        for (int i = 0; i < n && i < 16; i++) serials.push_back(sn[i]);
    } else {   // older library: is there at least one?
        AirspyApi::airspy_device* d = nullptr;
        if (airspy().open(&d) == 0 && d) { airspy().close(d); serials.push_back(0); }
    }
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
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeAirspy(const DeviceInfo& d) {
    return std::make_unique<AirspySource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
