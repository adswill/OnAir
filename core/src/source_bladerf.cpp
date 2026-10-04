// Native driver for the BladeRF: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

namespace dect2 {
namespace native {


struct BladeApi {
    struct bladerf;
    struct range_t { int64_t min, max, step; float scale; };   // struct bladerf_range
    struct devinfo_t {                                          // struct bladerf_devinfo
        int backend;
        char serial[33];
        uint8_t usb_bus, usb_addr;
        unsigned int instance;
        char manufacturer[33];
        char product[33];
    };
    static constexpr int kChannelRx0 = 0;          // BLADERF_CHANNEL_RX(0)
    static constexpr int kRxX1 = 0;                // BLADERF_RX_X1
    static constexpr int kFormatSc16Q11 = 0;       // BLADERF_FORMAT_SC16_Q11
    static constexpr int kGainMgc = 1;             // BLADERF_GAIN_MGC (manual)
    int (DECT2_CALL* get_device_list)(devinfo_t**) = nullptr;
    void (DECT2_CALL* free_device_list)(devinfo_t*) = nullptr;
    int (DECT2_CALL* open)(bladerf**, const char*) = nullptr;
    void (DECT2_CALL* close)(bladerf*) = nullptr;
    int (DECT2_CALL* set_frequency)(bladerf*, int, uint64_t) = nullptr;
    int (DECT2_CALL* set_sample_rate)(bladerf*, int, unsigned, unsigned*) = nullptr;
    int (DECT2_CALL* set_bandwidth)(bladerf*, int, unsigned, unsigned*) = nullptr;
    int (DECT2_CALL* set_gain)(bladerf*, int, int) = nullptr;
    int (DECT2_CALL* set_gain_mode)(bladerf*, int, int) = nullptr;
    int (DECT2_CALL* get_gain_range)(bladerf*, int, const range_t**) = nullptr;
    int (DECT2_CALL* enable_module)(bladerf*, int, bool) = nullptr;
    int (DECT2_CALL* sync_config)(bladerf*, int, int, unsigned, unsigned, unsigned, unsigned) = nullptr;
    int (DECT2_CALL* sync_rx)(bladerf*, void*, unsigned, void*, unsigned) = nullptr;
    bool ok = false;
    DynLib lib;
    BladeApi() {
        if (nativeDisabled() || !lib.open(libNames("bladeRF", {".2"}, {"bladeRF.dll", "libbladeRF.dll"}))) return;
        ok = lib.get(get_device_list, "bladerf_get_device_list") && lib.get(free_device_list, "bladerf_free_device_list") &&
             lib.get(open, "bladerf_open") && lib.get(close, "bladerf_close") && lib.get(set_frequency, "bladerf_set_frequency") &&
             lib.get(set_sample_rate, "bladerf_set_sample_rate") && lib.get(set_bandwidth, "bladerf_set_bandwidth") &&
             lib.get(set_gain, "bladerf_set_gain") && lib.get(set_gain_mode, "bladerf_set_gain_mode") &&
             lib.get(get_gain_range, "bladerf_get_gain_range") && lib.get(enable_module, "bladerf_enable_module") &&
             lib.get(sync_config, "bladerf_sync_config") && lib.get(sync_rx, "bladerf_sync_rx");
    }
};
BladeApi& blade() { static BladeApi a; return a; }

class BladeSource : public NativeSource {
public:
    explicit BladeSource(std::string serial) : serial_(std::move(serial)) {}
    ~BladeSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        const std::string id = serial_.empty() ? std::string() : "*:serial=" + serial_;
        if (blade().open(&dev_, id.empty() ? nullptr : id.c_str()) != 0 || !dev_) { dev_ = nullptr; err = "bladeRF: cannot open the radio (in use, or its FPGA image is missing)"; return false; }
        blade().set_gain_mode(dev_, BladeApi::kChannelRx0, BladeApi::kGainMgc);
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        const int ch = BladeApi::kChannelRx0;
        if (!live) {
            if (enabled_) { blade().enable_module(dev_, ch, false); enabled_ = false; }
            unsigned actual = 0;
            const unsigned want = (unsigned)std::llround(std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 0.52e6), 61.44e6));
            if (blade().set_sample_rate(dev_, ch, want, &actual) != 0) { err = "bladeRF: the sample rate was refused"; return false; }
            rate_ = actual ? actual : want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            unsigned bwActual = 0;
            blade().set_bandwidth(dev_, ch, (unsigned)std::llround(std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95)), &bwActual);
            // 16 buffers of 16384 samples, 8 USB transfers in flight
            if (blade().sync_config(dev_, BladeApi::kRxX1, BladeApi::kFormatSc16Q11, 16, 16384, 8, 3500) != 0) { err = "bladeRF: cannot start the sample stream"; return false; }
            if (blade().enable_module(dev_, ch, true) != 0) { err = "bladeRF: cannot enable the receiver"; return false; }
            enabled_ = true;
        }
        blade().set_frequency(dev_, ch, (uint64_t)std::llround(std::max(s.centerHz, 0.0)));
        double gmin = -15, gmax = 60;
        const BladeApi::range_t* r = nullptr;
        if (blade().get_gain_range(dev_, ch, &r) == 0 && r) { gmin = r->min * r->scale; gmax = r->max * r->scale; }
        blade().set_gain(dev_, ch, (int)std::lround(std::min(std::max(s.gainDb, gmin), gmax)));
        return true;
    }
    void closeDevice() override {
        if (!dev_) return;
        if (enabled_) blade().enable_module(dev_, BladeApi::kChannelRx0, false);
        enabled_ = false;
        blade().close(dev_);
        dev_ = nullptr;
    }
    void streamLoop() override {
        const unsigned N = 16384;
        std::vector<int16_t> raw((size_t)N * 2);
        conv_.resize(N);
        int failures = 0;
        while (run_) {
            const int r = blade().sync_rx(dev_, raw.data(), N, nullptr, 2000);
            if (r == 0) {
                for (unsigned i = 0; i < N; i++) conv_[i] = cf32(raw[2 * i] / 2048.f, raw[2 * i + 1] / 2048.f);   // Q11: +-2047 = full scale
                push(conv_.data(), N);
                failures = 0;
            } else if (++failures > 20) { fprintf(stderr, "bladeRF: stream error %d, stopping the source\n", r); break; }
        }
    }

private:
    std::string serial_;
    BladeApi::bladerf* dev_ = nullptr;
    bool enabled_ = false;
};

void listBlade(std::vector<DeviceInfo>& out) {
    if (!blade().ok) return;
    BladeApi::devinfo_t* list = nullptr;
    const int n = blade().get_device_list(&list);
    if (n <= 0 || !list) return;
    for (int i = 0; i < n && i < 16; i++) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "bladerf";
        d.serial = trimmed(list[i].serial, sizeof list[i].serial);
        d.nativeArgs = d.serial;
        std::string prod = trimmed(list[i].product, sizeof list[i].product);
        d.name = (prod.empty() ? "bladeRF" : prod) + (d.serial.size() > 8 ? " " + d.serial.substr(d.serial.size() - 8) : "") + " (native, experimental)";
        d.maxRateHz = 61.44e6; d.minRateHz = 0.52e6;
        d.gainMinDb = -15; d.gainMaxDb = 60;
        out.push_back(d);
    }
    blade().free_device_list(list);
}

std::unique_ptr<IqSource> makeBlade(const DeviceInfo& d) {
    return std::make_unique<BladeSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
