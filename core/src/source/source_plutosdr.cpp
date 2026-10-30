// Native driver for the PlutoSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "dect2/engine/rate_choice.h"
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
    long long (DECT2_CALL* device_attr_write)(const iio_device*, const char*, const char*) = nullptr;   // optional: the AD9361's FIR filter is loaded as text
    // optional: the context attributes, where the firmware names itself ("fw_version", "tezuka-v0.3.21" on a Tezuka)
    const char* (DECT2_CALL* context_get_attr_value)(const iio_context*, const char*) = nullptr;
    int (DECT2_CALL* device_set_kernel_buffers_count)(const iio_device*, unsigned) = nullptr;   // optional: more buffers queued in the kernel
    long long (DECT2_CALL* buffer_push)(iio_buffer*) = nullptr;   // optional: sends a transmit buffer (without it the radio cannot transmit here)
    bool ok = false;
    bool v1 = false;   // libiio 1.x: another API (iio_create_context(params, uri), blocks instead of buffers), not supported yet
    DynLib lib;
    IioApi() {
        std::vector<std::string> names = libNames("iio", {".0", ".1"}, {"libiio.dll", "iio.dll"});
#if defined(__APPLE__)
        names.insert(names.begin(), "/Library/Frameworks/iio.framework/iio");   // Analog Devices' macOS installer ships libiio as a framework
#endif
        if (nativeDisabled() || !lib.open(names)) return;
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
            lib.opt(device_attr_read, "iio_device_attr_read"); lib.opt(device_attr_write_longlong, "iio_device_attr_write_longlong"); lib.opt(device_attr_write, "iio_device_attr_write");
            lib.opt(context_get_attr_value, "iio_context_get_attr_value");
            lib.opt(device_set_kernel_buffers_count, "iio_device_set_kernel_buffers_count");
            lib.opt(buffer_push, "iio_buffer_push");
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
#if defined(__APPLE__)
constexpr const char* kNoIioMsg = "PlutoSDR: libiio is not installed. Install libiio 0.26 (Analog Devices' macOS package, which puts iio.framework in "
                                  "/Library/Frameworks, or Homebrew), then start OnAir again";
#else
constexpr const char* kNoIioMsg = "PlutoSDR: libiio is not installed (OnAir needs libiio 0.23-0.26)";
#endif
IioApi& iio() { static IioApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

// Below 2.083 Msps the AD9361 needs its decimating FIR: the converter runs at twice the rate and the FIR (16 taps is all its clock chain allows
// here) halves it. This one is a lowpass at 0.23 of the converter rate (0.46 of the output): -1 dB at 0.38, 42 dB down from 0.61. It gives DAB its
// 2.048 Msps straight from the radio: no resampling in software, half the data over the cable. Taps are in 1/32768 (the same for RX and TX).
constexpr long long kFirTaps[16] = {-282, -25, 891, 389, -2075, -1732, 5360, 13694, 13694, 5360, -1732, -2075, 389, 891, -25, -282};
constexpr double kFirMaxRate = 2.1e6, kFirMinRate = 2.048e6;   // the output rates the FIR is used for (the converter itself runs 2.083 Msps and up)
bool wantsFir(double rateHz) { return rateHz >= kFirMinRate - 1 && rateHz < kFirMaxRate; }
std::string firConfig() {
    std::string c = "RX 3 GAIN 0 DEC 2\nTX 3 GAIN 0 INT 2\n";
    for (long long t : kFirTaps) c += std::to_string(t) + "," + std::to_string(t) + "\n";
    return c;
}

constexpr double kUsbMaxRate = 4e6;   // USB 2: the continuous rate a Pluto keeps up without losing samples (measured)
constexpr double kUsbNetRate = 3e6;   // the same cable as a USB-gadget network (ip:192.168.2.1, pluto.local): iiod over it keeps up with less
// The link runs over the Pluto's USB cable: "usb:...", and its default network address ip:192.168.2.1 (pluto.local), which is the USB-gadget
// Ethernet on the same USB 2 cable and keeps up with even less than libusb does. A Pluto on a real network (another address) may go faster.
bool defaultAddress(const std::string& uri) { return uri == "ip:192.168.2.1" || uri == "ip:pluto.local"; }
bool overUsbCable(const std::string& uri) { return uri.rfind("usb:", 0) == 0 || defaultAddress(uri); }
// the steady rate of the link at uri (0 = a real network, not limited here): the USB-gadget network carries less than libusb, so a mode that
// fits in a few Msps (DVB-S/S2 of the 1.5 Msym/s QO-100 beacon, #59) is not run faster than it keeps up with there
double usbLinkRate(const std::string& uri) { return !overUsbCable(uri) ? 0 : uri.rfind("usb:", 0) == 0 ? kUsbMaxRate : kUsbNetRate; }
// The "Link" setting of a Pluto at its default network address, which a Pluto+ (real Gigabit Ethernet) shares with the USB-gadget link:
// Ethernet lifts the cap, USB cable holds it, Auto guesses from the address (USB gadget). #96
double usbLinkRate(const std::string& uri, const std::string& link) {
    if (link == "ethernet" && defaultAddress(uri)) return 0;
    if (link == "usb" && defaultAddress(uri)) return kUsbNetRate;
    return usbLinkRate(uri);
}

// Whether the board at each URI runs Tezuka firmware (known once it was opened or probed): only then the IQ format can be chosen
std::map<std::string, bool> gTezuka;
bool isTezukaFw(const char* fw) {
    if (!fw) return false;
    std::string low = fw;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low.find("tezuka") != std::string::npos;
}

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

namespace {
// RX and TX share the USB handle and the AD936x clock tree. Only configuration and
// teardown take this lock; refill/push on the two different DMA devices run in parallel.
std::recursive_mutex gPlutoMu;
struct PlutoRadio {
    IioApi::iio_context* ctx = nullptr;
    IioApi::iio_device* phy = nullptr;
    bool rx = false, tx = false, rxFir = false, rxCs8 = false;
    double rxRate = 0, txRate = 0;
    double xoBase = 0, xoPpm = 0;   // original reference clock, restored only after the last direction closes
    ~PlutoRadio() {
        if (phy && xoBase > 0 && xoPpm != 0)
            iio().device_attr_write_longlong(phy, "xo_correction", std::llround(xoBase));
        if (ctx) iio().context_destroy(ctx);
    }
    bool duplex(std::string& err) const {
        char mode[64] = {0};
        if (!iio().device_attr_read || iio().device_attr_read(phy, "ensm_mode", mode, sizeof mode - 1) <= 0 ||
            std::string(mode).find("fdd") != 0 || (mode[3] && !isspace((unsigned char)mode[3]))) {
            err = "PlutoSDR: simultaneous RX/TX requires firmware in FDD mode";
            return false;
        }
        return true;
    }
};
std::map<std::string, std::weak_ptr<PlutoRadio>> gPlutoRadios;
std::shared_ptr<PlutoRadio> openPluto(std::string& uri, const std::string& serial) {
    const std::string key = serial.empty() ? uri : "serial:" + serial;
    if (auto active = gPlutoRadios[key].lock()) return active;
    auto r = std::make_shared<PlutoRadio>();
    errno = 0;
    r->ctx = iio().create_context_from_uri(uri.c_str());
    if (!r->ctx && uri.rfind("usb:", 0) == 0 && !serial.empty()) {
        const std::string now = usbUriOf(serial);
        if (!now.empty() && now != uri) {
            uri = now; errno = 0;
            r->ctx = iio().create_context_from_uri(uri.c_str());
        }
    }
    if (!r->ctx) return {};
    iio().context_set_timeout(r->ctx, 3000);
    r->phy = iio().context_find_device(r->ctx, "ad9361-phy");
    if (r->phy && iio().device_attr_read && iio().device_attr_write_longlong) {
        char v[64] = {0};
        double hz = 0;
        if (iio().device_attr_read(r->phy, "xo_correction", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &hz) == 1 && hz > 1e6) r->xoBase = hz;
    }
    for (auto it = gPlutoRadios.begin(); it != gPlutoRadios.end();)
        it = it->second.expired() ? gPlutoRadios.erase(it) : std::next(it);
    gPlutoRadios[key] = r;
    return r;
}
// The second direction may use an already faster clock, but must not change it.
bool duplexRate(double need, double active, std::string& err) {
    if (need <= active + 1) return true;
    err = "PlutoSDR: simultaneous RX/TX shares one sample rate; stop the other direction before raising it to " +
          std::to_string(need / 1e6) + " Msps (running at " + std::to_string(active / 1e6) + " Msps)";
    return false;
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
        std::lock_guard<std::recursive_mutex> lk(gPlutoMu);
        // Address-based entries can exist even when libiio is missing.
        if (!iio().ok) { err = kNoIioMsg; openFailure_ = OpenFailure::Final; return false; }
        radio_ = openPluto(uri_, serial_);
        ctx_ = radio_ ? radio_->ctx : nullptr;
        if (!ctx_) {
            const int e = errno;
            err = "PlutoSDR: cannot connect (" + uri_ + ")" + (e ? ": " + iio().text(e) : std::string());
            if (kOnWindows && uri_.rfind("usb:", 0) == 0) err += std::string(". ") + windowsUsbDriverHint();
            return false;
        }
        if (radio_->rx) { err = "PlutoSDR: the receive stream is already open"; openFailure_ = OpenFailure::Final; return false; }
        radio_->rx = ownsRx_ = true;
        phy_ = radio_->phy;
        rx_ = iio().context_find_device(ctx_, "cf-ad9361-lpc");
        if (!phy_ || !rx_) { err = "PlutoSDR: the AD9361 devices were not found"; return false; }
        rxCh_ = iio().device_find_channel(phy_, "voltage0", false);
        lo_ = iio().device_find_channel(phy_, "altvoltage0", true);
        i_ = iio().device_find_channel(rx_, "voltage0", false);
        q_ = iio().device_find_channel(rx_, "voltage1", false);
        if (!rxCh_ || !lo_ || !i_ || !q_) { err = "PlutoSDR: the receive channels were not found"; return false; }
        gainMode_.clear();
        fw_.clear(); tezuka_ = false;
        if (iio().context_get_attr_value) {
            if (const char* fw = iio().context_get_attr_value(ctx_, "fw_version")) {
                fw_ = fw;
                tezuka_ = isTezukaFw(fw);
                { std::lock_guard<std::mutex> lk(gRangeMu); gTezuka[uri_] = tezuka_; }
                fprintf(stderr, "PlutoSDR firmware: %s%s\n", fw_.c_str(), tezuka_ ? " (Tezuka)" : "");
                fflush(stderr);
            }
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
    bool canRetune(const TuneSettings& s, std::string& err) override {
        std::lock_guard<std::recursive_mutex> lk(gPlutoMu);
        if (!radio_ || !radio_->tx) return true;
        if (tezuka_ && radioOption(s, "iqformat", "cs16") == "cs8") {
            err = "PlutoSDR: simultaneous RX/TX requires CS16; select CS16 in IQ format";
            return false;
        }
        if (wantsFir(s.sampleRate) && radioFlag(s, "lowratefir")) {
            err = "PlutoSDR: disable Low-rate FIR (RX only) to receive while TX is running";
            return false;
        }
        const double want = s.rateChosen && s.sampleRate > 0 ? s.sampleRate : std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 2.1e6), 61.44e6);   // (3.4: a chosen rate goes to the AD9361 as it is)
        return duplexRate(want, radio_->txRate, err) && radio_->duplex(err);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        std::lock_guard<std::recursive_mutex> lk(gPlutoMu);
        retuneRestored_ = false;
        if (!ctx_) return false;
        if (!canRetune(s, err)) {
            // A transmitter may have started after retune's preflight and before
            // the RX thread stopped. Recreate its cancelled buffer at the old rate.
            if (!live && buf_) {
                iio().buffer_destroy(buf_); buf_ = nullptr; createBuffer();
                retuneRestored_ = buf_ != nullptr;
            }
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (!live) {
            if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
            // the converter runs 0.52 - 61.44 Msps; below 2.083 Msps it needs a decimation filter: stay at or above that
            double want = s.rateChosen && s.sampleRate > 0 ? s.sampleRate : std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 2.1e6), 61.44e6);   // (3.4: a chosen rate goes to the AD9361 as it is)
            if (radio_->tx) want = radio_->txRate;
            if (firOn_) setFir(false);   // a rate chosen now may not be one for the FIR
            if (wantsFir(s.sampleRate) && radioFlag(s, "lowratefir")) {
                // 2.048 Msps: the converter at twice that, the FIR (loaded and switched on there) halves it
                if (setFirRate(s.sampleRate)) want = s.sampleRate;
                else { fprintf(stderr, "PlutoSDR: the radio did not accept the decimating FIR: running at 2.1 Msps, resampled in software\n"); fflush(stderr); }
            }
            if (!firOn_ && !radio_->tx)
                if (const int r = iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(want)); r != 0) { err = "PlutoSDR: the sample rate was refused: " + iio().text(r); return false; }
            // Tezuka: with only the first channel enabled the FPGA sends 8-bit I/Q pairs (half the bytes on the cable, twice the rate); both: 16-bit
            cs8_ = tezuka_ && radioOption(s, "iqformat", "cs16") == "cs8";
            if (const double cap = usbLinkRate(uri_, radioOption(s, "link", "auto")); cap > 0) {
                const std::string n = usbLinkNote(want, cap * (cs8_ ? 2 : 1), tezuka_, cs8_);
                if (!n.empty()) { fprintf(stderr, "%s\n", n.c_str()); fflush(stderr); if (err.empty()) err = n; }
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
            if (cs8_) iio().channel_disable(q_); else iio().channel_enable(q_);
            // 8 kernel buffers (as SDR++ does) to ride out a late reader, on every firmware (libiio 0.x has the call, 1.x does not)
            if (iio().device_set_kernel_buffers_count) iio().device_set_kernel_buffers_count(rx_, kKernelBuffers);
            // over a network every refill is a round trip to iiod: a buffer of ~25 ms keeps 16 Msps and more flowing (#114); USB keeps 64k
            block_ = uri_.rfind("ip:", 0) == 0 ? netBlock(rate_) : kBlock;
            createBuffer();
            if (!buf_) { err = "PlutoSDR: cannot allocate the sample buffer"; return false; }
            radio_->rxRate = rate_;
            radio_->rxFir = firOn_;
            radio_->rxCs8 = cs8_;
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
        if (radio_->xoBase > 0) {
            if (ppm != radio_->xoPpm && !radio_->tx) {
                if (iio().device_attr_write_longlong(phy_, "xo_correction", std::llround(radio_->xoBase * (1.0 + ppm * 1e-6))) == 0) radio_->xoPpm = ppm;
                else { fprintf(stderr, "PlutoSDR: the reference clock correction of %.2f ppm was refused\n", ppm); fflush(stderr); }
            }
            if (ppm != radio_->xoPpm) tuneHz = ppmCorrectedHz(s.centerHz, ppm) * (1.0 + radio_->xoPpm * 1e-6);
        } else tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        const double f = s.centerHz;
        // outside what the radio is specified for it is asked all the same: the AD936x driver decides (it refuses a frequency its PLL cannot make)
        const std::string spec = freqSpecNote("PlutoSDR", f, loHz_, hiHz_);
        if (iio().channel_attr_write_longlong(lo_, "frequency", (long long)std::llround(std::max(tuneHz, 0.0))) != 0) {
            err = "PlutoSDR: the driver refused " + mhz(f) + " MHz (specified for " + mhz(loHz_) + "-" + mhz(hiHz_) + " MHz)";
            if (hiHz_ <= 3.8e9) err += ": the stock firmware tunes 325-3800 MHz, 70-6000 MHz needs the AD9364 setting (fw_setenv attr_name compatible; fw_setenv attr_val ad9364)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        addNote(err, spec);
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
    // Switches the AD9361's FIR on or off (it is one switch for both directions); only ever turned off when this source turned it on
    void setFir(bool on) {
        if (on == firOn_) return;
        if (iio().channel_attr_write_longlong(rxCh_, "filter_fir_en", on ? 1 : 0) == 0) firOn_ = on;
        else if (!on) firOn_ = false;
    }
    // The FIR for `outHz`: converter at 2 * outHz with the filter off, filter loaded, switched on, then the output rate. False if the radio refuses
    bool setFirRate(double outHz) {
        if (!iio().device_attr_write || !phy_) return false;
        if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(2 * outHz)) != 0) return false;
        const std::string cfg = firConfig();
        if (iio().device_attr_write(phy_, "filter_fir_config", cfg.c_str()) < 0) return false;
        if (iio().channel_attr_write_longlong(rxCh_, "filter_fir_en", 1) != 0) return false;
        firOn_ = true;
        if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(outHz)) != 0) { setFir(false); return false; }
        return true;
    }
    void closeDevice() override {
        std::lock_guard<std::recursive_mutex> lk(gPlutoMu);
        if (firOn_ && rxCh_) {   // the radio goes back to a rate it can run without the FIR
            iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(2 * rate_));
            setFir(false);
        }
        if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
        if (i_) iio().channel_disable(i_);
        if (q_) iio().channel_disable(q_);
        if (radio_ && ownsRx_) { radio_->rx = false; radio_->rxRate = 0; radio_->rxFir = radio_->rxCs8 = false; }
        ownsRx_ = false;
        radio_.reset();
        ctx_ = nullptr; phy_ = rx_ = nullptr; rxCh_ = lo_ = i_ = q_ = nullptr;
    }
    void streamLoop() override {
        if (!buf_) return;   // a failed buffer restore is retried by NativeSource's reconnect loop
        conv_.resize(block_);
        int failures = 0;
        while (run_) {
            const auto got = iio().buffer_refill(buf_);
            if (got <= 0) { if (++failures > 20) { fprintf(stderr, "PlutoSDR: stream error, stopping the source\n"); break; } continue; }
            failures = 0;
            size_t n;
            if (cs8_) {
                const int8_t* p = static_cast<const int8_t*>(iio().buffer_start(buf_));
                n = std::min<size_t>((size_t)got / 2, block_);   // 2 bytes per I/Q sample
                for (size_t k = 0; k < n; k++) conv_[k] = cf32(p[2 * k] / 128.f, p[2 * k + 1] / 128.f);
            } else {
                const int16_t* p = static_cast<const int16_t*>(iio().buffer_start(buf_));
                n = std::min<size_t>((size_t)got / 4, block_);   // 4 bytes per I/Q sample
                for (size_t k = 0; k < n; k++) conv_[k] = cf32(p[2 * k] / 2048.f, p[2 * k + 1] / 2048.f);   // 12-bit samples in 16 bits
            }
            push(conv_.data(), n);
        }
    }
    void interrupt() override { if (buf_) iio().buffer_cancel(buf_); }
    bool resumeAfterConfigureFailure() const override { return retuneRestored_; }
    bool retuneRestored_ = false;

private:
    void createBuffer() {
        buf_ = iio().device_create_buffer(rx_, block_, false);
        if (!buf_ && block_ > kBlock) buf_ = iio().device_create_buffer(rx_, block_ = kBlock, false);
    }
    std::shared_ptr<PlutoRadio> radio_;
    bool ownsRx_ = false;
    static constexpr size_t kBlock = 1 << 16;
    static constexpr unsigned kKernelBuffers = 8;
    // the power of two of samples that holds ~25 ms at rate, 64k to 1M
    static size_t netBlock(double rate) { size_t b = kBlock; while (b < ((size_t)1 << 20) && (double)b < rate * 0.025) b <<= 1; return b; }
    size_t block_ = kBlock;           // the samples per buffer in use
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
    bool firOn_ = false;              // the decimating FIR was switched on by this source
    std::string gainMode_;            // the gain_control_mode written
    std::string fw_;                  // the firmware version the board reports ("" when it does not say)
    bool tezuka_ = false;             // the firmware is Tezuka
    bool cs8_ = false;                // streaming 8-bit samples (Tezuka only)
};

// ---- transmit -----------------------------------------------------------------------------------------------------------------------
// The PlutoSDR's transmit path through libiio: the AD9361's TX channels (device cf-ad9361-dds-core-lpc, voltage0 / voltage1 are I and Q as
// 16-bit samples), the TX LO (altvoltage1), the TX attenuation (voltage0 output "hardwaregain", -89.75 .. 0 dB), a non-cyclic buffer that is
// pushed block by block. Regression tests use a fake libiio that records the samples and shared-clock operations.
// Level: 1.0 (full scale of the generators) is sent as 2^14, as the AD9361 examples do, leaving the converter 6 dB of headroom.
class PlutoTx : public TxSink {
public:
    explicit PlutoTx(std::string uri) : uri_(std::move(uri)) {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto it = gSerial.find(uri_);
        if (it != gSerial.end()) serial_ = it->second;
    }
    ~PlutoTx() override { close(); }

    bool open(const TxTune& t, std::string& err) override {
        std::lock_guard<std::recursive_mutex> lk(gPlutoMu);
        close();
        if (iio().v1) { err = IioApi::kV1Msg; return false; }
        if (!iio().ok) { err = kNoIioMsg; return false; }
        if (!iio().buffer_push) { err = "PlutoSDR: this libiio has no iio_buffer_push, it cannot transmit"; return false; }
        radio_ = openPluto(uri_, serial_);
        ctx_ = radio_ ? radio_->ctx : nullptr;
        if (!ctx_) {
            const int e = errno;
            err = "PlutoSDR: cannot connect (" + uri_ + ")" + (e ? ": " + iio().text(e) : std::string());
            if (kOnWindows && uri_.rfind("usb:", 0) == 0) err += std::string(". ") + windowsUsbDriverHint();
            return false;
        }
        if (radio_->tx) { err = "PlutoSDR: the transmit stream is already open"; shut(); return false; }
        radio_->tx = ownsTx_ = true;
        phy_ = radio_->phy;
        dev_ = iio().context_find_device(ctx_, "cf-ad9361-dds-core-lpc");
        if (!phy_ || !dev_) { err = "PlutoSDR: the AD9361 transmit devices were not found"; shut(); return false; }
        // voltage0 is TX1. A 2T image (some Pluto+) has voltage1 as TX2; its I/Q pair on the DDS core is voltage2 and voltage3. One LO (altvoltage1) tunes both.
        const int ch = t.channel < 0 ? 0 : t.channel;
        if (ch > 1) { err = "this Pluto has at most two transmit outputs"; shut(); return false; }
        const char* phyName = ch == 0 ? "voltage0" : "voltage1";
        const char* iName = ch == 0 ? "voltage0" : "voltage2";
        const char* qName = ch == 0 ? "voltage1" : "voltage3";
        txCh_ = iio().device_find_channel(phy_, phyName, true);
        lo_ = iio().device_find_channel(phy_, "altvoltage1", true);
        i_ = iio().device_find_channel(dev_, iName, true);
        q_ = iio().device_find_channel(dev_, qName, true);
        if (ch == 1 && (!txCh_ || !i_ || !q_)) { err = "this Pluto has one transmit output (TX1)"; shut(); return false; }
        if (!txCh_ || !lo_ || !i_ || !q_) { err = "PlutoSDR: the transmit channels were not found"; shut(); return false; }
        if (t.sampleRate < 2.083e6) { err = "PlutoSDR: the converter runs from 2.083 Msps up; " + std::to_string(t.sampleRate / 1e6) + " Msps is below that"; shut(); return false; }
        if (radio_->rx) {
            if (radio_->rxFir) {
                err = "PlutoSDR: RX is using the low-rate FIR; disable Low-rate FIR (RX only) and restart RX before starting TX";
                shut(); return false;
            }
            if (radio_->rxCs8) {
                err = "PlutoSDR: RX is using CS8; select CS16 in IQ format and restart RX before starting TX";
                shut(); return false;
            }
            if (!duplexRate(t.sampleRate, radio_->rxRate, err) || !radio_->duplex(err)) { shut(); return false; }
        } else if (const int r = iio().channel_attr_write_longlong(txCh_, "sampling_frequency", (long long)std::llround(t.sampleRate)); r != 0) {
            err = "PlutoSDR: the sample rate was refused: " + iio().text(r); shut(); return false;
        }
        rate_ = radio_->rx ? radio_->rxRate : t.sampleRate;
        {
            char v[64] = {0};
            double got = 0;
            if (iio().channel_attr_read && iio().channel_attr_read(txCh_, "sampling_frequency", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &got) == 1 && got > 0) rate_ = got;
        }
        iio().channel_attr_write_longlong(txCh_, "rf_bandwidth", (long long)std::llround(std::min(56e6, std::max(0.2e6, t.bandwidthHz > 0 ? t.bandwidthHz : rate_ * 0.95))));
        // the transmit gain is an attenuation, shown as a gain from -89.75 dB (the least) to 0 dB; the radio's own range is read when it says
        double gmin = -89.75, gmax = 0;
        {
            char v[96] = {0};
            if (iio().channel_attr_read && iio().channel_attr_read(txCh_, "hardwaregain_available", v, sizeof v - 1) > 0) rangeIn(v, gmin, gmax);
        }
        // the frequency: asked for as given; the driver decides (the PLL refuses what it cannot make)
        TuneSettings ts; ts.radio = t.radio;
        const double want = std::max(ppmCorrectedHz(txRadioHz(t), radioPpm(ts)) * (1.0 + radio_->xoPpm * 1e-6), 0.0);
        if (const int r = iio().channel_attr_write_longlong(lo_, "frequency", (long long)std::llround(want)); r != 0) {
            err = "PlutoSDR: the driver refused " + mhz(txRadioHz(t)) + " MHz as a transmit frequency (" + iio().text(r) + "): the AD9361's synthesiser tunes 70 to 6000 MHz with the AD9364 setting, 325 to 3800 MHz on the stock firmware";
            shut();
            return false;
        }
        iio().channel_attr_write_longlong(lo_, "powerdown", 0);
        const double g = std::min(gmax, std::max(gmin, t.gainDb));
        {
            char s[32];
            snprintf(s, sizeof s, "%.2f", g);
            if (iio().channel_attr_write(txCh_, "hardwaregain", s) < 0) { err = "PlutoSDR: the transmit gain " + std::string(s) + " dB was refused"; shut(); return false; }
        }
        iio().channel_enable(i_);
        iio().channel_enable(q_);
        if (iio().device_set_kernel_buffers_count) iio().device_set_kernel_buffers_count(dev_, 4);
        buf_ = iio().device_create_buffer(dev_, kBlock, false);
        if (!buf_) { err = "PlutoSDR: cannot allocate the transmit buffer"; shut(); return false; }
        fill_ = 0;
        radio_->txRate = rate_;
        return true;
    }

    bool write(const cf32* x, size_t n, std::string& err) override {
        if (!buf_) { err = "the PlutoSDR is not open for transmitting"; return false; }
        size_t i = 0;
        while (i < n) {
            int16_t* p = static_cast<int16_t*>(iio().buffer_start(buf_));
            const size_t m = std::min(n - i, kBlock - fill_);
            for (size_t k = 0; k < m; k++) { p[2 * (fill_ + k)] = q16(x[i + k].real()); p[2 * (fill_ + k) + 1] = q16(x[i + k].imag()); }
            fill_ += m; i += m;
            if (fill_ == kBlock && !push(err)) return false;
        }
        return true;
    }

    void close() override { std::lock_guard<std::recursive_mutex> lk(gPlutoMu); if (radio_ || buf_) shut(); }
    double sampleRate() const override { return rate_; }
    bool fullDuplex() const override { return true; }
    std::string name() const override { return "PlutoSDR"; }

private:
    static constexpr size_t kBlock = 1 << 15;
    static int16_t q16(float v) { const float r = std::nearbyint(v * 16384.f); return (int16_t)(r > 32767.f ? 32767 : r < -32768.f ? -32768 : r); }
    bool push(std::string& err) {
        const long long r = iio().buffer_push(buf_);
        fill_ = 0;
        if (r < 0) { err = "the PlutoSDR stopped taking samples: " + iio().text((int)-r); return false; }
        return true;
    }
    void shut() {
        if (buf_ && fill_ > 0) {   // the rest of the last block goes out as silence
            int16_t* p = static_cast<int16_t*>(iio().buffer_start(buf_));
            std::fill(p + 2 * fill_, p + 2 * kBlock, (int16_t)0);
            std::string e;
            push(e);
        }
        if (txCh_) { iio().channel_attr_write(txCh_, "hardwaregain", "-89.75"); }   // least power, and the TX LO off, before OnAir lets go of the radio
        if (lo_) iio().channel_attr_write_longlong(lo_, "powerdown", 1);
        if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
        if (i_) iio().channel_disable(i_);
        if (q_) iio().channel_disable(q_);
        if (radio_ && ownsTx_) { radio_->tx = false; radio_->txRate = 0; }
        ownsTx_ = false;
        radio_.reset();
        ctx_ = nullptr; phy_ = dev_ = nullptr; txCh_ = lo_ = i_ = q_ = nullptr; fill_ = 0;
    }
    std::shared_ptr<PlutoRadio> radio_;
    bool ownsTx_ = false;
    std::string uri_, serial_;
    IioApi::iio_context* ctx_ = nullptr;
    IioApi::iio_device* phy_ = nullptr;
    IioApi::iio_device* dev_ = nullptr;
    IioApi::iio_channel* txCh_ = nullptr;
    IioApi::iio_channel* lo_ = nullptr;
    IioApi::iio_channel* i_ = nullptr;
    IioApi::iio_channel* q_ = nullptr;
    IioApi::iio_buffer* buf_ = nullptr;
    size_t fill_ = 0;
    double rate_ = 0;
};

namespace {
// The entry of the Pluto at uri. probe: the entry comes from a scan (its name says so). userAdded: the user added this address (Add network
// radio). A board on the network is asked which firmware it runs only when plutoProbeAllowed says so; otherwise its firmware is learnt
// when it is opened (#215: a network board nobody added is not contacted during listing or scanning)
DeviceInfo plutoEntry(const std::string& uri, const std::string& desc, bool probe, bool userAdded) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "pluto";
    d.nativeArgs = uri;
    d.network = !overUsbCable(uri);
    d.name = "PlutoSDR " + (desc.empty() ? uri : "(" + uri + ")") + (probe ? " (native, experimental)" : " (network)");
    // over USB 2 a Pluto keeps up with about 4 Msps (3 over its USB-gadget network address); over a real network the converter's full rate is offered
    // a mode that needs more still gets it (linkRateFor: some lost samples beat no decode, e.g. DVB-T/T2 at 64/7 Msps)
    d.maxRateHz = 61.44e6; d.minRateHz = kFirMinRate;   // 2.048 Msps through the optional RX-only FIR, otherwise at least 2.1 Msps in hardware
    d.steadyRateHz = plutoLinkRate(uri);
    d.gainMinDb = 0; d.gainMaxDb = 73;
    d.minFreqHz = 70e6; d.maxFreqHz = 6e9;
    d.canTransmit = true;   // PlutoTx (the AD9361 transmits; not tested on a radio)
    d.txFullDuplex = true;  // native RX/TX share their context and clock; incompatible configurations are refused at open
    d.txChannels = 1;       // the firmware OnAir opens exposes one RF output; a second (voltage1) is used only when that image has it, and the open says so
    d.txMinRateHz = 2.1e6; d.txMaxRateHz = 61.44e6;
    d.txGainMinDb = -89.75; d.txGainMaxDb = 0;
    d.txGainNote = "the AD9361's TX attenuation shown as a gain: -89.75 dB (least power) to 0 dB (most)";
    d.settings = {ppmSetting(0.01),
                  boolSetting("lowratefir", "Low-rate FIR (RX only)",
                              "Receive 2.048 Msps directly through the hardware FIR. RX only: disable this to share the radio with TX.\n"
                              "Off by default: use at least 2.1 Msps (or the running TX rate), with software resampling in the receiver.", false, true),
                  choiceSetting("gainmode", "Gain mode",
                                "Manual: the gain slider (and OnAir's AGC) set the AD9361's gain (the default).\n"
                                "Slow / fast attack: the AD9361's own AGC sets it (fast attack for bursts); the gain slider then has no effect.",
                                {"manual", "slow_attack", "fast_attack"}, {"Manual", "Radio AGC, slow attack", "Radio AGC, fast attack"}, "manual")};
    bool tez = false, known = false;
    {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto t = gTezuka.find(uri);
        if (t != gTezuka.end()) { tez = t->second; known = true; }
    }
    // a board on the network the user added can be asked which firmware it runs without taking anything from a receiver (a USB one is learnt when opened)
    if (plutoProbeAllowed(uri, userAdded) && !known && iio().context_get_attr_value && !iio().v1) {
        if (IioApi::iio_context* c = iio().create_context_from_uri(uri.c_str())) {
            tez = isTezukaFw(iio().context_get_attr_value(c, "fw_version"));
            iio().context_destroy(c);
            std::lock_guard<std::mutex> lk(gRangeMu);
            gTezuka[uri] = tez;
        }
    }
    if (defaultAddress(uri))
        d.settings.push_back(choiceSetting("link", "Link",
                                           "How this Pluto is connected at its default address (192.168.2.1 / pluto.local). Auto: taken as the USB cable's\n"
                                           "gadget network, which carries about 3 Msps without loss. Ethernet: a Pluto+ (or a Pluto on a USB Ethernet\n"
                                           "adapter) with a real network link: no limit but the converter's. USB cable: held to the USB rate.",
                                           {"auto", "usb", "ethernet"}, {"Auto", "USB cable", "Ethernet"}, "auto"));
    if (tez)
        d.settings.push_back(choiceSetting("iqformat", "IQ format",
                                           "Tezuka firmware only. CS16: 16-bit samples (the 12-bit converter, the default).\n"
                                           "CS8: 8-bit samples, half the data on the cable, so a higher sample rate over USB or a slow network; less dynamic range.",
                                           {"cs16", "cs8"}, {"CS16", "CS8"}, "cs16", true));
    {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto it = gRange.find(uri);
        if (it != gRange.end()) { d.minFreqHz = it->second.first; d.maxFreqHz = it->second.second; }
        const std::string sn = serialIn(desc);
        if (!sn.empty()) gSerial[uri] = sn;
    }
    return d;
}
} // namespace

// a Pluto the user added by address (Add network radio): over a real network, also at 192.168.2.1 (a Pluto+ on Ethernet), so no USB 2 limit
double plutoLinkRate(const std::string& uri) { return usbLinkRate(uri); }
// The entry of a Pluto the user added by address: not asked for its firmware while listing (it is learnt when opened)
DeviceInfo plutoDevice(const std::string& uri) { DeviceInfo d = plutoEntry(uri, "", false, false); d.steadyRateHz = 0; return d; }

// Only a Pluto the user added by address may be asked which firmware it runs, and only an "ip:" one. A scan never asks a network board (#215)
bool plutoProbeAllowed(const std::string& uri, bool userAdded) { return userAdded && uri.rfind("ip:", 0) == 0; }

void listPluto(std::vector<DeviceInfo>& out) {
    if (!iio().ok && !iio().v1) return;
    auto add = [&](const std::string& uri, const std::string& desc) {
        for (const auto& d : out) if (d.board == "pluto" && d.nativeArgs == uri) return;
        // a scan result is never a network probe, unless the user turned on "Search the network for radios": the network boards found then
        // are asked which firmware they run, as the ones added by address are
        out.push_back(plutoEntry(uri, desc, true, networkDiscovery()));
    };
    // USB always. The network boards (libiio's "ip" backend: mDNS) only when "Search the network for radios" is on; otherwise a network Pluto is
    // listed when the user adds its address (Add network radio), so no network device is contacted just by listing (#215)
    for (const char* backends : {"usb", "ip"}) {
        if (backends[0] == 'i' && !networkDiscovery()) continue;
        IioApi::iio_scan_context* sc = iio().v1 ? nullptr : iio().create_scan_context(backends, 0);
        if (!sc) continue;
        IioApi::iio_context_info** info = nullptr;
        const auto n = iio().scan_context_get_info_list(sc, &info);
        if (n <= 0) { if (info) iio().context_info_list_free(info); iio().scan_context_destroy(sc); continue; }
        for (long i = 0; i < (long)n && i < 32; i++) {
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

// A PlutoSDR that SoapyPlutoSDR found on the network (soapyArgs "driver=plutosdr, uri=ip:..."): the same board as the native driver's entry for that address.
// The Soapy module asks the kernel for big buffers, which the Pluto's small CMA area refuses at 10 Msps and more (issue #316: only the scanned entry failed,
// the same radio added by hand worked), so it is shown as the native entry. false: not a network Pluto, or no libiio for the native driver.
bool plutoFromSoapy(const std::string& soapyArgs, const std::string& name, DeviceInfo& out) {
    if (!iio().ok) return false;
    size_t p = soapyArgs.find("uri=");
    if (p == std::string::npos) return false;
    p += 4;
    std::string uri = soapyArgs.substr(p, soapyArgs.find(',', p) == std::string::npos ? std::string::npos : soapyArgs.find(',', p) - p);
    while (!uri.empty() && uri.back() == ' ') uri.pop_back();
    if (uri.rfind("ip:", 0) != 0) return false;
    out = plutoEntry(uri, "", false, false);
    if (!defaultAddress(uri)) out.steadyRateHz = 0;   // a Pluto on a real network: the converter's limit only
    if (!name.empty()) out.name = name;
    out.network = true;
    return true;
}

std::unique_ptr<IqSource> makePluto(const DeviceInfo& d) {
    return std::make_unique<PlutoSource>(d.nativeArgs);
}

std::unique_ptr<TxSink> makePlutoTx(const DeviceInfo& d) { return std::make_unique<PlutoTx>(d.nativeArgs); }

} // namespace native
} // namespace dect2
