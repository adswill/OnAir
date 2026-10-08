// Native driver for the PlutoSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include <cerrno>
#include <map>

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
    long long (DECT2_CALL* channel_attr_read)(const iio_channel*, const char*, char*, size_t) = nullptr;   // optional
    void (DECT2_CALL* strerror_)(int, char*, size_t) = nullptr;                                            // optional
    // optional: device attributes (the AD9361's reference clock, "xo_correction", for the frequency correction)
    long long (DECT2_CALL* device_attr_read)(const iio_device*, const char*, char*, size_t) = nullptr;
    int (DECT2_CALL* device_attr_write_longlong)(const iio_device*, const char*, long long) = nullptr;
    bool ok = false;
    bool v1 = false;   // libiio 1.x: another API (iio_create_context(params, uri), blocks instead of buffers), not supported yet
    DynLib lib;
    IioApi() {
        if (nativeDisabled() || !lib.open(libNames("iio", {".0", ".1"}, {"libiio.dll", "iio.dll"}))) return;
        {
            void (DECT2_CALL* get_version)(unsigned*, unsigned*, char*) = nullptr;
            void* (DECT2_CALL* scan1)(const void*, const char*) = nullptr;
            unsigned major = 0, minor = 0;
            char tag[8] = {0};
            if (lib.opt(get_version, "iio_library_get_version")) get_version(&major, &minor, tag);
            if (major >= 1 || (!lib.opt(create_scan_context, "iio_create_scan_context") && lib.opt(scan1, "iio_scan"))) {
                v1 = true;
                fprintf(stderr, "%s\n", kV1Msg);
                fflush(stderr);
                return;
            }
        }
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
        if (ok) {
            lib.opt(channel_attr_read, "iio_channel_attr_read"); lib.opt(strerror_, "iio_strerror");
            lib.opt(device_attr_read, "iio_device_attr_read"); lib.opt(device_attr_write_longlong, "iio_device_attr_write_longlong");
        }
    }
    // libiio reports failures as negative errno values (or NULL and errno)
    std::string text(int code) const {
        const int e = code < 0 ? -code : code;
        char b[200] = {0};
        if (strerror_) strerror_(e, b, sizeof b);
        else snprintf(b, sizeof b, "%s", strerror(e));
        return std::string(b) + " (" + std::to_string(e) + ")";
    }
    static constexpr const char* kV1Msg = "libiio 1.x found: OnAir needs libiio 0.23-0.26 for now";
};
IioApi& iio() { static IioApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

constexpr double kUsbMaxRate = 4e6;   // USB 2: the continuous rate a Pluto keeps up without losing samples (measured)
// The link runs over the Pluto's USB cable: "usb:...", and its default network address ip:192.168.2.1 (pluto.local), which is the USB-gadget
// Ethernet on the same USB 2 cable and keeps up with even less than libusb does. A Pluto on a real network (another address) may go faster.
bool overUsbCable(const std::string& uri) { return uri.rfind("usb:", 0) == 0 || uri == "ip:192.168.2.1" || uri == "ip:pluto.local"; }

// The RX LO range the radio reported when it was last opened (stock AD9363 firmware: 325-3800 MHz, AD9364 mode: 70-6000 MHz)
std::mutex gRangeMu;
std::map<std::string, std::pair<double, double>> gRange;
// The serial number of the Pluto at each USB URI, from the list: a USB URI is "usb:<bus>.<device address>.<interface>", and the address
// changes every time the radio is plugged in again
std::map<std::string, std::string> gSerial;

// "0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), serial=104473..." -> "104473..."
std::string serialIn(const std::string& desc) {
    const size_t p = desc.find("serial=");
    if (p == std::string::npos) return {};
    std::string s = desc.substr(p + 7);
    const size_t e = s.find_first_of(" ,)");
    if (e != std::string::npos) s.resize(e);
    return s;
}

// The USB URI the Pluto with this serial number has now ("" when it is not there)
std::string usbUriOf(const std::string& serial) {
    std::string found;
    if (serial.empty()) return found;
    if (IioApi::iio_scan_context* sc = iio().create_scan_context("usb", 0)) {
        IioApi::iio_context_info** info = nullptr;
        const auto n = iio().scan_context_get_info_list(sc, &info);
        for (long i = 0; i < (long)n && found.empty(); i++) {
            const char* desc = iio().context_info_get_description(info[i]);
            const char* uri = iio().context_info_get_uri(info[i]);
            if (desc && uri && serialIn(desc) == serial) found = uri;
        }
        if (info) iio().context_info_list_free(info);
        iio().scan_context_destroy(sc);
    }
    return found;
}

// "[-3 1 71]" (first, step, last) -> the first and last value (a and b stay as they are when s is not such a range)
bool rangeIn(const char* s, double& a, double& b) {
    double x = 0, st = 0, y = 0;
    if (sscanf(s, " [ %lf %lf %lf ]", &x, &st, &y) != 3 || !(y > x)) return false;
    a = x; b = y;
    return true;
}

} // namespace

class PlutoSource : public NativeSource {
public:
    explicit PlutoSource(std::string uri) : uri_(std::move(uri)) {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto it = gSerial.find(uri_);
        if (it != gSerial.end()) serial_ = it->second;
    }
    ~PlutoSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (iio().v1) { err = IioApi::kV1Msg; openFailure_ = OpenFailure::Final; return false; }
        errno = 0;
        ctx_ = iio().create_context_from_uri(uri_.c_str());
        if (!ctx_ && uri_.rfind("usb:", 0) == 0 && !serial_.empty()) {
            // plugged in again (or the USB bus was reset): the same radio has another USB address now, found by its serial number
            const std::string now = usbUriOf(serial_);
            if (!now.empty() && now != uri_) {
                fprintf(stderr, "PlutoSDR %s is now at %s\n", serial_.c_str(), now.c_str());
                fflush(stderr);
                uri_ = now;
                errno = 0;
                ctx_ = iio().create_context_from_uri(uri_.c_str());
            }
        }
        if (!ctx_) {
            const int e = errno;
            err = "PlutoSDR: cannot connect (" + uri_ + ")" + (e ? ": " + iio().text(e) : std::string());
            if (kOnWindows && uri_.rfind("usb:", 0) == 0) err += std::string(". ") + windowsUsbDriverHint();
            return false;
        }
        iio().context_set_timeout(ctx_, 3000);
        phy_ = iio().context_find_device(ctx_, "ad9361-phy");
        rx_ = iio().context_find_device(ctx_, "cf-ad9361-lpc");
        if (!phy_ || !rx_) { err = "PlutoSDR: the AD9361 devices were not found"; return false; }
        rxCh_ = iio().device_find_channel(phy_, "voltage0", false);
        lo_ = iio().device_find_channel(phy_, "altvoltage0", true);
        i_ = iio().device_find_channel(rx_, "voltage0", false);
        q_ = iio().device_find_channel(rx_, "voltage1", false);
        if (!rxCh_ || !lo_ || !i_ || !q_) { err = "PlutoSDR: the receive channels were not found"; return false; }
        // the reference clock the driver computes with (Hz; 40 MHz plus the radio's stored calibration): the frequency correction moves it
        xoBase_ = 0; xoPpm_ = 0;
        gainMode_.clear();
        if (iio().device_attr_read && iio().device_attr_write_longlong) {
            char v[64] = {0};
            double hz = 0;
            if (iio().device_attr_read(phy_, "xo_correction", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &hz) == 1 && hz > 1e6) xoBase_ = hz;
        }
        loHz_ = 70e6; hiHz_ = 6e9;
        char avail[128] = {0};   // "[70000000 1 6000000000]": first, step, last
        if (iio().channel_attr_read && iio().channel_attr_read(lo_, "frequency_available", avail, sizeof avail - 1) > 0) {
            double a = 0, b = 0;
            if (rangeIn(avail, a, b)) {
                loHz_ = a; hiHz_ = b;
                std::lock_guard<std::mutex> lk(gRangeMu);
                gRange[uri_] = {a, b};
            }
        }
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!ctx_) return false;
        if (!live) {
            if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
            // the converter runs 0.52 - 61.44 Msps; below 2.083 Msps it needs a decimation filter: stay at or above that
            const double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 2.1e6), 61.44e6);
            if (const int r = iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(want)); r != 0) { err = "PlutoSDR: the sample rate was refused: " + iio().text(r); return false; }
            if (overUsbCable(uri_) && want > kUsbMaxRate * 1.01) {
                fprintf(stderr, "PlutoSDR over USB 2 keeps up with about %.0f Msps; %s Msps will lose samples\n", kUsbMaxRate / 1e6, mhz(want).c_str());
                fflush(stderr);
                if (err.empty()) err = "PlutoSDR over its USB cable keeps up with about 4 Msps; " + mhz(want) + " Msps will lose samples";
            }
            iio().channel_attr_write_longlong(rxCh_, "rf_bandwidth", (long long)std::llround(std::min(56e6, std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : want * 0.95))));
            rate_ = want;
            {   // the rate the converter clocks really give (the receiver is set up with what the radio reports)
                char v[64] = {0};
                double got = 0;
                if (iio().channel_attr_read && iio().channel_attr_read(rxCh_, "sampling_frequency", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &got) == 1 && got > 0) rate_ = got;
            }
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            iio().channel_enable(i_);
            iio().channel_enable(q_);
            buf_ = iio().device_create_buffer(rx_, kBlock, false);
            if (!buf_) { err = "PlutoSDR: cannot allocate the sample buffer"; return false; }
        }
        // the gain control: manual, with OnAir's gain (the default), or the AD9361's own AGC (TuneSettings "gainmode": slow_attack, fast_attack)
        const std::string gm = radioOption(s, "gainmode", "manual");
        if (!live || gm != gainMode_) {
            if (iio().channel_attr_write(rxCh_, "gain_control_mode", gm.c_str()) >= 0) gainMode_ = gm;
            else { fprintf(stderr, "PlutoSDR: the gain mode %s was refused\n", gm.c_str()); fflush(stderr); }
        }
        // Frequency correction: the reference clock the driver computes with is moved by the clock error (a clock that runs fast by ppm is
        // told so), which corrects the tuning and the sample rate; a driver without xo_correction: the frequency asked for
        const double ppm = radioPpm(s);
        double tuneHz = s.centerHz;
        if (xoBase_ > 0) {
            if (ppm != xoPpm_) {
                if (iio().device_attr_write_longlong(phy_, "xo_correction", std::llround(xoBase_ * (1.0 + ppm * 1e-6))) == 0) xoPpm_ = ppm;
                else { fprintf(stderr, "PlutoSDR: the reference clock correction of %.2f ppm was refused\n", ppm); fflush(stderr); }
            }
            if (ppm != xoPpm_) tuneHz = ppmCorrectedHz(s.centerHz, ppm - xoPpm_);
        } else tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        const double f = s.centerHz;
        if (f < loHz_ || f > hiHz_ || iio().channel_attr_write_longlong(lo_, "frequency", (long long)std::llround(tuneHz)) != 0) {
            err = "PlutoSDR could not tune to " + mhz(f) + " MHz (range " + mhz(loHz_) + "-" + mhz(hiHz_) + " MHz)";
            if (hiHz_ <= 3.8e9) err += ": the stock firmware tunes 325-3800 MHz, 70-6000 MHz needs the AD9364 setting (fw_setenv attr_name compatible; fw_setenv attr_val ad9364)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        // The highest gain depends on the frequency (the AD9361's gain tables: 73 dB below 1300 MHz, 71 dB to 4000 MHz, 62 dB above), and a
        // gain above it is refused (EINVAL) with the old gain left in place: the driver's own range for this frequency, else the tables
        double gmin = -1, gmax = f < 1.3e9 ? 73 : f < 4e9 ? 71 : 62;
        {
            char v[64] = {0};
            if (iio().channel_attr_read && iio().channel_attr_read(rxCh_, "hardwaregain_available", v, sizeof v - 1) > 0) rangeIn(v, gmin, gmax);
        }
        const double g = std::min(gmax, std::max(std::max(0.0, gmin), s.gainDb));
        if (gainMode_ != "manual" && !gainMode_.empty()) return true;   // the radio's AGC sets the gain (hardwaregain is for manual mode only)
        if (const int r = iio().channel_attr_write_longlong(rxCh_, "hardwaregain", (long long)std::llround(g)); r != 0) {
            fprintf(stderr, "PlutoSDR: the gain %.0f dB was refused at %s MHz: %s\n", g, mhz(f).c_str(), iio().text(r).c_str());
            fflush(stderr);
        }
        return true;
    }
    void closeDevice() override {
        // the Pluto keeps its settings after OnAir lets go of it: the reference clock goes back to what it was
        if (phy_ && xoBase_ > 0 && xoPpm_ != 0) iio().device_attr_write_longlong(phy_, "xo_correction", std::llround(xoBase_));
        xoPpm_ = 0;
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
    std::string serial_;   // USB: finds the radio again when its URI changed
    IioApi::iio_context* ctx_ = nullptr;
    IioApi::iio_device* phy_ = nullptr;
    IioApi::iio_device* rx_ = nullptr;
    IioApi::iio_channel* rxCh_ = nullptr;
    IioApi::iio_channel* lo_ = nullptr;
    IioApi::iio_channel* i_ = nullptr;
    IioApi::iio_channel* q_ = nullptr;
    IioApi::iio_buffer* buf_ = nullptr;
    double loHz_ = 70e6, hiHz_ = 6e9;
    double xoBase_ = 0, xoPpm_ = 0;   // the reference clock at open (0 = cannot be corrected) and the correction applied to it
    std::string gainMode_;            // the gain_control_mode written
};

void listPluto(std::vector<DeviceInfo>& out) {
    if (!iio().ok && !iio().v1) return;
    auto add = [&](const std::string& uri, const std::string& desc) {
        for (const auto& d : out) if (d.board == "pluto" && d.nativeArgs == uri) return;
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "pluto";
        d.nativeArgs = uri;
        d.name = "PlutoSDR " + (desc.empty() ? uri : "(" + uri + ")") + " (native, experimental)";
        // over USB 2 (and its USB-gadget network address) a Pluto keeps up with about 4 Msps; over a real network the converter's full rate is offered
        d.maxRateHz = overUsbCable(uri) ? kUsbMaxRate : 61.44e6; d.minRateHz = 2.1e6;
        d.gainMinDb = 0; d.gainMaxDb = 73;
        d.minFreqHz = 70e6; d.maxFreqHz = 6e9;
        d.settings = {ppmSetting(0.01),
                      choiceSetting("gainmode", "Gain mode",
                                    "Manual: the gain slider (and OnAir's AGC) set the AD9361's gain (the default).\n"
                                    "Slow / fast attack: the AD9361's own AGC sets it (fast attack for bursts); the gain slider then has no effect.",
                                    {"manual", "slow_attack", "fast_attack"}, {"Manual", "Radio AGC, slow attack", "Radio AGC, fast attack"}, "manual")};
        {
            std::lock_guard<std::mutex> lk(gRangeMu);
            auto it = gRange.find(uri);
            if (it != gRange.end()) { d.minFreqHz = it->second.first; d.maxFreqHz = it->second.second; }
            const std::string sn = serialIn(desc);
            if (!sn.empty()) gSerial[uri] = sn;
        }
        out.push_back(d);
    };
    if (IioApi::iio_scan_context* sc = iio().v1 ? nullptr : iio().create_scan_context("usb", 0)) {
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
