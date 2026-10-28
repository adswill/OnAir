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
    err_t (DECT2_CALL* set_time_source)(usrp_t*, const char*, size_t) = nullptr;    // optional: where the PPS (time) comes from
    // optional: what the motherboard offers (get_clock_sources / get_time_sources) and the "ref_locked" sensor
    err_t (DECT2_CALL* get_clock_sources)(usrp_t*, size_t, strvec_t**) = nullptr;
    err_t (DECT2_CALL* get_time_sources)(usrp_t*, size_t, strvec_t**) = nullptr;
    err_t (DECT2_CALL* get_mboard_sensor)(usrp_t*, const char*, size_t, void**) = nullptr;
    err_t (DECT2_CALL* sensor_make)(void**) = nullptr;
    err_t (DECT2_CALL* sensor_free)(void**) = nullptr;
    err_t (DECT2_CALL* sensor_to_bool)(void*, bool*) = nullptr;
    // optional: the transmit side (UsrpTx). All of them present = hasTx; a UHD without them leaves the radio receive-only
    struct tx_streamer_t; struct tx_metadata_t;
    err_t (DECT2_CALL* set_tx_rate)(usrp_t*, double, size_t) = nullptr;
    err_t (DECT2_CALL* get_tx_rate)(usrp_t*, size_t, double*) = nullptr;
    err_t (DECT2_CALL* set_tx_freq)(usrp_t*, tune_request_t*, size_t, tune_result_t*) = nullptr;
    err_t (DECT2_CALL* set_tx_gain)(usrp_t*, double, size_t, const char*) = nullptr;
    err_t (DECT2_CALL* get_tx_gain_range)(usrp_t*, const char*, size_t, range_t_*) = nullptr;
    err_t (DECT2_CALL* set_tx_antenna)(usrp_t*, const char*, size_t) = nullptr;
    err_t (DECT2_CALL* set_tx_bandwidth)(usrp_t*, double, size_t) = nullptr;
    err_t (DECT2_CALL* get_tx_num_channels)(usrp_t*, size_t*) = nullptr;
    err_t (DECT2_CALL* get_tx_stream)(usrp_t*, stream_args_t*, tx_streamer_t*) = nullptr;
    err_t (DECT2_CALL* tx_streamer_make)(tx_streamer_t**) = nullptr;
    err_t (DECT2_CALL* tx_streamer_free)(tx_streamer_t**) = nullptr;
    err_t (DECT2_CALL* tx_streamer_max_num_samps)(tx_streamer_t*, size_t*) = nullptr;
    err_t (DECT2_CALL* tx_streamer_send)(tx_streamer_t*, const void**, size_t, tx_metadata_t**, double, size_t*) = nullptr;
    err_t (DECT2_CALL* tx_metadata_make)(tx_metadata_t**, bool, int64_t, double, bool, bool) = nullptr;
    err_t (DECT2_CALL* tx_metadata_free)(tx_metadata_t**) = nullptr;
    bool hasTx = false;
    bool ok = false;
    DynLib lib;
    UhdApi() {
        // Windows: the UHD the user installed from Ettus (uhd.dll, on the PATH) first, the one OnAir ships (libuhd.dll) only without it. A B210
        // shows up only after UHD has loaded its firmware, from the images of its own version (UHD_IMAGES_DIR of the Ettus install): our libuhd
        // first made a B210 that the Ettus UHD (and OnAir 0.2.0) saw disappear on a Windows 10 PC with UHD installed.
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
        if (ok) {
            lib.opt(get_last_error, "uhd_get_last_error"); lib.opt(usrp_last_error, "uhd_usrp_last_error"); lib.opt(get_rx_freq_range, "uhd_usrp_get_rx_freq_range");
            lib.opt(get_rx_num_channels, "uhd_usrp_get_rx_num_channels"); lib.opt(get_rx_antennas, "uhd_usrp_get_rx_antennas"); lib.opt(set_rx_antenna, "uhd_usrp_set_rx_antenna");
            lib.opt(set_clock_source, "uhd_usrp_set_clock_source"); lib.opt(set_time_source, "uhd_usrp_set_time_source");
            lib.opt(get_clock_sources, "uhd_usrp_get_clock_sources"); lib.opt(get_time_sources, "uhd_usrp_get_time_sources");
            lib.opt(get_mboard_sensor, "uhd_usrp_get_mboard_sensor"); lib.opt(sensor_make, "uhd_sensor_value_make");
            lib.opt(sensor_free, "uhd_sensor_value_free"); lib.opt(sensor_to_bool, "uhd_sensor_value_to_bool");
            lib.opt(set_tx_rate, "uhd_usrp_set_tx_rate"); lib.opt(get_tx_rate, "uhd_usrp_get_tx_rate"); lib.opt(set_tx_freq, "uhd_usrp_set_tx_freq");
            lib.opt(set_tx_gain, "uhd_usrp_set_tx_gain"); lib.opt(get_tx_gain_range, "uhd_usrp_get_tx_gain_range"); lib.opt(set_tx_antenna, "uhd_usrp_set_tx_antenna");
            lib.opt(set_tx_bandwidth, "uhd_usrp_set_tx_bandwidth"); lib.opt(get_tx_num_channels, "uhd_usrp_get_tx_num_channels");
            lib.opt(get_tx_stream, "uhd_usrp_get_tx_stream"); lib.opt(tx_streamer_make, "uhd_tx_streamer_make"); lib.opt(tx_streamer_free, "uhd_tx_streamer_free");
            lib.opt(tx_streamer_max_num_samps, "uhd_tx_streamer_max_num_samps"); lib.opt(tx_streamer_send, "uhd_tx_streamer_send");
            lib.opt(tx_metadata_make, "uhd_tx_metadata_make"); lib.opt(tx_metadata_free, "uhd_tx_metadata_free");
            hasTx = set_tx_rate && get_tx_rate && set_tx_freq && set_tx_gain && get_tx_gain_range && set_tx_antenna && get_tx_stream && tx_streamer_make &&
                    tx_streamer_free && tx_streamer_max_num_samps && tx_streamer_send && tx_metadata_make && tx_metadata_free;
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

std::string usrpDeviceArgs(const std::string& args_, bool warn) {
    std::string dargs = args_;
    const bool net = contains(args_, "addr") || contains(args_, "type=usrp2") || contains(args_, "type=x300") || contains(args_, "type=n3xx");
    if (net) {
#ifdef __linux__
        if (!contains(dargs, "recv_buff_size")) dargs += std::string(dargs.empty() ? "" : ",") + "recv_buff_size=50000000";
        if (warn) if (FILE* f = fopen("/proc/sys/net/core/rmem_max", "r")) {
            long v = 0;
            if (fscanf(f, "%ld", &v) == 1 && v < 50000000) fprintf(stderr, "USRP: the socket receive buffer limit is small (%ld); to avoid dropped samples run: sudo sysctl -w net.core.rmem_max=50000000\n", v);
            fclose(f);
        }
#endif
        if (!contains(dargs, "num_recv_frames")) dargs += std::string(dargs.empty() ? "" : ",") + "num_recv_frames=512";
    }
    const bool bus = contains(args_, "type=b200");
    if (bus) {
        if (!contains(dargs, "num_recv_frames")) dargs += std::string(dargs.empty() ? "" : ",") + "num_recv_frames=128";
    }
    return dargs;
}

// One UHD handle per device arguments, shared by the receiver and the transmitter of the same USRP: a USB USRP cannot be opened twice, so the
// transmitter used to be refused while the receiver ran (issue #294: receive on RX2, transmit on TX/RX). The count frees the handle with the last user.
std::mutex gDevMu;
std::map<std::string, std::pair<UhdApi::usrp_t*, int>> gDevs;
std::map<std::string, int> gRxOnTxRx;   // device arguments of the receivers that listen on the TX/RX connector (the transmitter needs it)
bool rxHoldsTxRx(const std::string& dargs) { std::lock_guard<std::mutex> lk(gDevMu); auto it = gRxOnTxRx.find(dargs); return it != gRxOnTxRx.end() && it->second > 0; }
void rxTxRx(const std::string& dargs, int d) { std::lock_guard<std::mutex> lk(gDevMu); gRxOnTxRx[dargs] += d; }
bool usrpAcquire(const std::string& dargs, UhdApi::usrp_t** out) {
    std::lock_guard<std::mutex> lk(gDevMu);
    auto it = gDevs.find(dargs);
    if (it != gDevs.end()) { it->second.second++; *out = it->second.first; return true; }
    UhdApi::usrp_t* d = nullptr;
    if (uhd().make(&d, dargs.c_str()) != 0 || !d) {
        if (d) uhd().free_(&d);
        *out = nullptr;
        return false;
    }
    gDevs[dargs] = {d, 1};
    *out = d;
    return true;
}
void usrpRelease(UhdApi::usrp_t** dev) {
    if (!*dev) return;
    std::lock_guard<std::mutex> lk(gDevMu);
    for (auto it = gDevs.begin(); it != gDevs.end(); ++it)
        if (it->second.first == *dev) {
            if (--it->second.second <= 0) { UhdApi::usrp_t* d = *dev; uhd().free_(&d); gDevs.erase(it); }
            break;
        }
    *dev = nullptr;
}

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
        otw_ = radioOption(s, "wire", "sc16") == "sc8" ? "sc8" : "sc16";
        // a network USRP (USRP2, N2xx, X3xx, N3xx) drops samples with the small default socket buffers: ask for the ones UHD's docs recommend
        // unless the user set them in the device arguments (issues #61/#62)
        const std::string dargs = usrpDeviceArgs(args_, true);
        if (!usrpAcquire(dargs, &dev_)) {
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
            std::string offered;
            if (!sourceOffered(true, clk, offered)) {
                err = "USRP: this radio has no reference clock \"" + clk + "\" (it offers: " + offered + ")";
                openFailure_ = OpenFailure::Final;
                return false;
            }
            if (!uhd().set_clock_source || uhd().set_clock_source(dev_, clk.c_str(), 0) != 0) {
                err = "USRP: the reference clock \"" + clk + "\" was refused" + (uhd().set_clock_source ? ": " + uhd().text(dev_) : std::string(" (this UHD cannot set it)"));
                openFailure_ = OpenFailure::Final;
                return false;
            }
        }
        // the time (PPS) source (TuneSettings "time"): UHD's own choice unless picked. "external" = the PPS SMA, "gpsdo" = the GPS module, "mimo" = the
        // MIMO cable of an N2xx/X3xx pair. Sample capture does not need it; it only matters when radios are to be aligned in time.
        if (const std::string ts = radioOption(s, "time", "default"); ts != "default") {
            std::string offered;
            if (!sourceOffered(false, ts, offered)) {
                err = "USRP: this radio has no time source \"" + ts + "\" (it offers: " + offered + ")";
                openFailure_ = OpenFailure::Final;
                return false;
            }
            if (!uhd().set_time_source || uhd().set_time_source(dev_, ts.c_str(), 0) != 0) {
                err = "USRP: the time source \"" + ts + "\" was refused" + (uhd().set_time_source ? ": " + uhd().text(dev_) : std::string(" (this UHD cannot set it)"));
                openFailure_ = OpenFailure::Final;
                return false;
            }
        }
        if (radioOption(s, "clock", "default") != "default") lockNote_ = refLockNote(); else lockNote_.clear();
        rememberInputs();
        if (size_t n = 0; ch_ > 0 && uhd().get_rx_num_channels && uhd().get_rx_num_channels(dev_, &n) == 0 && ch_ >= n) {
            err = "USRP: this radio has " + std::to_string(n) + " receive channel(s), channel " + std::to_string(ch_ + 1) + " was picked";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (!antenna_.empty()) {   // the default entry leaves UHD's choice alone (RX2 on a B2xx)
            if (!uhd().set_rx_antenna) { err = "USRP: this UHD cannot pick the antenna input (no uhd_usrp_set_rx_antenna)"; openFailure_ = OpenFailure::Final; return false; }
            if (uhd().set_rx_antenna(dev_, antenna_.c_str(), ch_) != 0) { err = "USRP: the antenna input " + antenna_ + " was refused: " + uhd().text(dev_); openFailure_ = OpenFailure::Final; return false; }
            if (antenna_ == "TX/RX" && !onTxRx_) { onTxRx_ = true; rxTxRx(dargs, 1); dargsKept_ = dargs; }
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
        const bool cfg = configure(s, err, false);
        if (cfg) addNote(err, lockNote_);
        return cfg;
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
            const double want = s.rateChosen && s.sampleRate > 0 ? s.sampleRate : std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, lo);   // (3.4: a chosen rate goes to UHD as it is)
            if (uhd().set_rx_rate(dev_, want, ch) != 0 && (want <= hi || uhd().set_rx_rate(dev_, hi, ch) != 0)) { err = "USRP: the sample rate was refused: " + uhd().text(dev_); return false; }
            double actual = std::min(want, hi);
            uhd().get_rx_rate(dev_, ch, &actual);
            rate_ = actual;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
            uhd().set_rx_bandwidth(dev_, std::max(0.2e6, s.basebandFilterHz > 0 ? s.basebandFilterHz : rate_ * 0.95), ch);
        }
        // UHD may clip a frequency outside the range without failing: it is asked all the same, and the actual frequency is checked below
        const double f = std::max(s.centerHz, 0.0);
        const std::string range = hi_ > 0 ? " (range " + mhz(lo_) + "-" + mhz(hi_) + " MHz)" : std::string();
        char noArgs[1] = {0};
        // UHD has no frequency correction (a USRP takes an external reference or a GPSDO for that): a correction the user set is taken out of the frequency
        UhdApi::tune_request_t req{ppmCorrectedHz(f, radioPpm(s)), UhdApi::kPolicyAuto, 0, UhdApi::kPolicyAuto, 0, noArgs};
        UhdApi::tune_result_t res{};
        if (uhd().set_rx_freq(dev_, &req, ch, &res) != 0) { err = "USRP: the driver refused " + mhz(f) + " MHz" + range + ": " + uhd().text(dev_); return false; }
        addNote(err, freqSpecNote("USRP", f, lo_, hi_));
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
        if (onTxRx_) { rxTxRx(dargsKept_, -1); onTxRx_ = false; }
        usrpRelease(&dev_);
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
        // fc32 out of UHD: it converts sc8 (half the Ethernet load, 8-bit samples) and sc16 on the wire itself
        char cpu[] = "fc32", none[] = "";
        char otw[8];
        snprintf(otw, sizeof otw, "%s", otw_.c_str());
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
    // The sources the radio offers ("" list = the library cannot say): the user's pick is checked against them, so a radio without a GPSDO
    // says what it does have instead of UHD's bare exception
    bool sourceOffered(bool clock, const std::string& want, std::string& offered) {
        auto fn = clock ? uhd().get_clock_sources : uhd().get_time_sources;
        offered.clear();
        if (!fn) return true;
        UhdApi::strvec_t* v = nullptr;
        if (uhd().strvec_make(&v) != 0 || !v) return true;
        bool found = false, known = false;
        size_t k = 0;
        if (fn(dev_, 0, &v) == 0 && uhd().strvec_size(v, &k) == 0 && k > 0)
            for (size_t i = 0; i < k && i < 16; i++) {
                char b[128] = {0};
                if (uhd().strvec_at(v, i, b, sizeof b) != 0 || !b[0]) continue;
                known = true;
                if (want == b) found = true;
                offered += (offered.empty() ? "" : ", ") + std::string(b);
            }
        uhd().strvec_free(&v);
        return found || !known;
    }
    // "reference locked" after a clock source was picked: the motherboard's ref_locked sensor, polled for up to 3 s (a GPSDO or an external
    // reference takes a moment). Returns a plain-words note, "" when the radio has no such sensor
    std::string refLockNote() {
        if (!uhd().get_mboard_sensor || !uhd().sensor_make || !uhd().sensor_free || !uhd().sensor_to_bool) return "";
        for (int i = 0; i < 12; i++) {
            void* sv = nullptr;
            if (uhd().sensor_make(&sv) != 0 || !sv) return "";
            bool locked = false;
            const bool got = uhd().get_mboard_sensor(dev_, "ref_locked", 0, &sv) == 0 && uhd().sensor_to_bool(sv, &locked) == 0;
            uhd().sensor_free(&sv);
            if (!got) return "";
            if (locked) return "reference locked";
            if (i < 11) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return "the reference clock is NOT locked (check the 10 MHz / GPSDO cable and that the signal is present)";
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
    std::string otw_ = "sc16";   // the wire format ("wire" setting)
    std::string lockNote_;   // "reference locked" / not locked, shown with the start message
    bool onTxRx_ = false;
    std::string dargsKept_;
    std::string args_, antenna_;   // antenna_: the input the user picked ("" = UHD's default)
    size_t ch_ = 0;                // the receive channel (the B210's RF A = 0, RF B = 1)
    UhdApi::usrp_t* dev_ = nullptr;
    UhdApi::streamer_t* streamer_ = nullptr;
    UhdApi::metadata_t* md_ = nullptr;
    double lo_ = 0, hi_ = 0;   // RX tuning range the library reports (0 = not known)
};

// ---- transmit -------------------------------------------------------------------------------------------------------------------------
// The USRP's transmit path through UHD's C API: tx_streamer, fc32 samples (sc16 on the wire), the TX/RX connector of the chosen output (a B210:
// RF A = 0, RF B = 1). Samples beyond +-1.0 are clipped. The radio is opened on its own (a USB USRP cannot be opened twice), so RX and TX of one
// USRP never run together: the device claim makes the TX window wait for the receiver that holds it.
class UsrpTx : public TxSink {
public:
    explicit UsrpTx(const std::string& args) { args_ = args.substr(0, args.find('#')); }
    ~UsrpTx() override { close(); }
    bool open(const TxTune& t, std::string& err) override {
        close();
        if (!uhd().ok) { err = "USRP: UHD is not installed, it cannot transmit"; return false; }
        if (!uhd().hasTx) { err = "USRP: the installed UHD has no transmit functions (uhd_usrp_set_tx_rate, uhd_tx_streamer_send, ...); install UHD 3.15 or newer"; return false; }
        const std::string dargs = usrpDeviceArgs(args_, false);
        if (rxHoldsTxRx(dargs)) { err = "USRP: the receiver is listening on the TX/RX connector, which the transmitter needs. Pick the RX2 input for receiving, or stop the receiver first."; return false; }
        if (!usrpAcquire(usrpDeviceArgs(args_, false), &dev_)) {
            const std::string x = uhd().text(nullptr);
            err = "USRP: cannot open the radio" + (x.empty() ? std::string(" (in use by a receiver?)") : ": " + x);
            return false;
        }
        const size_t ch = (size_t)std::max(0, t.channel);
        if (size_t n = 0; uhd().get_tx_num_channels && uhd().get_tx_num_channels(dev_, &n) == 0 && ch >= n) {
            err = "USRP: this radio has " + std::to_string(n) + " transmit output(s), output " + std::to_string(ch + 1) + " was picked"; shut(); return false;
        }
        const double rate = std::max(t.sampleRate > 0 ? t.sampleRate : 1e6, 0.2e6);
        if (uhd().set_tx_rate(dev_, rate, ch) != 0) { err = "USRP: the sample rate " + mhz(rate) + " Msps was refused: " + uhd().text(dev_); shut(); return false; }
        rate_ = rate;
        uhd().get_tx_rate(dev_, ch, &rate_);
        if (uhd().set_tx_bandwidth) uhd().set_tx_bandwidth(dev_, std::max(0.2e6, t.bandwidthHz > 0 ? t.bandwidthHz : rate_ * 0.95), ch);
        if (uhd().set_tx_antenna(dev_, "TX/RX", ch) != 0) { err = "USRP: the transmit connector TX/RX was refused: " + uhd().text(dev_); shut(); return false; }
        TuneSettings ts; ts.radio = t.radio;
        char noArgs[1] = {0};
        UhdApi::tune_request_t req{std::max(ppmCorrectedHz(txRadioHz(t), radioPpm(ts)), 0.0), UhdApi::kPolicyAuto, 0, UhdApi::kPolicyAuto, 0, noArgs};
        UhdApi::tune_result_t res{};
        if (uhd().set_tx_freq(dev_, &req, ch, &res) != 0) { err = "USRP: the driver refused " + mhz(txRadioHz(t)) + " MHz as a transmit frequency: " + uhd().text(dev_); shut(); return false; }
        double gmin = 0, gmax = 89.75;
        if (UhdApi::range_t_* rg = nullptr; uhd().get_tx_gain_range && uhd().meta_range_make(&rg) == 0 && rg) {
            if (uhd().get_tx_gain_range(dev_, "", ch, rg) == 0) { uhd().meta_range_start(rg, &gmin); uhd().meta_range_stop(rg, &gmax); }
            uhd().meta_range_free(&rg);
        }
        if (uhd().set_tx_gain(dev_, std::min(std::max(t.gainDb, gmin), gmax), ch, "") != 0) { err = "USRP: the transmit gain was refused: " + uhd().text(dev_); shut(); return false; }
        size_t chan = ch;
        char cpu[] = "fc32", otw[] = "sc16", none[] = "";
        UhdApi::stream_args_t sa{cpu, otw, none, &chan, 1};
        if (uhd().tx_streamer_make(&st_) != 0 || uhd().get_tx_stream(dev_, &sa, st_) != 0) { err = "USRP: cannot set up the transmit stream: " + uhd().text(dev_); shut(); return false; }
        size_t m = 0;
        uhd().tx_streamer_max_num_samps(st_, &m);
        maxN_ = m > 0 ? m : 8192;
        open_ = true; sob_ = true;
        return true;
    }
    bool write(const cf32* x, size_t n, std::string& err) override {
        if (!open_) { err = "USRP: the transmitter is not open"; return false; }
        size_t done = 0;
        while (done < n) {
            const size_t k = std::min(n - done, maxN_);
            buf_.resize(k);
            for (size_t i = 0; i < k; i++) buf_[i] = cf32(std::max(-1.f, std::min(1.f, x[done + i].real())), std::max(-1.f, std::min(1.f, x[done + i].imag())));
            const void* b[1] = {buf_.data()};
            size_t sent = 0;
            // the first packet starts the burst; the end of burst goes out with close()
            UhdApi::tx_metadata_t* md = nullptr;
            if (uhd().tx_metadata_make(&md, false, 0, 0.0, sob_, false) != 0 || !md) { err = "USRP: cannot make the transmit packet header"; return false; }
            sob_ = false;
            const int e = uhd().tx_streamer_send(st_, b, k, &md, 1.0, &sent);
            uhd().tx_metadata_free(&md);
            if (e != 0) { err = "USRP: the transmit stream failed: " + uhd().text(dev_); return false; }
            if (sent == 0) { if (++stalls_ > 5) { err = "USRP: the radio takes no transmit samples"; return false; } } else stalls_ = 0;
            done += sent;
        }
        return true;
    }
    void close() override {
        if (st_ && open_ && !sob_) {   // end of burst: an empty packet, then the radio stops sending
            UhdApi::tx_metadata_t* md = nullptr;
            if (uhd().tx_metadata_make(&md, false, 0, 0.0, false, true) == 0 && md) {
                size_t sent = 0;
                const void* b[1] = {buf_.data()};
                uhd().tx_streamer_send(st_, b, 0, &md, 0.5, &sent);
                uhd().tx_metadata_free(&md);
            }
        }
        open_ = false; sob_ = false; stalls_ = 0;
        shut();
    }
    double sampleRate() const override { return rate_; }
    std::string name() const override { return "USRP"; }
private:
    void shut() {
        if (st_) uhd().tx_streamer_free(&st_);
        st_ = nullptr;
        usrpRelease(&dev_);
    }
    std::string args_;
    UhdApi::usrp_t* dev_ = nullptr;
    UhdApi::tx_streamer_t* st_ = nullptr;
    std::vector<cf32> buf_;
    size_t maxN_ = 8192;
    double rate_ = 0;
    int stalls_ = 0;
    bool open_ = false, sob_ = false;
};
std::unique_ptr<TxSink> makeUsrpTx(const DeviceInfo& d) { return std::make_unique<UsrpTx>(d.nativeArgs); }

void listUsrp(std::vector<DeviceInfo>& out) {
    if (!uhd().ok) return;
    UhdApi::strvec_t* v = nullptr;
    if (uhd().strvec_make(&v) != 0 || !v) return;
    // without network discovery (#215) the hint keeps UHD to the USB B2xx family: an empty hint broadcasts for N/X-series USRPs on every network
    if (uhd().find(networkDiscovery() ? "" : "type=b200", &v) == 0) {
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
            // Transmit: every receive entry of one USRP is the same device; the TX window lists it once under txName (not-hasTx UHD: receive-only)
            d.canTransmit = uhd().hasTx;
            d.txFullDuplex = d.canTransmit && in.antenna != "TX/RX";   // receiving on RX2 and transmitting on TX/RX share the one open radio (#294)
            d.txName = "USRP " + prod + (d.serial.empty() ? "" : " " + d.serial) + " (native, experimental)";
            d.txChannels = prod == "B210" ? 2 : 1;
            d.txMinRateHz = 0.2e6; d.txMaxRateHz = 56e6;
            d.txGainMinDb = 0; d.txGainMaxDb = 89.75;
            d.txGainNote = "the USRP's own TX gain in dB (a B2xx: 0 to 89.75 dB), on the TX/RX connector";
            d.settings = {ppmSetting(0.01),
                          choiceSetting("clock", "Reference clock",
                                        "The 10 MHz reference the USRP locks to. Default: UHD's choice (the internal oscillator; a B2xx with a GPSDO fitted\n"
                                        "uses the GPSDO). External: a 10 MHz reference on the REF input (the radio locks its oscillator to it). A source this\n"
                                        "radio does not have is refused with the list it offers; whether the reference locked is shown when the radio starts.\n"
                                        "Takes effect when the radio is started.",
                                        {"default", "internal", "external", "gpsdo"}, {"Default", "Internal", "External 10 MHz", "GPSDO"}, "default", true),
                          choiceSetting("time", "Time source (PPS)",
                                        "Where the radio takes its one-pulse-per-second time reference from. Default: UHD's choice. None: no PPS. External: a PPS\n"
                                        "on the PPS input. GPSDO: the GPS module. MIMO: the MIMO cable of an N2xx / X3xx pair. Only needed to line radios up in\n"
                                        "time. Takes effect when the radio is started.",
                                        {"default", "none", "external", "gpsdo", "mimo"}, {"Default", "None", "External PPS", "GPSDO", "MIMO cable"}, "default", true),
                          choiceSetting("wire", "Wire format",
                                        "The sample format between the USRP and the computer. sc16: 16-bit samples (default). sc8: 8-bit samples, half the\n"
                                        "Ethernet/USB load: use it when a network USRP drops samples at high rates. Takes effect when the radio is started.",
                                        {"sc16", "sc8"}, {"sc16 (16-bit)", "sc8 (8-bit, half the load)"}, "sc16", true)};
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
