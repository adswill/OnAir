// Native driver for the BladeRF: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#if !defined(_WIN32) && !defined(__APPLE__)
#include <unistd.h>
#endif

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
    static int channelRx(int n) { return n << 1; }   // BLADERF_CHANNEL_RX(n): RX1 = 0, RX2 = 2
    static constexpr int kRxX1 = 0;                // BLADERF_RX_X1
    static constexpr int kFormatSc16Q11 = 0;       // BLADERF_FORMAT_SC16_Q11
    // bladerf_gain_mode: DEFAULT (the board's AGC: the bladeRF 1's FPGA AGC, the 2.0's slow attack), MGC (manual), FASTATTACK_AGC,
    // SLOWATTACK_AGC, HYBRID_AGC
    static constexpr int kGainDefault = 0, kGainMgc = 1, kGainFast = 2, kGainSlow = 3, kGainHybrid = 4;
    static constexpr int kXb200 = 2;               // bladerf_xb: NONE, XB_100, XB_200, XB_300
    static constexpr int kFpga40 = 40, kFpga115 = 115;   // bladerf_fpga_size of the bladeRF 1 (x40, x115)
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
    // optional: error text, the bladeRF 1's FPGA, the tuning range
    const char* (DECT2_CALL* strerror_)(int) = nullptr;
    int (DECT2_CALL* is_fpga_configured)(bladerf*) = nullptr;
    int (DECT2_CALL* get_fpga_size)(bladerf*, int*) = nullptr;
    int (DECT2_CALL* load_fpga)(bladerf*, const char*) = nullptr;
    const char* (DECT2_CALL* get_board_name)(bladerf*) = nullptr;
    int (DECT2_CALL* get_frequency_range)(bladerf*, int, const range_t**) = nullptr;
    int (DECT2_CALL* set_bias_tee)(bladerf*, int, bool) = nullptr;   // bladeRF 2.0 only (libbladeRF 2.4 and later)
    int (DECT2_CALL* get_sample_rate_range)(bladerf*, int, const range_t**) = nullptr;
    int (DECT2_CALL* expansion_attach)(bladerf*, int) = nullptr;   // the bladeRF 1's XB-200 transverter (libbladeRF does its mixer and filters itself)
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
        if (ok) {
            lib.opt(strerror_, "bladerf_strerror");
            lib.opt(is_fpga_configured, "bladerf_is_fpga_configured");
            lib.opt(get_fpga_size, "bladerf_get_fpga_size");
            lib.opt(load_fpga, "bladerf_load_fpga");
            lib.opt(get_board_name, "bladerf_get_board_name");
            lib.opt(get_frequency_range, "bladerf_get_frequency_range");
            lib.opt(set_bias_tee, "bladerf_set_bias_tee");
            lib.opt(get_sample_rate_range, "bladerf_get_sample_rate_range");
            lib.opt(expansion_attach, "bladerf_expansion_attach");
        }
    }
    std::string text(int code) {
        const char* t = strerror_ ? strerror_(code) : nullptr;
        return (t && *t ? std::string(t) : std::string("error")) + " (" + std::to_string(code) + ")";
    }
};
BladeApi& blade() { static BladeApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

std::string programDir() {
    char p[4096] = {0};
#if defined(_WIN32)
    if (!GetModuleFileNameA(nullptr, p, (DWORD)sizeof p - 1)) return {};
#elif defined(__APPLE__)
    uint32_t size = sizeof p;
    if (_NSGetExecutablePath(p, &size) != 0) return {};
#else
    if (readlink("/proc/self/exe", p, sizeof p - 1) <= 0) return {};
#endif
    std::string d = p;
    const size_t cut = d.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : d.substr(0, cut + 1);
}

// The folders libbladeRF itself looks in for FPGA images, plus the program's own folder
std::vector<std::string> fpgaDirs() {
    std::vector<std::string> v;
    auto add = [&](const std::string& d) { if (!d.empty()) v.push_back(d.back() == '/' || d.back() == '\\' ? d : d + "/"); };
    add(programDir());
    if (const char* e = getenv("BLADERF_SEARCH_DIR")) add(e);
#ifdef _WIN32
    if (const char* e = getenv("APPDATA")) add(std::string(e) + "\\Nuand\\bladeRF");
    if (const char* e = getenv("ProgramFiles")) add(std::string(e) + "\\bladeRF");
#else
    if (const char* h = getenv("HOME")) { add(std::string(h) + "/.config/Nuand/bladeRF"); add(std::string(h) + "/.Nuand/bladeRF"); }
    for (const char* d : {"/etc/Nuand/bladeRF", "/usr/share/Nuand/bladeRF", "/usr/local/share/Nuand/bladeRF", "/opt/homebrew/share/Nuand/bladeRF"}) add(d);
#endif
    return v;
}

bool fileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

} // namespace

class BladeSource : public NativeSource {
public:
    // args: the serial, then "#" and the input of the bladeRF 2.0 (0 = RX1, 1 = RX2; listBlade)
    explicit BladeSource(const std::string& args) {
        const size_t cut = args.find('#');
        serial_ = args.substr(0, cut);
        if (cut != std::string::npos) ch_ = BladeApi::channelRx(std::max(0, std::min(1, atoi(args.c_str() + cut + 1))));
    }
    ~BladeSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        const std::string id = serial_.empty() ? std::string() : "*:serial=" + serial_;
        const int r = blade().open(&dev_, id.empty() ? nullptr : id.c_str());
        if (r != 0 || !dev_) {
            dev_ = nullptr;
            err = "bladeRF: cannot open the radio: " + blade().text(r);
            if (kOnWindows && (r == -5 || r == -7 || r == -8 || r == -17)) err += std::string(". ") + windowsUsbDriverHint();   // IO, NODEV, UNSUPPORTED, PERMISSION
            return false;
        }
        if (!loadFpga(err)) { openFailure_ = OpenFailure::Final; return false; }
        lo_ = hi_ = 0;
        bias_ = false;
        is20_ = is1_ = false;
        if (blade().get_board_name) {   // the bladeRF 1 has no bias-tee
            const char* bn = blade().get_board_name(dev_);
            is20_ = bn && strstr(bn, "bladerf2");
            is1_ = bn && strstr(bn, "bladerf1");
        }
        // a bladeRF 1 with the XB-200 on it (TuneSettings "xb200"): attached, libbladeRF tunes below 300 MHz through its mixer and picks its
        // filters by frequency (xb200_init: BLADERF_XB200_AUTO_1DB); its tuning range then starts at 0 (BLADERF_FREQUENCY_MIN_XB200)
        xb200_ = false;
        if (radioFlag(s, "xb200") && is1_ && blade().expansion_attach) {
            if (const int r = blade().expansion_attach(dev_, BladeApi::kXb200); r == 0) xb200_ = true;
            else { err = "bladeRF: the XB-200 transverter did not attach: " + blade().text(r); openFailure_ = OpenFailure::Final; return false; }
        }
        const BladeApi::range_t* fr = nullptr;
        if (blade().get_frequency_range && blade().get_frequency_range(dev_, ch_, &fr) == 0 && fr) {
            const double sc = fr->scale > 0 ? fr->scale : 1.0;
            lo_ = fr->min * sc; hi_ = fr->max * sc;
        }
        if (xb200_) lo_ = std::min(lo_, 60e3);   // the XB-200 covers 60 kHz - 300 MHz (an older library may report the bare board's range)
        gainMode_ = BladeApi::kGainMgc;
        blade().set_gain_mode(dev_, ch_, gainMode_);   // configure() switches to the board's AGC when the settings ask for it
        return configure(s, err, false);
    }
    // manual (the default: OnAir's gain control) or the board's own AGC (TuneSettings "gainmode")
    static int gainModeOf(const TuneSettings& s) {
        const std::string m = radioOption(s, "gainmode", "manual");
        return m == "agc" ? BladeApi::kGainDefault : m == "fast" ? BladeApi::kGainFast : m == "slow" ? BladeApi::kGainSlow : m == "hybrid" ? BladeApi::kGainHybrid : BladeApi::kGainMgc;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        const int ch = ch_;   // a single-channel stream (BLADERF_RX_X1) carries the channel that is enabled, RX1 or RX2 (as in bladeRF-cli)
        if (!live) {
            if (enabled_) { blade().enable_module(dev_, ch, false); enabled_ = false; }
            unsigned actual = 0;
            // a rate outside the board's range is refused (BLADERF_ERR_RANGE): bladeRF 2.0 520834 Hz - 61.44 MHz, bladeRF 1 80 kHz - 40 MHz
            double rmin = 520834, rmax = is1_ ? 40e6 : 61.44e6;
            const BladeApi::range_t* rr = nullptr;
            if (blade().get_sample_rate_range && blade().get_sample_rate_range(dev_, ch, &rr) == 0 && rr && rr->max > rr->min) {
                const double sc = rr->scale > 0 ? rr->scale : 1.0;
                rmin = std::ceil(rr->min * sc); rmax = std::floor(rr->max * sc);
            }
            const unsigned want = (unsigned)std::llround(std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, rmin), rmax));
            if (const int r = blade().set_sample_rate(dev_, ch, want, &actual); r != 0) { err = "bladeRF: the sample rate was refused: " + blade().text(r); return false; }
            rate_ = actual ? actual : want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            unsigned bwActual = 0;
            blade().set_bandwidth(dev_, ch, (unsigned)std::llround(std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95)), &bwActual);
            // 16 buffers of 16384 samples, 8 USB transfers in flight
            if (const int r = blade().sync_config(dev_, BladeApi::kRxX1, BladeApi::kFormatSc16Q11, 16, 16384, 8, 3500); r != 0) { err = "bladeRF: cannot start the sample stream: " + blade().text(r); return false; }
            if (const int r = blade().enable_module(dev_, ch, true); r != 0) { err = "bladeRF: cannot enable the receiver: " + blade().text(r); return false; }
            enabled_ = true;
        }
        const double f = std::max(s.centerHz, 0.0);
        const std::string range = hi_ > 0 ? " (range " + mhz(lo_) + "-" + mhz(hi_) + " MHz)" : std::string();
        if (hi_ > 0 && (f < lo_ || f > hi_)) { err = "bladeRF could not tune to " + mhz(f) + " MHz" + range; openFailure_ = OpenFailure::Final; return false; }
        // libbladeRF has no frequency correction in Hz (only the VCTCXO trim DAC): a correction the user set is taken out of the frequency
        const double tuneHz = std::max(ppmCorrectedHz(f, radioPpm(s)), 0.0);
        if (const int r = blade().set_frequency(dev_, ch, (uint64_t)std::llround(tuneHz)); r != 0) { err = "bladeRF could not tune to " + mhz(f) + " MHz" + range + ": " + blade().text(r); openFailure_ = OpenFailure::Final; return false; }
        if (const int gm = gainModeOf(s); gm != gainMode_ && blade().set_gain_mode(dev_, ch, gm) == 0) gainMode_ = gm;
        double gmin = -15, gmax = 60;
        const BladeApi::range_t* r = nullptr;
        if (blade().get_gain_range(dev_, ch, &r) == 0 && r) { gmin = r->min * r->scale; gmax = r->max * r->scale; }
        if (gainMode_ == BladeApi::kGainMgc) blade().set_gain(dev_, ch, (int)std::lround(std::min(std::max(s.gainDb, gmin), gmax)));   // the board's AGC sets it otherwise
        // the bladeRF 2.0 has one RX antenna-power switch whatever the channel (libbladeRF: RFFE_CONTROL_RX_BIAS_EN): it powers RX1 and RX2
        if (blade().set_bias_tee && is20_ && s.biasTee != bias_) {
            if (blade().set_bias_tee(dev_, ch, s.biasTee) == 0) bias_ = s.biasTee;
            else if (s.biasTee) { fprintf(stderr, "bladeRF: the radio refused the bias-tee\n"); fflush(stderr); }
        }
        return true;
    }
    void closeDevice() override {
        if (!dev_) return;
        if (bias_ && blade().set_bias_tee) blade().set_bias_tee(dev_, ch_, false);   // antenna power never stays on
        bias_ = false;
        if (enabled_) blade().enable_module(dev_, ch_, false);
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
            } else if (++failures > 20) { fprintf(stderr, "bladeRF: stream error %s, stopping the source\n", blade().text(r).c_str()); break; }
        }
    }

private:
    // A bladeRF 1 loses its FPGA at power-off and libbladeRF loads it at open only when it finds the image in its own folders:
    // look next to OnAir too, and say plainly what is missing.
    bool loadFpga(std::string& err) {
        if (!blade().is_fpga_configured) return true;
        if (blade().is_fpga_configured(dev_) != 0) return true;   // 1 = configured; an error is left to the calls that follow
        int size = 0;
        if (blade().get_fpga_size) blade().get_fpga_size(dev_, &size);
        std::vector<std::string> names;
        if (size == BladeApi::kFpga40) names = {"hostedx40.rbf"};
        else if (size == BladeApi::kFpga115) names = {"hostedx115.rbf"};
        else names = {"hostedx40.rbf", "hostedx115.rbf"};
        if (blade().load_fpga) {
            for (const auto& dir : fpgaDirs()) {
                for (const auto& n : names) {
                    const std::string p = dir + n;
                    if (!fileExists(p)) continue;
                    fprintf(stderr, "bladeRF 1: loading the FPGA image %s\n", p.c_str());
                    fflush(stderr);
                    if (const int r = blade().load_fpga(dev_, p.c_str()); r != 0) { err = "bladeRF 1: loading the FPGA image " + p + " failed: " + blade().text(r); return false; }
                    return true;
                }
            }
        }
        err = "bladeRF 1 needs its FPGA image (hostedx40.rbf or hostedx115.rbf): download it from nuand.com and put it next to OnAir";
        return false;
    }
    std::string serial_;
    int ch_ = BladeApi::channelRx(0);   // the receive input the user picked
    BladeApi::bladerf* dev_ = nullptr;
    bool enabled_ = false, bias_ = false, is20_ = false, is1_ = false, xb200_ = false;
    int gainMode_ = BladeApi::kGainMgc;
    double lo_ = 0, hi_ = 0;
};

void listBlade(std::vector<DeviceInfo>& out) {
    if (!blade().ok) return;
    BladeApi::devinfo_t* list = nullptr;
    const int n = blade().get_device_list(&list);
    if (n <= 0 || !list) return;
    for (int i = 0; i < n && i < 16; i++) {
        const std::string serial = trimmed(list[i].serial, sizeof list[i].serial);
        const std::string prod = trimmed(list[i].product, sizeof list[i].product);
        // one entry per receive input on the bladeRF 2.0 (RX1, RX2), so the user picks the one the antenna is on (the first is what OnAir
        // used before); the bladeRF 1 has one
        const bool two = prod.find("2.0") != std::string::npos;
        for (int port = 0; port < (two ? 2 : 1); port++) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "bladerf";
        d.serial = serial;
        d.nativeArgs = two ? serial + "#" + std::to_string(port) : serial;
        d.name = (prod.empty() ? "bladeRF" : prod) + (d.serial.size() > 8 ? " " + d.serial.substr(d.serial.size() - 8) : "") + (two ? (port ? " RX2" : " RX1") : "") + " (native, experimental)";
        d.maxRateHz = 61.44e6; d.minRateHz = 0.52e6;
        d.gainMinDb = -15; d.gainMaxDb = 60;
        // the product string tells the two generations apart: "bladeRF 2.0" (AD9361) and "bladeRF" (LMS6002D). The ranges are libbladeRF's:
        // bladeRF 2.0 RX 70 MHz - 6 GHz (bladerf2_rx_frequency_range), bladeRF 1 at most 40 Msps (BLADERF_SAMPLERATE_REC_MAX)
        if (prod.find("2.0") != std::string::npos) { d.minFreqHz = 70e6; d.maxFreqHz = 6e9; d.hasBiasTee = blade().set_bias_tee != nullptr; }
        else if (!prod.empty()) { d.minFreqHz = 237.5e6; d.maxFreqHz = 3.8e9; d.maxRateHz = 40e6; }   // BLADERF_FREQUENCY_MIN (bladeRF1.h)
        d.settings = {ppmSetting(0.01)};
        if (two)
            d.settings.push_back(choiceSetting("gainmode", "Gain mode",
                                               "Manual: the gain slider (and OnAir's AGC) set the gain (the default). Otherwise the AD9361's own AGC does (fast attack\nfor bursts, hybrid as the AD9361 defines it); the gain slider then has no effect.",
                                               {"manual", "slow", "fast", "hybrid"}, {"Manual", "Radio AGC, slow attack", "Radio AGC, fast attack", "Radio AGC, hybrid"}, "manual"));
        else {
            d.settings.push_back(choiceSetting("gainmode", "Gain mode", "Manual: the gain slider (and OnAir's AGC) set the gain (the default). Radio AGC: the FPGA's AGC (FPGA v0.7 and later); the slider then\nhas no effect.",
                                               {"manual", "agc"}, {"Manual", "Radio AGC"}, "manual"));
            if (blade().expansion_attach)
                d.settings.push_back(boolSetting("xb200", "XB-200 transverter", "The XB-200 expansion board is fitted: the bladeRF then receives 60 kHz - 300 MHz through it (libbladeRF switches its\nmixer and filters by frequency). Takes effect when the radio is started.", false, true));
        }
        out.push_back(d);
        }
    }
    blade().free_device_list(list);
}

std::unique_ptr<IqSource> makeBlade(const DeviceInfo& d) {
    return std::make_unique<BladeSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
