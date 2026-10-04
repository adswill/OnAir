// Native driver for the PlutoSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

namespace dect2 {
namespace native {


struct IioApi {
    struct iio_context; struct iio_device; struct iio_channel; struct iio_buffer; struct iio_scan_context; struct iio_context_info;
    iio_scan_context* (DECT2_CALL* create_scan_context)(const char*, unsigned) = nullptr;
    void (DECT2_CALL* scan_context_destroy)(iio_scan_context*) = nullptr;
    long long (DECT2_CALL* scan_context_get_info_list)(iio_scan_context*, iio_context_info***) = nullptr;
    void (DECT2_CALL* context_info_list_free)(iio_context_info**) = nullptr;
    const char* (DECT2_CALL* context_info_get_description)(const iio_context_info*) = nullptr;
    const char* (DECT2_CALL* context_info_get_uri)(const iio_context_info*) = nullptr;
    iio_context* (DECT2_CALL* create_context_from_uri)(const char*) = nullptr;
    void (DECT2_CALL* context_destroy)(iio_context*) = nullptr;
    iio_device* (DECT2_CALL* context_find_device)(const iio_context*, const char*) = nullptr;
    int (DECT2_CALL* context_set_timeout)(iio_context*, unsigned) = nullptr;
    iio_channel* (DECT2_CALL* device_find_channel)(const iio_device*, const char*, bool) = nullptr;
    long long (DECT2_CALL* channel_attr_write)(const iio_channel*, const char*, const char*) = nullptr;
    int (DECT2_CALL* channel_attr_write_longlong)(const iio_channel*, const char*, long long) = nullptr;
    void (DECT2_CALL* channel_enable)(iio_channel*) = nullptr;
    void (DECT2_CALL* channel_disable)(iio_channel*) = nullptr;
    iio_buffer* (DECT2_CALL* device_create_buffer)(const iio_device*, size_t, bool) = nullptr;
    void (DECT2_CALL* buffer_destroy)(iio_buffer*) = nullptr;
    long long (DECT2_CALL* buffer_refill)(iio_buffer*) = nullptr;
    void (DECT2_CALL* buffer_cancel)(iio_buffer*) = nullptr;
    void* (DECT2_CALL* buffer_start)(const iio_buffer*) = nullptr;
    void* (DECT2_CALL* buffer_end)(const iio_buffer*) = nullptr;
    bool ok = false;
    DynLib lib;
    IioApi() {
        if (nativeDisabled() || !lib.open(libNames("iio", {".0"}, {"libiio.dll", "iio.dll"}))) return;
        ok = lib.get(create_scan_context, "iio_create_scan_context") && lib.get(scan_context_destroy, "iio_scan_context_destroy") &&
             lib.get(scan_context_get_info_list, "iio_scan_context_get_info_list") && lib.get(context_info_list_free, "iio_context_info_list_free") &&
             lib.get(context_info_get_description, "iio_context_info_get_description") && lib.get(context_info_get_uri, "iio_context_info_get_uri") &&
             lib.get(create_context_from_uri, "iio_create_context_from_uri") && lib.get(context_destroy, "iio_context_destroy") &&
             lib.get(context_find_device, "iio_context_find_device") && lib.get(context_set_timeout, "iio_context_set_timeout") &&
             lib.get(device_find_channel, "iio_device_find_channel") && lib.get(channel_attr_write, "iio_channel_attr_write") &&
             lib.get(channel_attr_write_longlong, "iio_channel_attr_write_longlong") && lib.get(channel_enable, "iio_channel_enable") &&
             lib.get(channel_disable, "iio_channel_disable") && lib.get(device_create_buffer, "iio_device_create_buffer") &&
             lib.get(buffer_destroy, "iio_buffer_destroy") && lib.get(buffer_refill, "iio_buffer_refill") && lib.get(buffer_cancel, "iio_buffer_cancel") &&
             lib.get(buffer_start, "iio_buffer_start") && lib.get(buffer_end, "iio_buffer_end");
    }
};
IioApi& iio() { static IioApi a; return a; }

class PlutoSource : public NativeSource {
public:
    explicit PlutoSource(std::string uri) : uri_(std::move(uri)) {}
    ~PlutoSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        ctx_ = iio().create_context_from_uri(uri_.c_str());
        if (!ctx_) { err = "PlutoSDR: cannot connect (" + uri_ + ")"; return false; }
        iio().context_set_timeout(ctx_, 3000);
        phy_ = iio().context_find_device(ctx_, "ad9361-phy");
        rx_ = iio().context_find_device(ctx_, "cf-ad9361-lpc");
        if (!phy_ || !rx_) { err = "PlutoSDR: the AD9361 devices were not found"; return false; }
        rxCh_ = iio().device_find_channel(phy_, "voltage0", false);
        lo_ = iio().device_find_channel(phy_, "altvoltage0", true);
        i_ = iio().device_find_channel(rx_, "voltage0", false);
        q_ = iio().device_find_channel(rx_, "voltage1", false);
        if (!rxCh_ || !lo_ || !i_ || !q_) { err = "PlutoSDR: the receive channels were not found"; return false; }
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!ctx_) return false;
        if (!live) {
            if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
            // the converter runs 0.52 - 61.44 Msps; below 2.083 Msps it needs a decimation filter: stay at or above that
            const double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 2.1e6), 61.44e6);
            if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(want)) != 0) { err = "PlutoSDR: the sample rate was refused"; return false; }
            iio().channel_attr_write_longlong(rxCh_, "rf_bandwidth", (long long)std::llround(std::min(56e6, std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : want * 0.95))));
            iio().channel_attr_write(rxCh_, "gain_control_mode", "manual");
            rate_ = want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            iio().channel_enable(i_);
            iio().channel_enable(q_);
            buf_ = iio().device_create_buffer(rx_, kBlock, false);
            if (!buf_) { err = "PlutoSDR: cannot allocate the sample buffer"; return false; }
        }
        iio().channel_attr_write_longlong(lo_, "frequency", (long long)std::llround(std::min(std::max(s.centerHz, 70e6), 6e9)));
        iio().channel_attr_write_longlong(rxCh_, "hardwaregain", (long long)std::llround(std::min(73.0, std::max(0.0, s.gainDb))));
        return true;
    }
    void closeDevice() override {
        if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
        if (i_) iio().channel_disable(i_);
        if (q_) iio().channel_disable(q_);
        if (ctx_) iio().context_destroy(ctx_);
        ctx_ = nullptr; phy_ = rx_ = nullptr; rxCh_ = lo_ = i_ = q_ = nullptr;
    }
    void streamLoop() override {
        conv_.resize(kBlock);
        int failures = 0;
        while (run_) {
            const auto got = iio().buffer_refill(buf_);
            if (got <= 0) { if (++failures > 20) { fprintf(stderr, "PlutoSDR: stream error, stopping the source\n"); break; } continue; }
            failures = 0;
            const int16_t* p = static_cast<const int16_t*>(iio().buffer_start(buf_));
            const size_t n = std::min<size_t>((size_t)got / 4, kBlock);   // 4 bytes per I/Q sample
            for (size_t k = 0; k < n; k++) conv_[k] = cf32(p[2 * k] / 2048.f, p[2 * k + 1] / 2048.f);   // 12-bit samples in 16 bits
            push(conv_.data(), n);
        }
    }
    void interrupt() override { if (buf_) iio().buffer_cancel(buf_); }

private:
    static constexpr size_t kBlock = 1 << 16;
    std::string uri_;
    IioApi::iio_context* ctx_ = nullptr;
    IioApi::iio_device* phy_ = nullptr;
    IioApi::iio_device* rx_ = nullptr;
    IioApi::iio_channel* rxCh_ = nullptr;
    IioApi::iio_channel* lo_ = nullptr;
    IioApi::iio_channel* i_ = nullptr;
    IioApi::iio_channel* q_ = nullptr;
    IioApi::iio_buffer* buf_ = nullptr;
};

void listPluto(std::vector<DeviceInfo>& out) {
    if (!iio().ok) return;
    auto add = [&](const std::string& uri, const std::string& desc) {
        for (const auto& d : out) if (d.board == "pluto" && d.nativeArgs == uri) return;
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "pluto";
        d.nativeArgs = uri;
        d.name = "PlutoSDR " + (desc.empty() ? uri : "(" + uri + ")") + " (native, experimental)";
        d.maxRateHz = 61.44e6; d.minRateHz = 2.1e6;
        d.gainMinDb = 0; d.gainMaxDb = 73;
        out.push_back(d);
    };
    if (IioApi::iio_scan_context* sc = iio().create_scan_context("usb", 0)) {
        IioApi::iio_context_info** info = nullptr;
        const auto n = iio().scan_context_get_info_list(sc, &info);
        for (long i = 0; i < (long)n && i < 16; i++) {
            const char* desc = iio().context_info_get_description(info[i]);
            const char* uri = iio().context_info_get_uri(info[i]);
            if (!uri || !desc) continue;
            const std::string dd = desc;
            if (dd.find("PlutoSDR") != std::string::npos || dd.find("ADALM-PLUTO") != std::string::npos || dd.find("AD9361") != std::string::npos) add(uri, dd);
        }
        if (info) iio().context_info_list_free(info);
        iio().scan_context_destroy(sc);
    }
    if (const char* u = getenv("DECT2_PLUTO_URI")) add(u, "");   // a PlutoSDR on the network, e.g. ip:192.168.2.1
}

std::unique_ptr<IqSource> makePluto(const DeviceInfo& d) {
    return std::make_unique<PlutoSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
