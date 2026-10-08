// Native driver for the USRP: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include <map>

namespace dect2 {
namespace native {


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
    // optional: the library's own error text, the tuning range, the antenna inputs
    err_t (DECT2_CALL* get_last_error)(char*, size_t) = nullptr;
    err_t (DECT2_CALL* usrp_last_error)(usrp_t*, char*, size_t) = nullptr;
    err_t (DECT2_CALL* get_rx_freq_range)(usrp_t*, size_t, range_t_*) = nullptr;
    err_t (DECT2_CALL* get_rx_num_channels)(usrp_t*, size_t*) = nullptr;
    err_t (DECT2_CALL* get_rx_antennas)(usrp_t*, size_t, strvec_t**) = nullptr;
    err_t (DECT2_CALL* set_rx_antenna)(usrp_t*, const char*, size_t) = nullptr;
    err_t (DECT2_CALL* set_clock_source)(usrp_t*, const char*, size_t) = nullptr;   // optional: the reference clock of a motherboard
    bool ok = false;
    DynLib lib;
    UhdApi() {
        if (nativeDisabled() || !lib.open(libNames("uhd", {".4.8.0", ".4.7.0", ".4.6.0", ".4.5.0", ".4.4.0", ".4.3.0", ".4.2.0", ".4.1.0", ".4.0.0", ".3.15.0", ""}, {"libuhd.dll", "uhd.dll"}))) return;
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
        if (ok) {
            lib.opt(get_last_error, "uhd_get_last_error"); lib.opt(usrp_last_error, "uhd_usrp_last_error"); lib.opt(get_rx_freq_range, "uhd_usrp_get_rx_freq_range");
            lib.opt(get_rx_num_channels, "uhd_usrp_get_rx_num_channels"); lib.opt(get_rx_antennas, "uhd_usrp_get_rx_antennas"); lib.opt(set_rx_antenna, "uhd_usrp_set_rx_antenna");
            lib.opt(set_clock_source, "uhd_usrp_set_clock_source");
        }
    }
    // UHD's text for the last failure, on one line (its messages can span several)
    std::string text(usrp_t* dev) {
        char b[1024] = {0};
        if (dev && usrp_last_error) usrp_last_error(dev, b, sizeof b);
        if (!b[0] && get_last_error) get_last_error(b, sizeof b);
        std::string t = trimmed(b, sizeof b);
        for (char& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (t.size() > 400) t = t.substr(0, 400) + "...";
        return t;
    }
};
UhdApi& uhd() { static UhdApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

bool contains(const std::string& s, const char* what) {
    std::string a = s, b = what;
    for (char& c : a) c = (char)tolower((unsigned char)c);
    for (char& c : b) c = (char)tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

// Opening a USRP loads its FPGA (seconds): the list shows the range the radio reported when it was last opened, else the B2xx range
std::mutex gRangeMu;
std::map<std::string, std::pair<double, double>> gRange;
// ... and the receive inputs (channel, antenna) it reported then: the radios other than the B2xx, whose inputs are not known before
std::map<std::string, std::vector<std::pair<size_t, std::string>>> gInputs;

} // namespace

class UsrpSource : public NativeSource {
public:
    // args: UHD's device arguments, then "#<channel>#<antenna>" for an input other than the default one (listUsrp)
    explicit UsrpSource(const std::string& args) {
        const size_t cut = args.find('#');
        args_ = args.substr(0, cut);
        if (cut == std::string::npos) return;
        const size_t cut2 = args.find('#', cut + 1);
        ch_ = (size_t)std::max(0, atoi(args.c_str() + cut + 1));
        if (cut2 != std::string::npos) antenna_ = args.substr(cut2 + 1);
    }
    ~UsrpSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (uhd().make(&dev_, args_.c_str()) != 0 || !dev_) {
            if (dev_) uhd().free_(&dev_);
            dev_ = nullptr;
            const std::string t = uhd().text(nullptr);
            if (contains(t, "images_downloader") || contains(t, "could not find path for image") || contains(t, "UHD_IMAGES_DIR") || contains(t, "image not found")) {
                err = "USRP firmware/FPGA images not found: run uhd_images_downloader once (or set UHD_IMAGES_DIR)";
                openFailure_ = OpenFailure::Final;
            } else {
                err = "USRP: cannot open the radio" + (t.empty() ? std::string(" (in use?)") : ": " + t);
                if (kOnWindows && (contains(t, "libusb") || contains(t, "no devices found"))) err += std::string(". ") + windowsUsbDriverHint();
            }
            return false;
        }
        lo_ = hi_ = 0;
        // the reference clock (TuneSettings "clock"): UHD's own choice unless the user picked one (an external 10 MHz, a GPSDO). Set first:
        // everything that follows is tuned against it
        if (const std::string clk = radioOption(s, "clock", "default"); clk != "default") {
            if (!uhd().set_clock_source || uhd().set_clock_source(dev_, clk.c_str(), 0) != 0) {
                err = "USRP: the reference clock \"" + clk + "\" was refused" + (uhd().set_clock_source ? ": " + uhd().text(dev_) : std::string(" (this UHD cannot set it)"));
                openFailure_ = OpenFailure::Final;
                return false;
            }
        }
        rememberInputs();
        if (size_t n = 0; ch_ > 0 && uhd().get_rx_num_channels && uhd().get_rx_num_channels(dev_, &n) == 0 && ch_ >= n) {
            err = "USRP: this radio has " + std::to_string(n) + " receive channel(s), channel " + std::to_string(ch_ + 1) + " was picked";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (!antenna_.empty()) {   // the default entry leaves UHD's choice alone (RX2 on a B2xx)
            if (!uhd().set_rx_antenna) { err = "USRP: this UHD cannot pick the antenna input (no uhd_usrp_set_rx_antenna)"; openFailure_ = OpenFailure::Final; return false; }
            if (uhd().set_rx_antenna(dev_, antenna_.c_str(), ch_) != 0) { err = "USRP: the antenna input " + antenna_ + " was refused: " + uhd().text(dev_); openFailure_ = OpenFailure::Final; return false; }
        }
        if (UhdApi::range_t_* rg = nullptr; uhd().get_rx_freq_range && uhd().meta_range_make(&rg) == 0 && rg) {
            double a = 0, b = 0;
            if (uhd().get_rx_freq_range(dev_, ch_, rg) == 0 && uhd().meta_range_start(rg, &a) == 0 && uhd().meta_range_stop(rg, &b) == 0 && b > a) {
                lo_ = a; hi_ = b;
                std::lock_guard<std::mutex> lk(gRangeMu);
                gRange[args_] = {a, b};
            }
            uhd().meta_range_free(&rg);
        }
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        const size_t ch = ch_;
        if (!live) {
            if (streamer_) { stopReceiving(); }
            double lo = 0.1e6, hi = 200e6;
            UhdApi::range_t_* rg = nullptr;
            if (uhd().meta_range_make(&rg) == 0 && rg) {
                if (uhd().get_rx_rates(dev_, ch, rg) == 0) { uhd().meta_range_start(rg, &lo); uhd().meta_range_stop(rg, &hi); }
                uhd().meta_range_free(&rg);
            }
            // The range is the one of the master clock in use: a B2xx starts at 16 MHz (ad936x_manager::DEFAULT_TICK_RATE) and raises it
            // itself for a faster rate (b200_impl::coerce_rx_samp_rate), so a rate above the range is asked for first; the range's top
            // only when the radio refuses it
            const double want = std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, lo);
            if (uhd().set_rx_rate(dev_, want, ch) != 0 && (want <= hi || uhd().set_rx_rate(dev_, hi, ch) != 0)) { err = "USRP: the sample rate was refused: " + uhd().text(dev_); return false; }
            double actual = std::min(want, hi);
            uhd().get_rx_rate(dev_, ch, &actual);
            rate_ = actual;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            uhd().set_rx_bandwidth(dev_, std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95), ch);
        }
        // UHD clips a frequency outside the range without failing: check first
        const double f = hi_ > 0 ? s.centerHz : std::max(s.centerHz, 1e6);
        const std::string range = hi_ > 0 ? " (range " + mhz(lo_) + "-" + mhz(hi_) + " MHz)" : std::string();
        if (hi_ > 0 && (f < lo_ || f > hi_)) { err = "USRP could not tune to " + mhz(f) + " MHz" + range; openFailure_ = OpenFailure::Final; return false; }
        char noArgs[1] = {0};
        // UHD has no frequency correction (a USRP takes an external reference or a GPSDO for that): a correction the user set is taken out of the frequency
        UhdApi::tune_request_t req{ppmCorrectedHz(f, radioPpm(s)), UhdApi::kPolicyAuto, 0, UhdApi::kPolicyAuto, 0, noArgs};
        UhdApi::tune_result_t res{};
        if (uhd().set_rx_freq(dev_, &req, ch, &res) != 0) { err = "USRP could not tune to " + mhz(f) + " MHz" + range + ": " + uhd().text(dev_); return false; }
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
        int failures = 0, timeouts = 0;
        while (run_) {
            void* bufs[1] = {conv_.data()};
            size_t got = 0;
            const int e = uhd().rx_streamer_recv(streamer_, bufs, N, &md_, 1.0, false, &got);
            int code = 0;
            if (e == 0) uhd().rx_metadata_error_code(md_, &code);
            if (e == 0 && (code == UhdApi::kErrNone || code == UhdApi::kRxOverflow) && got > 0) { push(conv_.data(), got); failures = timeouts = 0; }
            // an overflow means samples were lost, not that the stream ended: continuous streaming goes on (rx_metadata_t::ERROR_CODE_OVERFLOW)
            else if (e == 0 && code == UhdApi::kRxOverflow) continue;
            // nothing for kStallS seconds (a network USRP whose cable was pulled only times out): treated as lost, so that it is opened again
            else if (e == 0 && code == UhdApi::kRxTimeout) { if (run_ && ++timeouts >= kStallS) { fprintf(stderr, "USRP: no samples for %d s, stopping the source\n", kStallS); break; } }
            else if (++failures > 20) { fprintf(stderr, "USRP: stream error %d / %d, stopping the source\n", e, code); break; }
        }
    }

private:
    static constexpr int kStallS = 5;
    bool startReceiving(std::string& err) {
        size_t chan = ch_;
        char cpu[] = "fc32", otw[] = "sc16", none[] = "";
        UhdApi::stream_args_t sa{cpu, otw, none, &chan, 1};
        if (uhd().rx_streamer_make(&streamer_) != 0 || uhd().rx_metadata_make(&md_) != 0 || uhd().get_rx_stream(dev_, &sa, streamer_) != 0) { err = "USRP: cannot set up the sample stream: " + uhd().text(dev_); stopReceiving(); return false; }
        UhdApi::stream_cmd_t cmd{UhdApi::kStartContinuous, 0, true, 0, 0.0};
        if (uhd().rx_streamer_issue_stream_cmd(streamer_, &cmd) != 0) { err = "USRP: cannot start the sample stream: " + uhd().text(dev_); stopReceiving(); return false; }
        return true;
    }
    void stopReceiving() {
        if (streamer_) { UhdApi::stream_cmd_t cmd{UhdApi::kStopContinuous, 0, true, 0, 0.0}; uhd().rx_streamer_issue_stream_cmd(streamer_, &cmd); uhd().rx_streamer_free(&streamer_); }
        if (md_) uhd().rx_metadata_free(&md_);
        streamer_ = nullptr; md_ = nullptr;
    }
    // the inputs of each channel, for the next listing (a radio is only opened when it is started)
    void rememberInputs() {
        if (!uhd().get_rx_antennas) return;
        size_t n = 1;
        if (uhd().get_rx_num_channels && (uhd().get_rx_num_channels(dev_, &n) != 0 || n == 0)) return;
        std::vector<std::pair<size_t, std::string>> in;
        for (size_t c = 0; c < n && c < 8; c++) {
            UhdApi::strvec_t* v = nullptr;
            if (uhd().strvec_make(&v) != 0 || !v) return;
            size_t k = 0;
            if (uhd().get_rx_antennas(dev_, c, &v) == 0 && uhd().strvec_size(v, &k) == 0)
                for (size_t i = 0; i < k && i < 8; i++) { char b[128] = {0}; if (uhd().strvec_at(v, i, b, sizeof b) == 0 && b[0]) in.push_back({c, trimmed(b, sizeof b)}); }
            uhd().strvec_free(&v);
        }
        std::lock_guard<std::mutex> lk(gRangeMu);
        gInputs[args_] = in;
    }
    std::string args_, antenna_;   // antenna_: the input the user picked ("" = UHD's default)
    size_t ch_ = 0;                // the receive channel (the B210's RF A = 0, RF B = 1)
    UhdApi::usrp_t* dev_ = nullptr;
    UhdApi::streamer_t* streamer_ = nullptr;
    UhdApi::metadata_t* md_ = nullptr;
    double lo_ = 0, hi_ = 0;   // RX tuning range the library reports (0 = not known)
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
            std::string prod = field("product");
            if (prod.empty()) prod = field("type");
            // One entry per receive input, so the user picks the one the antenna is on. The first is what OnAir used before: channel 0 with
            // UHD's default antenna. A B2xx has RX2 (UHD's default) and TX/RX on each channel (b200_impl), the B210 two channels (RF A, RF B).
            // Other models: their inputs are known once the radio has been started (opening a USRP loads its FPGA, too slow for a listing)
            struct Input { size_t ch; std::string antenna, label; };
            std::vector<Input> inputs;
            if (field("type") == "b200") {
                const bool b210 = prod == "B210";
                for (size_t c = 0; c < (b210 ? 2u : 1u); c++)
                    for (const char* a : {"RX2", "TX/RX"}) inputs.push_back({c, c == 0 && inputs.empty() ? std::string() : a, (b210 ? std::string(c ? "RF B " : "RF A ") : std::string()) + a});
            } else {
                inputs.push_back({0, "", ""});
                std::lock_guard<std::mutex> lk(gRangeMu);
                const auto it = gInputs.find(args);
                if (it != gInputs.end() && it->second.size() > 1) {
                    size_t chans = 0;
                    for (const auto& x : it->second) chans = std::max(chans, x.first + 1);
                    for (const auto& x : it->second) inputs.push_back({x.first, x.second, (chans > 1 ? "channel " + std::to_string(x.first + 1) + " " : std::string()) + x.second});
                }
            }
            for (const Input& in : inputs) {
            DeviceInfo d;
            d.kind = DeviceInfo::Native;
            d.board = "usrp";
            d.nativeArgs = in.antenna.empty() && in.ch == 0 ? args : args + "#" + std::to_string(in.ch) + "#" + in.antenna;
            d.serial = field("serial");
            d.name = "USRP " + prod + (d.serial.empty() ? "" : " " + d.serial) + (in.label.empty() ? "" : " " + in.label) + " (native, experimental)";
            d.maxRateHz = 56e6; d.minRateHz = 0.2e6;
            d.gainMinDb = 0; d.gainMaxDb = 76;
            d.settings = {ppmSetting(0.01),
                          choiceSetting("clock", "Reference clock",
                                        "The 10 MHz reference the USRP locks to. Default: UHD's choice (the internal oscillator; a B2xx with a GPSDO fitted\n"
                                        "uses the GPSDO). External: a 10 MHz reference on the REF input. Takes effect when the radio is started.",
                                        {"default", "internal", "external", "gpsdo"}, {"Default", "Internal", "External 10 MHz", "GPSDO"}, "default", true)};
            {
                std::lock_guard<std::mutex> lk(gRangeMu);
                auto it = gRange.find(args);
                if (it != gRange.end()) { d.minFreqHz = it->second.first; d.maxFreqHz = it->second.second; }
                else if (field("type") == "b200") { d.minFreqHz = 70e6; d.maxFreqHz = 6e9; }   // B200, B210, B200mini, B205mini (AD936x)
            }
            out.push_back(d);
            }
        }
    }
    uhd().strvec_free(&v);
}

std::unique_ptr<IqSource> makeUsrp(const DeviceInfo& d) {
    return std::make_unique<UsrpSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
