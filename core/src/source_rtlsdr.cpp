// Native driver for the RTL-SDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "native_common.h"
#include "dect2/mode_tuning.h"
#include <map>

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
    int (DECT2_CALL* read_sync)(rtlsdr_dev_t*, void*, int, int*) = nullptr;   // optional (every librtlsdr has it): see streamLoop()
    int (DECT2_CALL* get_tuner_type)(rtlsdr_dev_t*) = nullptr;         // optional
    int (DECT2_CALL* set_direct_sampling)(rtlsdr_dev_t*, int) = nullptr;  // optional
    int (DECT2_CALL* set_bias_tee)(rtlsdr_dev_t*, int) = nullptr;         // optional: the RTL-SDR Blog library and osmocom 0.6 and later
    int (DECT2_CALL* set_freq_correction)(rtlsdr_dev_t*, int) = nullptr;  // optional (every librtlsdr has it): whole ppm
    int (DECT2_CALL* set_offset_tuning)(rtlsdr_dev_t*, int) = nullptr;    // optional
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
        if (ok) {
            lib.opt(get_tuner_type, "rtlsdr_get_tuner_type"); lib.opt(set_direct_sampling, "rtlsdr_set_direct_sampling"); lib.opt(set_bias_tee, "rtlsdr_set_bias_tee");
            lib.opt(set_freq_correction, "rtlsdr_set_freq_correction"); lib.opt(set_offset_tuning, "rtlsdr_set_offset_tuning");
            lib.opt(read_sync, "rtlsdr_read_sync");
        }
    }
};
RtlApi& rtl() { static RtlApi a; return a; }

namespace {

enum { kTunerUnknown = 0, kE4000 = 1, kFC0012 = 2, kFC0013 = 3, kFC2580 = 4, kR820T = 5, kR828D = 6 };   // enum rtlsdr_tuner
constexpr double kDirectBelowHz = 24e6;   // the R820T/R828D tuners stop here

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

// What the dongle's USB strings say about its HF input:
// - the RTL-SDR Blog V4 (R828D) and V4 Lite (R820T, USB product "Blog V4L") have an upconverter for HF that their library switches itself: no
//   direct sampling. Same test as the Blog library (rtlsdr_check_dongle_model with "Blog V4" and "Blog V4L"; it leaves the V4L out of its own
//   direct sampling below 24 MHz, tuner_r82xx.c upconverts below 28.8 MHz).
// - the RTL-SDR Blog V3 ("RTLSDRBlog" / "Blog V3") has its HF input on the RTL2832U's Q branch: direct sampling below 24 MHz.
// - any other dongle has no HF input unless it was modified (the user can pick the I or Q input in the radio settings)
enum RtlModel { kGeneric, kBlogV3, kBlogV4 };
RtlModel rtlModel(uint32_t index) {
    char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
    if (rtl().get_device_usb_strings(index, vendor, product, serial) != 0) return kGeneric;
    const std::string v = trimmed(vendor, sizeof vendor), p = trimmed(product, sizeof product);
    if ((v == "RTLSDRBlog" && (p == "Blog V4" || p == "Blog V4L")) || p.find("RTL-SDR Blog V4") != std::string::npos) return kBlogV4;
    if ((v == "RTLSDRBlog" && p == "Blog V3") || p.find("RTL-SDR Blog V3") != std::string::npos) return kBlogV3;
    return kGeneric;
}

// Auto direct sampling: the V3's Q-branch HF input below 24 MHz
bool directCapable(int tuner, RtlModel m) { return (tuner == kR820T || tuner == kR828D) && m == kBlogV3 && rtl().set_direct_sampling; }
// librtlsdr's offset tuning (no DC spike) works with the E4000 and Fitipower tuners only; on an R820T/R828D the RTL-SDR Blog library even
// switches the antenna power with it (rtlsdr_set_offset_tuning), so it is never called there
bool offsetCapable(int tuner) { return (tuner == kE4000 || tuner == kFC0012 || tuner == kFC0013 || tuner == kFC2580) && rtl().set_offset_tuning; }

// The settings of the radio's entry (TuneSettings::radio)
std::vector<RadioSetting> rtlSettings(int tuner, RtlModel m) {
    std::vector<RadioSetting> v = {ppmSetting(1)};   // librtlsdr takes whole ppm
    if (m != kBlogV4 && rtl().set_direct_sampling)
        v.push_back(choiceSetting("ds", "Direct sampling (HF)",
                                  "Below 24 MHz the tuner cannot receive: the RTL2832U can sample an HF antenna on one of its inputs directly.\n"
                                  "Auto: the Q input below 24 MHz on an RTL-SDR Blog V3 (its HF input); other dongles have none unless modified.\n"
                                  "I / Q: that input below 24 MHz, whatever the dongle (dongles modified for HF). Off: never.",
                                  {"auto", "off", "i", "q"}, {"Auto", "Off", "I input", "Q input"}, "auto"));
    if (offsetCapable(tuner))
        v.push_back(boolSetting("offset", "Offset tuning", "Tunes the E4000/FC00xx tuner off the centre and shifts back digitally: no DC spike in the middle\n(not with direct sampling)."));
    return v;
}

// tuning range by tuner, as librtlsdr's tuner drivers accept it
// (with the HF input of a V3 or the V4's upconverter from 0.5 MHz)
void tunerRange(int tuner, RtlModel m, double& lo, double& hi) {
    switch (tuner) {
    case kE4000: lo = 52e6; hi = 2200e6; break;
    case kFC0012: lo = 22e6; hi = 948.6e6; break;
    case kFC0013: lo = 22e6; hi = 1100e6; break;
    case kFC2580: lo = 146e6; hi = 924e6; break;
    case kR820T: case kR828D: lo = (m == kBlogV4 || directCapable(tuner, m)) ? 0.5e6 : kDirectBelowHz; hi = 1766e6; break;
    default: lo = hi = 0;
    }
}

// The radio must be open to tell its tuner; a radio that is streaming cannot be opened again, so the answer is kept.
std::mutex gTunerMu;
std::map<std::string, int> gTuner;

const char* familyOf(const TuneSettings& s) {
    if (s.synth.atsc) return "ATSC";
    if (s.synth.mode == 5) return "ATSC 3.0";
    if (s.synth.mode == 6) return "ISDB-T";
    if (const ModeTuning* mt = s.synth.mode >= 8 ? modeTuning(s.synth.mode) : nullptr) return mt->name;
    if (s.bandwidthMhz >= 5) return "DVB-T/T2";
    return "this mode";
}

} // namespace

class RtlSource : public NativeSource {
public:
    RtlSource(uint32_t index, std::string serial) : index_(index), serial_(std::move(serial)) {}
    ~RtlSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        // librtlsdr opens by position in the USB list: with two dongles, an unplug or a new one shifts it, and the other radio (with its
        // antenna power) would be opened. The serial of the listing finds the radio again.
        const int at = indexOfSerial();
        if (at == -2) { err = "RTL-SDR " + serial_ + " not found (unplugged?)"; return false; }
        const uint32_t index = at >= 0 ? (uint32_t)at : index_;
        const int r = rtl().open(&dev_, index);
        if (r != 0 || !dev_) {
            dev_ = nullptr;
            // librtlsdr hands back libusb's error code; a missing driver or permission does not pass by trying again
            err = "RTL-SDR: cannot open the radio: " + usbErrorText(r);
            if (r == -3 || r == -12 || (kOnWindows && usbDriverMissing(r))) openFailure_ = OpenFailure::Final;
            return false;
        }
        model_ = rtlModel(index);
        v4_ = model_ == kBlogV4;
        tuner_ = rtl().get_tuner_type ? rtl().get_tuner_type(dev_) : kTunerUnknown;
        tunerRange(tuner_, model_, lo_, hi_);
        ds_ = 0;
        bias_ = offset_ = false;
        ppm_ = 0;   // librtlsdr opens without a correction
        rtl().set_agc_mode(dev_, 0);
        rtl().set_tuner_gain_mode(dev_, 1);   // manual gain: OnAir runs its own AGC
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        if (!live) {
            // a TV channel cannot be squeezed into what a dongle delivers: say so instead of streaming a useless rate
            if (s.sampleRate > 3.2e6) {
                char b[200];
                snprintf(b, sizeof b, "RTL-SDR gives at most 3.2 Msps; %s needs about %.0f Msps: use a faster radio", familyOf(s), s.sampleRate / 1e6);
                err = b;
                openFailure_ = OpenFailure::Final;
                return false;
            }
            // stable rates are 225-300 kHz and 0.9-3.2 MHz; above about 2.56 MHz most dongles drop samples
            double want = std::min(s.sampleRate > 0 ? s.sampleRate : 2.048e6, 2.56e6);
            if (want < 900001 && (want > 300000 || want <= 225000)) want = 900001;   // librtlsdr refuses 225 kHz and below as well
            if (const int r = rtl().set_sample_rate(dev_, (uint32_t)std::lround(want)); r < 0) {
                err = "RTL-SDR: the sample rate of " + mhz(want) + " Msps was refused (error " + std::to_string(r) + ")";
                return false;
            }
            rate_ = rtl().get_sample_rate(dev_);
            if (rate_ <= 0) rate_ = want;
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = std::max(s.centerHz, 0.0);
        // below 24 MHz the R820T/R828D cannot tune: on a Blog V3 the HF signal on the Q input of the RTL2832U is sampled directly (Auto);
        // the radio settings can name the input for a modified dongle, or switch it off (TuneSettings "ds")
        const std::string mode = radioOption(s, "ds", "auto");
        int ds = 0;   // librtlsdr: 0 off, 1 I input, 2 Q input
        if (mode == "i" || mode == "q") ds = !v4_ && rtl().set_direct_sampling && f < kDirectBelowHz ? (mode == "i" ? 1 : 2) : 0;
        else if (mode != "off") ds = directCapable(tuner_, model_) && f < kDirectBelowHz ? 2 : 0;
        // the direct-sampling input works from 0.5 MHz, the tuner from its own lower end (24 MHz for an R820T/R828D without the V4's upconverter)
        const double tunerLo = !v4_ && (tuner_ == kR820T || tuner_ == kR828D) ? kDirectBelowHz : lo_;
        const double lo = mode == "off" ? tunerLo : mode == "auto" ? lo_ : (!v4_ && rtl().set_direct_sampling ? 0.5e6 : lo_);   // for the message
        if (hi_ > 0 && (f > hi_ || f < (ds ? 0.5e6 : tunerLo))) {
            err = "RTL-SDR could not tune to " + mhz(f) + " MHz (range " + mhz(lo) + "-" + mhz(hi_) + " MHz" + (mode == "off" && tunerLo == kDirectBelowHz ? "; direct sampling is off" : "") + ")";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (ds != ds_) {
            rtl().set_direct_sampling(dev_, ds);
            ds_ = ds;
            if (!ds) offset_ = false;   // offset tuning is refused while direct sampling is on: set again below
            fprintf(stderr, "RTL-SDR: direct sampling%s %s at %s MHz\n", ds == 1 ? " (I branch)" : " (Q branch)", ds ? "on" : "off", mhz(f).c_str());
            fflush(stderr);
        }
        // frequency correction in whole ppm (the library also corrects the sample clock); a library without it: the frequency asked for
        const double ppm = radioPpm(s);
        double tuneHz = f;
        if (rtl().set_freq_correction) {
            const int p = (int)std::lround(ppm);
            if (p != ppm_) {
                const int r = rtl().set_freq_correction(dev_, p);   // -2: already set
                if (r == 0 || r == -2) ppm_ = p;
                else { fprintf(stderr, "RTL-SDR: the frequency correction of %d ppm was refused (%d)\n", p, r); fflush(stderr); }
            }
        } else tuneHz = ppmCorrectedHz(f, ppm);
        // offset tuning (E4000 / FC00xx only, TuneSettings "offset")
        if (offsetCapable(tuner_) && !ds_) {
            const bool off = radioFlag(s, "offset");
            if (off != offset_ && rtl().set_offset_tuning(dev_, off ? 1 : 0) == 0) offset_ = off;
        }
        if (rtl().set_center_freq(dev_, (uint32_t)std::llround(std::min(tuneHz, 4.29e9))) < 0) {
            err = "RTL-SDR could not tune to " + mhz(f) + " MHz" + (hi_ > 0 ? " (range " + mhz(lo) + "-" + mhz(hi_) + " MHz)" : std::string());
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (rtl().set_bias_tee && s.biasTee != bias_) {
            if (rtl().set_bias_tee(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
            else if (s.biasTee) { fprintf(stderr, "RTL-SDR: the radio refused the bias-tee\n"); fflush(stderr); }
        }
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
    void closeDevice() override {
        if (dev_ && bias_ && rtl().set_bias_tee) rtl().set_bias_tee(dev_, 0);   // antenna power never stays on after OnAir lets go of the radio
        bias_ = false;
        if (dev_) rtl().close(dev_);
        dev_ = nullptr;
    }
    void streamLoop() override {
        rtl().reset_buffer(dev_);
#ifdef __APPLE__
        // On macOS the samples are read one block at a time. librtlsdr's read_async can return while a cancelled transfer is still in
        // flight and free it: libusb keeps it in its list of transfers, and the next control transfer (rtlsdr_close() setting the
        // demodulator back) walks into the freed one and crashes (issue #14, a Mac with an RTL-SDR crashing on Stop; the cancel there
        // completes later than on Linux or Windows). The radio streams without pause once reset_buffer() is done, so each read returns
        // within the 100 ms of samples it asks for, and stop() waits for at most that.
        if (rtl().read_sync) {
            const size_t bytes = (size_t)std::clamp(std::lround(rate_ * 2 * 0.1 / 16384) * 16384, 16384L, 262144L);
            std::vector<unsigned char> buf(bytes);
            while (run_) {
                int got = 0;
                if (rtl().read_sync(dev_, buf.data(), (int)bytes, &got) < 0) return;   // unplugged, or the stream died: reopened by the caller
                if (got > 0 && run_) convert(buf.data(), (uint32_t)got);
            }
            return;
        }
#endif
        // blocks until cancel_async(); 15 buffers of 256 kB (the library's defaults)
        rtl().read_async(dev_, &RtlSource::callback, this, 0, 0);
    }
    void interrupt() override { if (dev_) rtl().cancel_async(dev_); }   // (a blocking read_sync ends with its block)

private:
    // where the radio with serial_ is now: the listed position while it is still there, else the first with that serial (many dongles share
    // one, such as 00000001); -1 = unknown (no serial, or a radio whose strings cannot be read), -2 = every radio was read and it is not there
    int indexOfSerial() const {
        if (serial_.empty()) return -1;
        const uint32_t n = rtl().get_device_count();
        int first = -1;
        bool unread = false;
        for (uint32_t i = 0; i < n && i < 16; i++) {
            char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
            if (rtl().get_device_usb_strings(i, vendor, product, serial) != 0) { unread = true; continue; }
            if (trimmed(serial, sizeof serial) != serial_) continue;
            if (i == index_) return (int)i;
            if (first < 0) first = (int)i;
        }
        if (first >= 0) return first;
        return n > 0 && !unread ? -2 : -1;
    }
    static void DECT2_CALL callback(unsigned char* buf, uint32_t len, void* ctx) {
        auto* self = static_cast<RtlSource*>(ctx);
        if (!self->run_) { rtl().cancel_async(self->dev_); return; }
        self->convert(buf, len);
    }
    void convert(const unsigned char* buf, uint32_t len) {
        const size_t n = len / 2;
        conv_.resize(n);
        for (size_t i = 0; i < n; i++) conv_[i] = cf32((buf[2 * i] - 127.4f) / 128.f, (buf[2 * i + 1] - 127.4f) / 128.f);
        push(conv_.data(), n);
    }
    uint32_t index_;
    std::string serial_;
    RtlApi::rtlsdr_dev_t* dev_ = nullptr;
    int tuner_ = kTunerUnknown;
    RtlModel model_ = kGeneric;
    bool v4_ = false, bias_ = false, offset_ = false;
    int ds_ = 0;    // direct sampling in use: 0 off, 1 I, 2 Q
    int ppm_ = 0;   // the correction the library has
    double lo_ = 0, hi_ = 0;
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
        d.rateRanges = {{0.225001e6, 0.3e6}, {0.900001e6, 2.56e6}};   // librtlsdr refuses 0.3 to 0.9 Msps (and above 3.2); faster than 2.56 drops samples
        d.gainMinDb = 0; d.gainMaxDb = 49.6;
        int tuner = -1;
        const std::string key = d.nativeArgs + "/" + label + "/" + d.serial;
        {
            std::lock_guard<std::mutex> lk(gTunerMu);
            auto it = gTuner.find(key);
            if (it != gTuner.end()) tuner = it->second;
        }
        if (tuner < 0 && rtl().get_tuner_type) {
            RtlApi::rtlsdr_dev_t* dev = nullptr;
            if (rtl().open(&dev, i) == 0 && dev) {
                tuner = rtl().get_tuner_type(dev);
                rtl().close(dev);
                std::lock_guard<std::mutex> lk(gTunerMu);
                gTuner[key] = tuner;
            }
        }
        const RtlModel model = rtlModel(i);
        tunerRange(tuner, model, d.minFreqHz, d.maxFreqHz);
        d.hasBiasTee = rtl().set_bias_tee != nullptr;
        d.settings = rtlSettings(tuner, model);
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeRtl(const DeviceInfo& d) {
    return std::make_unique<RtlSource>((uint32_t)strtoul(d.nativeArgs.c_str(), nullptr, 10), d.serial);
}

} // namespace native
} // namespace dect2
