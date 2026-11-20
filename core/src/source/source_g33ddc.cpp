// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Native driver for the WiNRADiO G33DDC (Excalibur Pro, 9 kHz to 50 MHz, a direct-sampling receiver with a hardware DDC): WiNRADiO's own
// G33DDCAPI.dll (Windows only; it ships in the G33DDC application's installer) is loaded at run time when it is installed. See native_common.h.
//
// The API's DDC1 is the wide I/Q stream: one of several fixed DDC types, each with a sample rate, a bandwidth and 16 or 32 bits per sample, centred
// on the frequency set with SetDDC1Frequency. This driver picks the DDC type with the smallest sample rate that is at least the rate asked for.
// The G33DDC's own DDC2 channels, demodulators and audio are not used: OnAir demodulates.
//
// Written from WiNRADiO's public API specification (winradio.com/home/g33ddc_sdk-doc.htm) WITHOUT a radio and without the header file: NOT TESTED on a
// real G33DDC. Two details come from the specification only in outline, and are marked below: the layout of G33DDC_DEVICE_INFO (only the first radio
// is used, whose serial number sits at a fixed offset) and the order of the members of G33DDC_CALLBACKS.
//
// The API is not thread-safe: "one receiver can be controlled from a single user thread only", and the handle OpenDevice returns can only be used in
// the thread that opened it. So every call for one radio goes through one thread of this driver (DevThread), whichever thread asks.
#include "dect2/common/diag.h"
#include "dect2/common/thread_name.h"
#include "native_common.h"
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>

namespace dect2 {
namespace native {

#if defined(_WIN32)
#define G33_CALL __stdcall
#else
#define G33_CALL
#endif

namespace {

typedef int32_t G33Bool;   // BOOL
// the callbacks the API makes (each from a thread of its own): only the DDC1 stream is used
typedef void(G33_CALL* G33IfCb)(const int16_t* buf, uint32_t n, uint16_t maxAmp, uint32_t adcRate, uintptr_t user);
typedef void(G33_CALL* G33Ddc1Cb)(const void* buf, uint32_t n, uint32_t bitsPerSample, uintptr_t user);
typedef G33Bool(G33_CALL* G33Ddc1PlayCb)(void* buf, uint32_t n, uint32_t bitsPerSample, uintptr_t user);
typedef void(G33_CALL* G33Ddc2Cb)(uint32_t channel, const float* buf, uint32_t n, uintptr_t user);
typedef void(G33_CALL* G33Ddc2PreCb)(uint32_t channel, const float* buf, uint32_t n, float peak, float rms, uintptr_t user);
typedef void(G33_CALL* G33AudioCb)(uint32_t channel, const float* buf, const float* filtered, uint32_t n, uintptr_t user);
typedef G33Bool(G33_CALL* G33AudioPlayCb)(uint32_t channel, float* buf, uint32_t n, uintptr_t user);

#pragma pack(push, 1)
// G33DDC_CALLBACKS: the members in the order the specification describes them (not confirmed against the header)
struct G33Callbacks {
    G33IfCb IFCallback = nullptr;
    G33Ddc1Cb DDC1StreamCallback = nullptr;
    G33Ddc1PlayCb DDC1PlaybackStreamCallback = nullptr;
    G33Ddc2Cb DDC2StreamCallback = nullptr;
    G33Ddc2PreCb DDC2PreprocessedStreamCallback = nullptr;
    G33AudioCb AudioStreamCallback = nullptr;
    G33AudioPlayCb AudioPlaybackStreamCallback = nullptr;
};
struct G3xDdcInfo { uint32_t SampleRate, Bandwidth, BitsPerSample; };   // G3XDDC_DDC_INFO
// the start of G33DDC_DEVICE_INFO: CHAR DevicePath[MAX_PATH]; BYTE InterfaceType; CHAR SerialNumber[...]; then channel and DDC type counts
struct G33DeviceInfoHead { char devicePath[260]; uint8_t interfaceType; char serial[64]; };
#pragma pack(pop)

constexpr uint32_t kBufSamples = 16384;   // I/Q sample sets per DDC1 buffer (a multiple of 64)
constexpr double kMaxHz = 50e6;
constexpr size_t kInfoStride = 1024;      // room per radio for GetDeviceList (the real structure is smaller)

struct G33Api {
    int32_t(G33_CALL* GetDeviceList)(void*, uint32_t) = nullptr;
    int32_t(G33_CALL* OpenDevice)(const char*) = nullptr;
    G33Bool(G33_CALL* CloseDevice)(int32_t) = nullptr;
    G33Bool(G33_CALL* IsDeviceConnected)(int32_t) = nullptr;
    G33Bool(G33_CALL* SetPower)(int32_t, G33Bool) = nullptr;
    G33Bool(G33_CALL* SetAttenuator)(int32_t, uint32_t) = nullptr;
    G33Bool(G33_CALL* SetPreselectors)(int32_t, uint32_t, uint32_t) = nullptr;
    G33Bool(G33_CALL* SetPreamp)(int32_t, G33Bool) = nullptr;
    G33Bool(G33_CALL* SetDithering)(int32_t, G33Bool) = nullptr;
    G33Bool(G33_CALL* GetDDCInfo)(int32_t, uint32_t, G3xDdcInfo*) = nullptr;
    G33Bool(G33_CALL* GetDDC1Count)(int32_t, uint32_t*) = nullptr;
    G33Bool(G33_CALL* SetDDC1)(int32_t, uint32_t) = nullptr;
    G33Bool(G33_CALL* SetDDC1Frequency)(int32_t, uint32_t) = nullptr;
    G33Bool(G33_CALL* StartDDC1)(int32_t, uint32_t) = nullptr;
    G33Bool(G33_CALL* StopDDC1)(int32_t) = nullptr;
    G33Bool(G33_CALL* SetCallbacks)(int32_t, const G33Callbacks*, uintptr_t) = nullptr;
    bool ok = false;
    DynLib lib;
    G33Api() {
        if (nativeDisabled() || !lib.open(libNames("g33ddcapi", {}, {"G33DDCAPI.dll"}))) return;
        ok = lib.get(GetDeviceList, "GetDeviceList") && lib.get(OpenDevice, "OpenDevice") && lib.get(CloseDevice, "CloseDevice") &&
             lib.get(SetPower, "SetPower") && lib.get(SetAttenuator, "SetAttenuator") && lib.get(SetPreselectors, "SetPreselectors") &&
             lib.get(SetPreamp, "SetPreamp") && lib.get(SetDithering, "SetDithering") && lib.get(GetDDCInfo, "GetDDCInfo") &&
             lib.get(GetDDC1Count, "GetDDC1Count") && lib.get(SetDDC1, "SetDDC1") && lib.get(SetDDC1Frequency, "SetDDC1Frequency") &&
             lib.get(StartDDC1, "StartDDC1") && lib.get(StopDDC1, "StopDDC1") && lib.get(SetCallbacks, "SetCallbacks");
        lib.opt(IsDeviceConnected, "IsDeviceConnected");
    }
};
G33Api& g33() { static G33Api a; return a; }

// The thread that owns one radio: every call of the API for it runs there, and the caller waits for the result.
class DevThread {
public:
    DevThread() : th_([this] { loop(); }) {}
    ~DevThread() {
        { std::lock_guard<std::mutex> lk(mu_); quit_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    template <class F> auto call(F&& f) -> decltype(f()) {
        using R = decltype(f());
        std::packaged_task<R()> t(std::forward<F>(f));
        auto fut = t.get_future();
        { std::lock_guard<std::mutex> lk(mu_); q_.emplace_back([&t] { t(); }); }
        cv_.notify_all();
        return fut.get();
    }

private:
    void loop() {
        dect2::setThreadName("onair-g33ddc");
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return quit_ || !q_.empty(); });
                if (q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool quit_ = false;
    std::thread th_;
};

std::mutex gMu;
int gOpen = 0;   // radios this program has open

struct DdcType { uint32_t rate = 0, bw = 0, bits = 0; };

// (on the thread that opened the radio) the DDC1 types
std::vector<DdcType> readDdcTypes(int32_t h) {
    std::vector<DdcType> v;
    uint32_t n = 0;
    if (!g33().GetDDC1Count(h, &n) || n == 0 || n > 64) return v;
    for (uint32_t i = 0; i < n; i++) {
        G3xDdcInfo in{};
        if (!g33().GetDDCInfo(h, i, &in) || in.SampleRate == 0) v.push_back(DdcType());
        else v.push_back({in.SampleRate, in.Bandwidth, in.BitsPerSample});
    }
    return v;
}

// the serial number of the first radio ("" when there is none); the layout of the device list is read as documented, see the top of the file
std::string firstSerial() {
    const int32_t count = g33().GetDeviceList(nullptr, 0);
    if (count <= 0 || count > 16) return std::string();
    std::vector<uint8_t> buf((size_t)count * kInfoStride, 0);
    if (g33().GetDeviceList(buf.data(), (uint32_t)buf.size()) <= 0) return std::string();
    const auto* h = reinterpret_cast<const G33DeviceInfoHead*>(buf.data());
    return trimmed(h->serial, sizeof h->serial);
}

std::vector<RadioSetting> g33Settings() {
    return {
        ppmSetting(0.01),
        choiceSetting("preamp", "Preamplifier",
                      "The input preamplifier of the radio. Off (the default) for the HF bands with strong signals; on for weak ones on the high bands.\n"
                      "The gain slider moves the input attenuator (21 dB at 0, none at 21), which is in front of it.",
                      {"off", "on"}, {"Off", "On"}, "off"),
        choiceSetting("dither", "ADC dithering",
                      "Noise added before the converter to spread its distortion. Off is the usual choice; on can help with a very strong signal.",
                      {"off", "on"}, {"Off", "On"}, "off")};
}

} // namespace

class G33Source : public NativeSource {
public:
    explicit G33Source(std::string serial) : serial_(std::move(serial)) {}
    ~G33Source() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (!g33().ok) { DIAG_ERROR(731904, "The WiNRADiO G33DDC library (G33DDCAPI.dll) could not be loaded, so the radio cannot be opened."); err = "WiNRADiO G33DDC: the radio's library (G33DDCAPI.dll) is missing"; openFailure_ = OpenFailure::Final; return false; }
        std::lock_guard<std::mutex> g(gMu);
        if (h_ >= 0) return true;
        thread_ = std::make_unique<DevThread>();
        const std::string sn = serial_;
        h_ = thread_->call([sn]() -> int32_t { return g33().OpenDevice(sn.c_str()); });
        if (h_ < 0) {
            h_ = -1;
            thread_.reset();
            DIAG_ERROR(284617, "Opening the WiNRADiO G33DDC %s failed, so the radio stays closed.", sn.c_str());
            err = "WiNRADiO G33DDC: cannot open the radio (in use by another program, or not connected)";
            return false;
        }
        gOpen++;
        const int32_t h = h_;
        const bool up = thread_->call([h, this]() -> bool {
            auto& a = g33();
            if (!a.SetPower(h, 1)) return false;
            a.SetPreselectors(h, 0, 50000000);   // the whole range: no band filter picked for the tuned frequency
            types_ = readDdcTypes(h);
            return !types_.empty();
        });
        if (!up) {
            DIAG_ERROR(590238, "The WiNRADiO G33DDC could not be switched on or reports no DDC types, so the radio is closed.");
            err = "WiNRADiO G33DDC: the radio did not start (power on or the list of DDC types failed)";
            closeDevice();
            return false;
        }
        fprintf(stderr, "WiNRADiO G33DDC: opened %s, %zu DDC types\n", serial_.c_str(), types_.size());
        fflush(stderr);
        type_ = -1; att_ = -1; preamp_ = dither_ = -1;
        return apply(s, err, false);
    }
    // A change of the DDC type (the sample rate) needs the stream stopped: NativeSource stops it for any change of the sample rate.
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::mutex> lk(api_);
        return apply(s, err, live);
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> g(gMu);
        std::lock_guard<std::mutex> lk(api_);
        if (h_ < 0) { thread_.reset(); return; }
        const int32_t h = h_;
        thread_->call([h]() -> bool {
            auto& a = g33();
            a.StopDDC1(h);
            a.SetCallbacks(h, nullptr, 0);
            a.SetPower(h, 0);
            return a.CloseDevice(h) != 0;
        });
        h_ = -1;
        thread_.reset();
        gOpen--;
    }
    // The samples arrive on a thread of the API: this one starts the stream and waits until it is stopped, or the radio is gone.
    void streamLoop() override {
        const int32_t h = h_;
        if (h < 0 || !run_) return;
        G33Callbacks cb;
        cb.DDC1StreamCallback = &G33Source::callback;
        const bool started = thread_->call([&]() -> bool {
            return g33().SetCallbacks(h, &cb, reinterpret_cast<uintptr_t>(this)) && g33().StartDDC1(h, kBufSamples);
        });
        if (!started) { DIAG(417065, "The WiNRADiO G33DDC refused to start the I/Q stream, so the source stops."); fprintf(stderr, "WiNRADiO G33DDC: the stream did not start\n"); fflush(stderr); return; }
        int idle = 0;
        while (run_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (++idle >= 20) {   // once a second: is the radio still there (unplugged, or the PCIe card reset)
                idle = 0;
                if (g33().IsDeviceConnected && !thread_->call([h]() -> bool { return g33().IsDeviceConnected(h) != 0; })) {
                    DIAG(863127, "The WiNRADiO G33DDC is no longer connected, so the sample stream ends.");
                    break;
                }
            }
        }
        thread_->call([h]() -> bool { g33().StopDDC1(h); g33().SetCallbacks(h, nullptr, 0); return true; });
    }

private:
    // (under api_) live: the stream is running, only the frequency, the gain and the settings may change
    bool apply(const TuneSettings& s, std::string& err, bool live) {
        if (h_ < 0) return false;
        const int32_t h = h_;
        auto& a = g33();
        if (!live) {
            // the DDC type with the smallest rate that is at least the one asked for (the widest when none is)
            const double want = s.sampleRate > 0 ? s.sampleRate : 0;
            int pick = -1;
            for (size_t i = 0; i < types_.size(); i++) {
                if (!types_[i].rate) continue;
                if (types_[i].rate >= want * 0.999 && (pick < 0 || types_[i].rate < types_[(size_t)pick].rate)) pick = (int)i;
            }
            if (pick < 0) for (size_t i = 0; i < types_.size(); i++) if (types_[i].rate && (pick < 0 || types_[i].rate > types_[(size_t)pick].rate)) pick = (int)i;
            if (pick < 0) { err = "WiNRADiO G33DDC: no DDC type to use"; return false; }
            if (pick != type_) {
                const bool ok = thread_->call([h, pick]() -> bool { return g33().SetDDC1(h, (uint32_t)pick) != 0; });
                if (!ok) { DIAG_ERROR(512906, "The WiNRADiO G33DDC refused DDC type %d, so the radio is not started.", pick); err = "WiNRADiO G33DDC: the DDC type for " + std::to_string(types_[(size_t)pick].rate) + " samples/s was refused"; return false; }
                type_ = pick;
            }
            bits_.store((int)types_[(size_t)type_].bits);
            rate_ = types_[(size_t)type_].rate;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = std::clamp(ppmCorrectedHz(std::max(s.centerHz, 0.0), radioPpm(s)), 0.0, kMaxHz);
        // the input attenuator in 3 dB steps: gain 21 dB is no attenuation
        const uint32_t att = (uint32_t)std::clamp(std::lround((21.0 - std::clamp(s.gainDb, 0.0, 21.0)) / 3.0), 0L, 7L) * 3;
        const int pre = radioOption(s, "preamp", "off") == "on" ? 1 : 0;
        const int dith = radioOption(s, "dither", "off") == "on" ? 1 : 0;
        const bool setAtt = (int)att != att_, setPre = pre != preamp_, setDith = dith != dither_;
        const bool ok = thread_->call([=]() -> bool {
            auto& api = g33();
            bool r = api.SetDDC1Frequency(h, (uint32_t)std::llround(f)) != 0;
            if (setAtt) api.SetAttenuator(h, att);
            if (setPre) api.SetPreamp(h, pre);
            if (setDith) api.SetDithering(h, dith);
            return r;
        });
        if (!ok) { DIAG_ERROR(906244, "The WiNRADiO G33DDC refused a centre frequency of %.0f Hz, so the tune fails.", f); err = "WiNRADiO G33DDC: the radio refused " + std::to_string((long long)f) + " Hz"; return false; }
        att_ = (int)att; preamp_ = pre; dither_ = dith;
        if (s.centerHz > kMaxHz) addNote(err, "the G33DDC is specified up to 50 MHz; the frequency was limited");
        (void)a;
        return true;
    }
    static void G33_CALL callback(const void* buf, uint32_t n, uint32_t bits, uintptr_t user) {
        auto* self = reinterpret_cast<G33Source*>(user);
        if (!self || !self->run_ || !buf || !n) return;
        self->convert(buf, n, bits);
    }
    // I and Q, signed little-endian 16 or 32 bits, to float with 1.0 as full scale
    void convert(const void* buf, size_t n, uint32_t bits) {
        conv_.resize(n);
        float* o = reinterpret_cast<float*>(conv_.data());
        if (bits == 16) {
            const int16_t* p = static_cast<const int16_t*>(buf);
            for (size_t i = 0; i < 2 * n; i++) o[i] = (float)p[i] * (1.f / 32768.f);
        } else {
            const int32_t* p = static_cast<const int32_t*>(buf);
            for (size_t i = 0; i < 2 * n; i++) o[i] = (float)((double)p[i] * (1.0 / 2147483648.0));
        }
        push(conv_.data(), conv_.size());
    }

    std::string serial_;
    int32_t h_ = -1;
    std::unique_ptr<DevThread> thread_;
    std::mutex api_;                 // the control calls of this radio (retune, open, close)
    std::vector<DdcType> types_;
    int type_ = -1, att_ = -1, preamp_ = -1, dither_ = -1;
    std::atomic<int> bits_{16};
    std::vector<cf32> conv_;
};

void listG33ddc(std::vector<DeviceInfo>& out) {
    if (!g33().ok) return;
    std::lock_guard<std::mutex> g(gMu);
    static std::vector<DeviceInfo> known;
    if (gOpen > 0 && !known.empty()) { for (const auto& d : known) out.push_back(d); return; }   // an open radio is not asked again
    known.clear();
    DevThread t;   // the calls for the listing, opened and closed on one thread
    const std::string sn = t.call([]() -> std::string { return firstSerial(); });
    if (sn.empty()) return;
    std::vector<DdcType> types;
    t.call([&]() -> bool {
        const int32_t h = g33().OpenDevice(sn.c_str());
        if (h < 0) return false;
        types = readDdcTypes(h);
        g33().CloseDevice(h);
        return true;
    });
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "g33ddc";
    d.serial = sn;
    d.nativeArgs = sn;
    d.name = "WiNRADiO G33DDC " + sn + " (experimental, untested)";
    double lo = 0, hi = 0;
    for (const auto& ty : types) {
        if (!ty.rate) continue;
        lo = lo == 0 ? ty.rate : std::min(lo, (double)ty.rate);
        hi = std::max(hi, (double)ty.rate);
        d.rateRanges.emplace_back((double)ty.rate, (double)ty.rate);
    }
    if (hi == 0) { lo = 48000; hi = 2e6; }   // the radio could not be asked: a plain range, the driver picks the nearest DDC type
    d.minRateHz = lo; d.maxRateHz = hi;
    d.gainMinDb = 0; d.gainMaxDb = 21;
    d.minFreqHz = 9e3; d.maxFreqHz = kMaxHz;
    d.settings = g33Settings();
    known.push_back(d);
    out.push_back(d);
}

std::unique_ptr<IqSource> makeG33ddc(const DeviceInfo& d) {
    return std::make_unique<G33Source>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
