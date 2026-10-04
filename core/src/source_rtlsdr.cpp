// Native driver for the RTL-SDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

namespace dect2 {
namespace native {


struct RtlApi {
    typedef struct rtlsdr_dev rtlsdr_dev_t;
    typedef void (DECT2_CALL* cb_t)(unsigned char* buf, uint32_t len, void* ctx);
    uint32_t (DECT2_CALL* get_device_count)(void) = nullptr;
    const char* (DECT2_CALL* get_device_name)(uint32_t) = nullptr;
    int (DECT2_CALL* get_device_usb_strings)(uint32_t, char*, char*, char*) = nullptr;
    int (DECT2_CALL* open)(rtlsdr_dev_t**, uint32_t) = nullptr;
    int (DECT2_CALL* close)(rtlsdr_dev_t*) = nullptr;
    int (DECT2_CALL* set_center_freq)(rtlsdr_dev_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_sample_rate)(rtlsdr_dev_t*, uint32_t) = nullptr;
    uint32_t (DECT2_CALL* get_sample_rate)(rtlsdr_dev_t*) = nullptr;
    int (DECT2_CALL* set_tuner_gain_mode)(rtlsdr_dev_t*, int) = nullptr;
    int (DECT2_CALL* get_tuner_gains)(rtlsdr_dev_t*, int*) = nullptr;
    int (DECT2_CALL* set_tuner_gain)(rtlsdr_dev_t*, int) = nullptr;
    int (DECT2_CALL* set_agc_mode)(rtlsdr_dev_t*, int) = nullptr;
    int (DECT2_CALL* reset_buffer)(rtlsdr_dev_t*) = nullptr;
    int (DECT2_CALL* read_async)(rtlsdr_dev_t*, cb_t, void*, uint32_t, uint32_t) = nullptr;
    int (DECT2_CALL* cancel_async)(rtlsdr_dev_t*) = nullptr;
    bool ok = false;
    DynLib lib;
    RtlApi() {
        if (nativeDisabled() || !lib.open(libNames("rtlsdr", {".0", ".2"}, {"rtlsdr.dll", "librtlsdr.dll"}))) return;
        ok = lib.get(get_device_count, "rtlsdr_get_device_count") && lib.get(get_device_name, "rtlsdr_get_device_name") &&
             lib.get(get_device_usb_strings, "rtlsdr_get_device_usb_strings") && lib.get(open, "rtlsdr_open") && lib.get(close, "rtlsdr_close") &&
             lib.get(set_center_freq, "rtlsdr_set_center_freq") && lib.get(set_sample_rate, "rtlsdr_set_sample_rate") &&
             lib.get(get_sample_rate, "rtlsdr_get_sample_rate") && lib.get(set_tuner_gain_mode, "rtlsdr_set_tuner_gain_mode") &&
             lib.get(get_tuner_gains, "rtlsdr_get_tuner_gains") && lib.get(set_tuner_gain, "rtlsdr_set_tuner_gain") &&
             lib.get(set_agc_mode, "rtlsdr_set_agc_mode") && lib.get(reset_buffer, "rtlsdr_reset_buffer") &&
             lib.get(read_async, "rtlsdr_read_async") && lib.get(cancel_async, "rtlsdr_cancel_async");
    }
};
RtlApi& rtl() { static RtlApi a; return a; }

class RtlSource : public NativeSource {
public:
    explicit RtlSource(uint32_t index) : index_(index) {}
    ~RtlSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (rtl().open(&dev_, index_) != 0 || !dev_) { dev_ = nullptr; err = "RTL-SDR: cannot open the radio (in use, or the driver is missing)"; return false; }
        rtl().set_agc_mode(dev_, 0);
        rtl().set_tuner_gain_mode(dev_, 1);   // manual gain: OnAir runs its own AGC
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        if (!live) {
            // stable rates are 225-300 kHz and 0.9-3.2 MHz; above about 2.56 MHz most dongles drop samples
            double want = std::min(s.sampleRate > 0 ? s.sampleRate : 2.048e6, 2.56e6);
            if (want < 900001 && want > 300000) want = 900001;
            rtl().set_sample_rate(dev_, (uint32_t)std::lround(want));
            rate_ = rtl().get_sample_rate(dev_);
            if (rate_ <= 0) rate_ = want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        rtl().set_center_freq(dev_, (uint32_t)std::llround(std::min(std::max(s.centerHz, 0.0), 4.29e9)));
        // the tuner offers a fixed list of gains (tenths of a dB): take the closest
        const int n = rtl().get_tuner_gains(dev_, nullptr);
        if (n > 0) {
            std::vector<int> g((size_t)n);
            rtl().get_tuner_gains(dev_, g.data());
            int best = g[0];
            for (int v : g) if (std::abs(v - (int)std::lround(s.gainDb * 10)) < std::abs(best - (int)std::lround(s.gainDb * 10))) best = v;
            rtl().set_tuner_gain(dev_, best);
        }
        return true;
    }
    void closeDevice() override { if (dev_) rtl().close(dev_); dev_ = nullptr; }
    void streamLoop() override {
        rtl().reset_buffer(dev_);
        // blocks until cancel_async(); 15 buffers of 256 kB (the library's defaults)
        rtl().read_async(dev_, &RtlSource::callback, this, 0, 0);
    }
    void interrupt() override { if (dev_) rtl().cancel_async(dev_); }

private:
    static void DECT2_CALL callback(unsigned char* buf, uint32_t len, void* ctx) {
        auto* self = static_cast<RtlSource*>(ctx);
        if (!self->run_) { rtl().cancel_async(self->dev_); return; }
        const size_t n = len / 2;
        self->conv_.resize(n);
        for (size_t i = 0; i < n; i++) self->conv_[i] = cf32((buf[2 * i] - 127.4f) / 128.f, (buf[2 * i + 1] - 127.4f) / 128.f);
        self->push(self->conv_.data(), n);
    }
    uint32_t index_;
    RtlApi::rtlsdr_dev_t* dev_ = nullptr;
};

void listRtl(std::vector<DeviceInfo>& out) {
    if (!rtl().ok) return;
    const uint32_t n = rtl().get_device_count();
    for (uint32_t i = 0; i < n && i < 16; i++) {
        char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
        rtl().get_device_usb_strings(i, vendor, product, serial);
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "rtlsdr";
        d.nativeArgs = std::to_string(i);
        d.serial = trimmed(serial, sizeof serial);
        std::string label = trimmed(product, sizeof product);
        if (label.empty()) { const char* nm = rtl().get_device_name(i); label = nm ? nm : "RTL-SDR"; }
        d.name = label + (d.serial.empty() ? "" : " " + d.serial) + " (native, experimental)";
        d.maxRateHz = 2.56e6; d.minRateHz = 0.9e6;
        d.gainMinDb = 0; d.gainMaxDb = 49.6;
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeRtl(const DeviceInfo& d) {
    return std::make_unique<RtlSource>((uint32_t)strtoul(d.nativeArgs.c_str(), nullptr, 10));
}

} // namespace native
} // namespace dect2
