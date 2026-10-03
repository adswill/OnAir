// Native drivers for radios other than the HackRF: RTL-SDR, Airspy, PlutoSDR, BladeRF, LimeSDR and USRP, without SoapySDR in between.
//
// EXPERIMENTAL: none of these has been tried on real hardware yet (only against fake libraries in the tests). Each one talks to the
// manufacturer's own library (librtlsdr, libairspy, libiio, libbladeRF, LimeSuite, UHD), which is loaded when the program starts: OnAir
// needs none of them to build, and a radio whose library is not installed is simply not offered. The SoapySDR route stays available for
// the same radios. Set DECT2_NO_NATIVE=1 to switch all of this off.
//
// The few functions and structures used are declared here (the libraries' headers are not needed); they follow the libraries' public C APIs.
#include "dect2/source.h"
#include "dect2/ring.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define DECT2_CALL __cdecl
#else
#include <dlfcn.h>
#define DECT2_CALL
#endif

namespace dect2 {
namespace {

// ------------------------------------------------------------------ loading a library at run time

struct DynLib {
    void* h = nullptr;
    std::string path;
    bool open(const std::vector<std::string>& names) {
        if (const char* dir = getenv("DECT2_NATIVE_LIBDIR")) {   // tests: fake libraries in one folder
            for (const auto& n : names) {
                std::string b = base(n);
                const size_t so = b.find(".so.");
                if (so != std::string::npos) b.resize(so + 3);   // the test libraries carry no version number
                if (tryOpen(std::string(dir) + "/" + b)) return true;
            }
            return false;
        }
        for (const auto& n : names) if (tryOpen(n)) return true;
        return false;
    }
    template <class F> bool get(F& fn, const char* name) {
        if (!h) return false;
#ifdef _WIN32
        fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress((HMODULE)h, name)));
#else
        fn = reinterpret_cast<F>(dlsym(h, name));
#endif
        return fn != nullptr;
    }

private:
    static std::string base(const std::string& n) { const size_t s = n.find_last_of("/\\"); return s == std::string::npos ? n : n.substr(s + 1); }
    bool tryOpen(const std::string& n) {
#ifdef _WIN32
        h = (void*)LoadLibraryA(n.c_str());
#else
        h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (h) path = n;
        return h != nullptr;
    }
};

// Where each platform keeps a library called `stem` (with the usual name variants)
std::vector<std::string> libNames(const char* stem, std::initializer_list<const char*> soVersions, std::initializer_list<const char*> winNames) {
    std::vector<std::string> v;
#if defined(_WIN32)
    for (const char* w : winNames) v.push_back(w);
    (void)stem; (void)soVersions;
#elif defined(__APPLE__)
    (void)soVersions; (void)winNames;
    for (const char* dir : {"", "/opt/homebrew/lib/", "/usr/local/lib/", "/opt/local/lib/"}) v.push_back(std::string(dir) + "lib" + stem + ".dylib");
#else
    (void)winNames;
    for (const char* ver : soVersions) v.push_back(std::string("lib") + stem + ".so" + ver);
    v.push_back(std::string("lib") + stem + ".so");
#endif
    return v;
}

bool nativeDisabled() { const char* e = getenv("DECT2_NO_NATIVE"); return e && *e && *e != '0'; }

// ------------------------------------------------------------------ the common part of every native source

class NativeSource : public IqSource {
public:
    ~NativeSource() override = default;
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        if (!openDevice(s, err)) { closeDevice(); return false; }
        startStream();
        return true;
    }
    void stop() override {
        stopStream();
        closeDevice();
    }
    bool retune(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::mutex> lk(cfg_);
        const bool rateChange = s.sampleRate > 0 && std::fabs(s.sampleRate - requested_) > 1.0;
        if (rateChange && restartForRate()) {   // most radios change their sample rate only while not streaming
            stopStream();
            const bool ok = configure(s, err, false);
            startStream();
            return ok;
        }
        return configure(s, err, true);
    }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

protected:
    virtual bool openDevice(const TuneSettings& s, std::string& err) = 0;   // open and configure()
    virtual bool configure(const TuneSettings& s, std::string& err, bool live) = 0;
    virtual void closeDevice() = 0;
    virtual void streamLoop() = 0;            // runs until run_ is false (or the stream fails)
    virtual void interrupt() {}              // makes a blocking read in streamLoop() return
    virtual bool restartForRate() const { return true; }

    void startStream() {
        run_ = true;
        th_ = std::thread([this] { streamLoop(); });
    }
    void stopStream() {
        if (!th_.joinable()) return;
        run_ = false;
        interrupt();
        th_.join();
    }
    void push(const cf32* p, size_t n) { if (ring_ && n) ring_->write(p, n); }
    // reports a rate below what the channel needs (the receiver may still work for narrower channels)
    void checkRate(double want, std::string& err) {
        if (rate_ < want * 0.97) {
            char b[200];
            snprintf(b, sizeof b, "this radio only offers %.2f Msps, %.2f Msps were requested for this channel width", rate_ / 1e6, want / 1e6);
            err = b;
        }
    }

    IqRing* ring_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread th_;
    std::mutex cfg_;
    double rate_ = 0, requested_ = 0;
    std::vector<cf32> conv_;
};

std::string trimmed(const char* s, size_t max) {
    std::string r(s, strnlen(s, max));
    while (!r.empty() && (r.back() == ' ' || r.back() == '\n')) r.pop_back();
    return r;
}

// =================================================================== RTL-SDR (librtlsdr)

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

// =================================================================== Airspy R2 / Mini (libairspy)

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


// =================================================================== BladeRF (libbladeRF 2.x)

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

// =================================================================== LimeSDR (LimeSuite)

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

// =================================================================== PlutoSDR (libiio 0.x)

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

// =================================================================== USRP (UHD C API)

struct UhdApi {
    typedef int err_t;   // uhd_error: 0 = success
    struct usrp_t; struct streamer_t; struct metadata_t; struct range_t_; struct strvec_t;
    struct tune_request_t { double target_freq; int rf_freq_policy; double rf_freq; int dsp_freq_policy; double dsp_freq; char* args; };
    struct tune_result_t { double clipped_rf_freq, target_rf_freq, actual_rf_freq, target_dsp_freq, actual_dsp_freq; };
    struct stream_args_t { char* cpu_format; char* otw_format; char* args; size_t* channel_list; int n_channels; };
    struct stream_cmd_t { int stream_mode; size_t num_samps; bool stream_now; int64_t time_spec_full_secs; double time_spec_frac_secs; };
    static constexpr int kPolicyAuto = 65, kStartContinuous = 97, kStopContinuous = 111, kErrNone = 0, kRxOverflow = 0x8, kRxTimeout = 0x1;
    err_t (DECT2_CALL* find)(const char*, strvec_t**) = nullptr;
    err_t (DECT2_CALL* strvec_make)(strvec_t**) = nullptr;
    err_t (DECT2_CALL* strvec_free)(strvec_t**) = nullptr;
    err_t (DECT2_CALL* strvec_size)(strvec_t*, size_t*) = nullptr;
    err_t (DECT2_CALL* strvec_at)(strvec_t*, size_t, char*, size_t) = nullptr;
    err_t (DECT2_CALL* make)(usrp_t**, const char*) = nullptr;
    err_t (DECT2_CALL* free_)(usrp_t**) = nullptr;
    err_t (DECT2_CALL* set_rx_rate)(usrp_t*, double, size_t) = nullptr;
    err_t (DECT2_CALL* get_rx_rate)(usrp_t*, size_t, double*) = nullptr;
    err_t (DECT2_CALL* get_rx_rates)(usrp_t*, size_t, range_t_*) = nullptr;
    err_t (DECT2_CALL* set_rx_freq)(usrp_t*, tune_request_t*, size_t, tune_result_t*) = nullptr;
    err_t (DECT2_CALL* set_rx_gain)(usrp_t*, double, size_t, const char*) = nullptr;
    err_t (DECT2_CALL* get_rx_gain_range)(usrp_t*, const char*, size_t, range_t_*) = nullptr;
    err_t (DECT2_CALL* set_rx_bandwidth)(usrp_t*, double, size_t) = nullptr;
    err_t (DECT2_CALL* get_rx_stream)(usrp_t*, stream_args_t*, streamer_t*) = nullptr;
    err_t (DECT2_CALL* rx_streamer_make)(streamer_t**) = nullptr;
    err_t (DECT2_CALL* rx_streamer_free)(streamer_t**) = nullptr;
    err_t (DECT2_CALL* rx_streamer_max_num_samps)(streamer_t*, size_t*) = nullptr;
    err_t (DECT2_CALL* rx_streamer_recv)(streamer_t*, void**, size_t, metadata_t**, double, bool, size_t*) = nullptr;
    err_t (DECT2_CALL* rx_streamer_issue_stream_cmd)(streamer_t*, const stream_cmd_t*) = nullptr;
    err_t (DECT2_CALL* rx_metadata_make)(metadata_t**) = nullptr;
    err_t (DECT2_CALL* rx_metadata_free)(metadata_t**) = nullptr;
    err_t (DECT2_CALL* rx_metadata_error_code)(metadata_t*, int*) = nullptr;
    err_t (DECT2_CALL* meta_range_make)(range_t_**) = nullptr;
    err_t (DECT2_CALL* meta_range_free)(range_t_**) = nullptr;
    err_t (DECT2_CALL* meta_range_start)(range_t_*, double*) = nullptr;
    err_t (DECT2_CALL* meta_range_stop)(range_t_*, double*) = nullptr;
    bool ok = false;
    DynLib lib;
    UhdApi() {
        if (nativeDisabled() || !lib.open(libNames("uhd", {".4.8.0", ".4.7.0", ".4.6.0", ".4.5.0", ".4.4.0", ".4.3.0", ".4.2.0", ".4.1.0", ".4.0.0", ".3.15.0", ""}, {"uhd.dll", "libuhd.dll"}))) return;
        ok = lib.get(find, "uhd_usrp_find") && lib.get(strvec_make, "uhd_string_vector_make") && lib.get(strvec_free, "uhd_string_vector_free") &&
             lib.get(strvec_size, "uhd_string_vector_size") && lib.get(strvec_at, "uhd_string_vector_at") && lib.get(make, "uhd_usrp_make") &&
             lib.get(free_, "uhd_usrp_free") && lib.get(set_rx_rate, "uhd_usrp_set_rx_rate") && lib.get(get_rx_rate, "uhd_usrp_get_rx_rate") &&
             lib.get(get_rx_rates, "uhd_usrp_get_rx_rates") && lib.get(set_rx_freq, "uhd_usrp_set_rx_freq") && lib.get(set_rx_gain, "uhd_usrp_set_rx_gain") &&
             lib.get(get_rx_gain_range, "uhd_usrp_get_rx_gain_range") && lib.get(set_rx_bandwidth, "uhd_usrp_set_rx_bandwidth") &&
             lib.get(get_rx_stream, "uhd_usrp_get_rx_stream") && lib.get(rx_streamer_make, "uhd_rx_streamer_make") &&
             lib.get(rx_streamer_free, "uhd_rx_streamer_free") && lib.get(rx_streamer_max_num_samps, "uhd_rx_streamer_max_num_samps") &&
             lib.get(rx_streamer_recv, "uhd_rx_streamer_recv") && lib.get(rx_streamer_issue_stream_cmd, "uhd_rx_streamer_issue_stream_cmd") &&
             lib.get(rx_metadata_make, "uhd_rx_metadata_make") && lib.get(rx_metadata_free, "uhd_rx_metadata_free") &&
             lib.get(rx_metadata_error_code, "uhd_rx_metadata_error_code") && lib.get(meta_range_make, "uhd_meta_range_make") &&
             lib.get(meta_range_free, "uhd_meta_range_free") && lib.get(meta_range_start, "uhd_meta_range_start") && lib.get(meta_range_stop, "uhd_meta_range_stop");
    }
};
UhdApi& uhd() { static UhdApi a; return a; }

class UsrpSource : public NativeSource {
public:
    explicit UsrpSource(std::string args) : args_(std::move(args)) {}
    ~UsrpSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (uhd().make(&dev_, args_.c_str()) != 0 || !dev_) { dev_ = nullptr; err = "USRP: cannot open the radio (in use, or its FPGA images are not installed: run uhd_images_downloader)"; return false; }
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        const size_t ch = 0;
        if (!live) {
            if (streamer_) { stopReceiving(); }
            double lo = 0.1e6, hi = 200e6;
            UhdApi::range_t_* rg = nullptr;
            if (uhd().meta_range_make(&rg) == 0 && rg) {
                if (uhd().get_rx_rates(dev_, ch, rg) == 0) { uhd().meta_range_start(rg, &lo); uhd().meta_range_stop(rg, &hi); }
                uhd().meta_range_free(&rg);
            }
            const double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, lo), hi);
            if (uhd().set_rx_rate(dev_, want, ch) != 0) { err = "USRP: the sample rate was refused"; return false; }
            double actual = want;
            uhd().get_rx_rate(dev_, ch, &actual);
            rate_ = actual;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            uhd().set_rx_bandwidth(dev_, std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95), ch);
        }
        char noArgs[1] = {0};
        UhdApi::tune_request_t req{std::max(s.centerHz, 1e6), UhdApi::kPolicyAuto, 0, UhdApi::kPolicyAuto, 0, noArgs};
        UhdApi::tune_result_t res{};
        uhd().set_rx_freq(dev_, &req, ch, &res);
        double gmin = 0, gmax = 76;
        if (UhdApi::range_t_* rg = nullptr; uhd().meta_range_make(&rg) == 0 && rg) {
            if (uhd().get_rx_gain_range(dev_, "", ch, rg) == 0) { uhd().meta_range_start(rg, &gmin); uhd().meta_range_stop(rg, &gmax); }
            uhd().meta_range_free(&rg);
        }
        uhd().set_rx_gain(dev_, std::min(std::max(s.gainDb, gmin), gmax), ch, "");
        if (!live && !startReceiving(err)) return false;
        return true;
    }
    void closeDevice() override {
        stopReceiving();
        if (dev_) uhd().free_(&dev_);
        dev_ = nullptr;
    }
    void streamLoop() override {
        size_t maxN = 0;
        uhd().rx_streamer_max_num_samps(streamer_, &maxN);
        const size_t N = std::min<size_t>(maxN > 0 ? maxN : 16384, 65536);
        conv_.resize(N);
        int failures = 0;
        while (run_) {
            void* bufs[1] = {conv_.data()};
            size_t got = 0;
            const int e = uhd().rx_streamer_recv(streamer_, bufs, N, &md_, 1.0, false, &got);
            int code = 0;
            if (e == 0) uhd().rx_metadata_error_code(md_, &code);
            if (e == 0 && (code == UhdApi::kErrNone || code == UhdApi::kRxOverflow) && got > 0) { push(conv_.data(), got); failures = 0; }
            else if (e == 0 && code == UhdApi::kRxTimeout) continue;
            else if (++failures > 20) { fprintf(stderr, "USRP: stream error %d / %d, stopping the source\n", e, code); break; }
        }
    }

private:
    bool startReceiving(std::string& err) {
        size_t chan = 0;
        char cpu[] = "fc32", otw[] = "sc16", none[] = "";
        UhdApi::stream_args_t sa{cpu, otw, none, &chan, 1};
        if (uhd().rx_streamer_make(&streamer_) != 0 || uhd().rx_metadata_make(&md_) != 0 || uhd().get_rx_stream(dev_, &sa, streamer_) != 0) { err = "USRP: cannot set up the sample stream"; stopReceiving(); return false; }
        UhdApi::stream_cmd_t cmd{UhdApi::kStartContinuous, 0, true, 0, 0.0};
        if (uhd().rx_streamer_issue_stream_cmd(streamer_, &cmd) != 0) { err = "USRP: cannot start the sample stream"; stopReceiving(); return false; }
        return true;
    }
    void stopReceiving() {
        if (streamer_) { UhdApi::stream_cmd_t cmd{UhdApi::kStopContinuous, 0, true, 0, 0.0}; uhd().rx_streamer_issue_stream_cmd(streamer_, &cmd); uhd().rx_streamer_free(&streamer_); }
        if (md_) uhd().rx_metadata_free(&md_);
        streamer_ = nullptr; md_ = nullptr;
    }
    std::string args_;
    UhdApi::usrp_t* dev_ = nullptr;
    UhdApi::streamer_t* streamer_ = nullptr;
    UhdApi::metadata_t* md_ = nullptr;
};

void listUsrp(std::vector<DeviceInfo>& out) {
    if (!uhd().ok) return;
    UhdApi::strvec_t* v = nullptr;
    if (uhd().strvec_make(&v) != 0 || !v) return;
    if (uhd().find("", &v) == 0) {
        size_t n = 0;
        uhd().strvec_size(v, &n);
        for (size_t i = 0; i < n && i < 16; i++) {
            char buf[512] = {0};
            if (uhd().strvec_at(v, i, buf, sizeof buf) != 0) continue;
            const std::string args = buf;   // e.g. "type=b200,name=,serial=30C6...,product=B200"
            auto field = [&](const std::string& k) { const size_t p = args.find(k + "="); if (p == std::string::npos) return std::string(); const size_t e = args.find(',', p); return args.substr(p + k.size() + 1, e == std::string::npos ? std::string::npos : e - p - k.size() - 1); };
            DeviceInfo d;
            d.kind = DeviceInfo::Native;
            d.board = "usrp";
            d.nativeArgs = args;
            d.serial = field("serial");
            std::string prod = field("product");
            if (prod.empty()) prod = field("type");
            d.name = "USRP " + prod + (d.serial.empty() ? "" : " " + d.serial) + " (native, experimental)";
            d.maxRateHz = 56e6; d.minRateHz = 0.2e6;
            d.gainMinDb = 0; d.gainMaxDb = 76;
            out.push_back(d);
        }
    }
    uhd().strvec_free(&v);
}

} // namespace

// ------------------------------------------------------------------ public entry points

std::vector<DeviceInfo> listNativeDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    if (nativeDisabled()) return out;
    try {
        listRtl(out);
        listAirspy(out);
        listBlade(out);
        listLime(out);
        listPluto(out);
        listUsrp(out);
    } catch (const std::exception& e) {
        err = std::string("native radios: ") + e.what();
    }
    return out;
}

std::unique_ptr<IqSource> makeNativeSource(const DeviceInfo& d) {
    if (d.board == "rtlsdr") return std::make_unique<RtlSource>((uint32_t)strtoul(d.nativeArgs.c_str(), nullptr, 10));
    if (d.board == "airspy") return std::make_unique<AirspySource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 10));
    if (d.board == "bladerf") return std::make_unique<BladeSource>(d.nativeArgs);
    if (d.board == "lime") return std::make_unique<LimeSource>(d.nativeArgs);
    if (d.board == "pluto") return std::make_unique<PlutoSource>(d.nativeArgs);
    if (d.board == "usrp") return std::make_unique<UsrpSource>(d.nativeArgs);
    return nullptr;
}

} // namespace dect2
