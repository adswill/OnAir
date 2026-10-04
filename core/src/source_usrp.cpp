// Native driver for the USRP: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"

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

std::unique_ptr<IqSource> makeUsrp(const DeviceInfo& d) {
    return std::make_unique<UsrpSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
