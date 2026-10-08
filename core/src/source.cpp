#include "dect2/mode_synth.h"
#include "dect2/source.h"
#include "dect2/gen_util.h"
#include "dect2/demo_ts.h"
#include "dect2/dvbt_gen.h"
#include "dect2/atsc_gen.h"

#include <libhackrf/hackrf.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
#include <mutex>
#include <random>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// hackrf_usb.cpp
std::unique_ptr<IqSource> makeUsbHackrfSource(const std::string& serial);
std::vector<DeviceInfo> listUsbHackrfDevices(std::string& err);
uint32_t hackrfAutoFilterHz(double sampleRate, double channelMhz);
void hackrfBoardRange(int id, double& lo, double& hi);

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- the radios' own settings (source.h)

RadioSetting ppmSetting(double step) {
    RadioSetting r;
    r.type = RadioSetting::Number;
    r.key = "ppm";
    r.label = "Frequency correction";
    r.help = "The radio's clock error in ppm (parts per million): positive when its clock runs fast, so that every signal shows up a little below\n"
             "its real frequency. A dongle without a TCXO is often 20-100 ppm off, a HackRF One up to 20 ppm, a radio with a TCXO about 1 ppm.\n"
             "Find it with a signal of known frequency (a broadcast carrier, a time signal, a GSM/LTE base station). 0 = no correction.";
    r.minV = -200; r.maxV = 200; r.step = step > 0 ? step : 0.1; r.unit = "ppm";
    r.def = "0";
    return r;
}

RadioSetting boolSetting(const std::string& key, const std::string& label, const std::string& help, bool def, bool restart) {
    RadioSetting r;
    r.type = RadioSetting::Bool;
    r.key = key; r.label = label; r.help = help;
    r.def = def ? "1" : "0";
    r.restart = restart;
    return r;
}

RadioSetting choiceSetting(const std::string& key, const std::string& label, const std::string& help, std::vector<std::string> values,
                           std::vector<std::string> names, const std::string& def, bool restart) {
    RadioSetting r;
    r.type = RadioSetting::Choice;
    r.key = key; r.label = label; r.help = help;
    r.values = std::move(values); r.names = std::move(names);
    if (r.names.size() != r.values.size()) r.names = r.values;
    r.def = def;
    r.restart = restart;
    return r;
}

// ---------------------------------------------------------------- HackRF

namespace {

std::mutex g_hackrfInitMutex;
int g_hackrfRefs = 0;

bool hackrfAcquire(std::string& err) {
    std::lock_guard<std::mutex> lk(g_hackrfInitMutex);
    if (g_hackrfRefs == 0) {
        int r = hackrf_init();
        if (r != HACKRF_SUCCESS) {
            err = std::string("hackrf_init: ") + hackrf_error_name((hackrf_error)r);
            return false;
        }
    }
    g_hackrfRefs++;
    return true;
}

void hackrfRelease() {
    std::lock_guard<std::mutex> lk(g_hackrfInitMutex);
    if (--g_hackrfRefs == 0) hackrf_exit();
}

class HackrfSource : public IqSource {
public:
    explicit HackrfSource(std::string serial) : serial_(std::move(serial)) {}
    ~HackrfSource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        if (!hackrfAcquire(err)) return false;
        ring_ = &ring;
        int r = serial_.empty() ? hackrf_open(&dev_) : hackrf_open_by_serial(serial_.c_str(), &dev_);
        if (r != HACKRF_SUCCESS) {
            err = std::string("open: ") + hackrf_error_name((hackrf_error)r);
            hackrfRelease();
            return false;
        }
        bias_ = false;
        {   // only the One (board id 2, or 4 from hardware r9 on) and the Pro (5) have antenna power
            uint8_t id = 0xFF;
            hasBias_ = hackrf_board_id_read(dev_, &id) == HACKRF_SUCCESS && (id == 2 || id == 4 || id == 5);
        }
        if (!apply(s, err)) {
            hackrf_close(dev_);
            dev_ = nullptr;
            hackrfRelease();
            return false;
        }
        rate_ = s.sampleRate;
        r = hackrf_start_rx(dev_, &HackrfSource::rxCb, this);
        if (r != HACKRF_SUCCESS) {
            err = std::string("start_rx: ") + hackrf_error_name((hackrf_error)r);
            if (bias_) hackrf_set_antenna_enable(dev_, 0);   // apply() may have switched the antenna power on
            bias_ = false;
            hackrf_close(dev_);
            dev_ = nullptr;
            hackrfRelease();
            return false;
        }
        return true;
    }

    void stop() override {
        if (!dev_) return;
        hackrf_stop_rx(dev_);
        if (bias_) hackrf_set_antenna_enable(dev_, 0);   // antenna power never stays on
        bias_ = false;
        hackrf_close(dev_);
        dev_ = nullptr;
        hackrfRelease();
    }

    bool retune(const TuneSettings& s, std::string& err) override { return dev_ ? apply(s, err, true) : false; }
    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

private:
    bool apply(const TuneSettings& s, std::string& err, bool live = false) {
        auto chk = [&](int r, const char* what) {
            if (r == HACKRF_SUCCESS) return true;
            err = std::string(what) + ": " + hackrf_error_name((hackrf_error)r);
            return false;
        };
        if (!live || s.sampleRate != rate_) {
            if (!chk(hackrf_set_sample_rate(dev_, s.sampleRate), "set_sample_rate")) return false;
            rate_ = s.sampleRate;
        }
        uint32_t bw = s.basebandFilterHz > 0 ? (uint32_t)s.basebandFilterHz : hackrfAutoFilterHz(s.sampleRate, s.bandwidthMhz);   // as hackrf_usb.cpp
        if (!chk(hackrf_set_baseband_filter_bandwidth(dev_, bw), "set_baseband_filter")) return false;
        // the HackRF's library has no frequency correction: the clock error is taken out of the frequency asked for
        if (!chk(hackrf_set_freq(dev_, (uint64_t)ppmCorrectedHz(s.centerHz, radioPpm(s))), "set_freq")) return false;
        if (!chk(hackrf_set_lna_gain(dev_, (uint32_t)s.lnaDb), "set_lna_gain")) return false;
        if (!chk(hackrf_set_vga_gain(dev_, (uint32_t)s.vgaDb), "set_vga_gain")) return false;
        if (!chk(hackrf_set_amp_enable(dev_, s.ampOn ? 1 : 0), "set_amp_enable")) return false;
        if (hasBias_ && (!live || s.biasTee != bias_)) {
            if (!chk(hackrf_set_antenna_enable(dev_, s.biasTee ? 1 : 0), "set_antenna_enable")) return false;
            bias_ = s.biasTee;
        }
        return true;
    }

    static int rxCb(hackrf_transfer* t) {
        auto* self = static_cast<HackrfSource*>(t->rx_ctx);
        const int8_t* p = reinterpret_cast<const int8_t*>(t->buffer);
        size_t n = (size_t)t->valid_length / 2;
        constexpr size_t kChunk = 4096;
        cf32 tmp[kChunk];
        size_t i = 0;
        while (i < n) {
            size_t m = std::min(kChunk, n - i);
            for (size_t k = 0; k < m; k++) tmp[k] = cf32(p[2 * (i + k)] * (1.0f / 128), p[2 * (i + k) + 1] * (1.0f / 128));
            self->ring_->write(tmp, m);
            i += m;
        }
        return 0;
    }

    std::string serial_;
    hackrf_device* dev_ = nullptr;
    IqRing* ring_ = nullptr;
    double rate_ = 0;
    bool hasBias_ = false, bias_ = false;
};

// Our own USB driver first (hackrf_usb.cpp); libhackrf when it cannot open the radio (or always, with DECT2_LIBHACKRF=1 in the environment).
class HackrfAuto : public IqSource {
public:
    explicit HackrfAuto(std::string serial) : serial_(std::move(serial)) {}
    ~HackrfAuto() override { stop(); }
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        stop();
        std::string ownErr;
        if (!getenv("DECT2_LIBHACKRF")) {
            auto u = makeUsbHackrfSource(serial_);
            if (u->start(s, ring, ownErr)) { impl_ = std::move(u); if (getenv("DECT2_DEBUG")) fprintf(stderr, "HackRF: own USB driver\n"); return true; }
            if (getenv("DECT2_DEBUG")) fprintf(stderr, "HackRF: own USB driver failed (%s), trying libhackrf\n", ownErr.c_str());
        }
        auto l = std::make_unique<HackrfSource>(serial_);
        std::string libErr;
        if (l->start(s, ring, libErr)) { impl_ = std::move(l); if (getenv("DECT2_DEBUG")) fprintf(stderr, "HackRF: libhackrf\n"); return true; }
        err = ownErr.empty() ? libErr : ownErr;   // the message of the driver that was tried first
        return false;
    }
    void stop() override { if (impl_) { impl_->stop(); impl_.reset(); } }
    bool retune(const TuneSettings& s, std::string& err) override { return impl_ ? impl_->retune(s, err) : false; }
    double sampleRate() const override { return impl_ ? impl_->sampleRate() : 0; }
    bool realtimeHardware() const override { return true; }
private:
    std::string serial_;
    std::unique_ptr<IqSource> impl_;
};

// ---------------------------------------------------------------- paced thread base

class PacedSource : public IqSource {
public:
    ~PacedSource() override { stop(); }
    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        ring_ = &ring;
        rate_ = effectiveRate(s);
        if (!prepare(s, err)) return false;
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() override {
        run_ = false;
        if (th_.joinable()) th_.join();
    }
    bool retune(const TuneSettings&, std::string&) override { return true; }
    double sampleRate() const override { return rate_; }

protected:
    virtual double effectiveRate(const TuneSettings& s) { return s.sampleRate; }
    virtual bool prepare(const TuneSettings&, std::string&) { return true; }
    virtual size_t produce(cf32* dst, size_t maxN) = 0; // return samples produced; 0 = end of data
    virtual double pace() { return 1.0; }               // speed relative to the nominal sample rate

    void loop() {
        std::vector<cf32> buf(1 << 15);
        auto t0 = Clock::now();
        uint64_t sent = 0;
        while (run_) {
            size_t n = produce(buf.data(), buf.size());
            if (n == 0) break;
            // Recordings and the synthetic signal are not live: when the receiver cannot keep up (a slow computer) the source waits for it
            // instead of throwing samples away, so playback just runs slower than real time. (Live radios have to drop.)
            while (run_ && ring_->capacity() - ring_->available() < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!run_) break;
            ring_->write(buf.data(), n);
            sent += n;
            if (getenv("DECT2_NOPACE")) {   // benchmarks: play as fast as the receiver takes the samples, never overrun the ring
                while (run_ && ring_->available() > (1u << 21)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            auto due = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sent / (rate_ * pace())));
            std::this_thread::sleep_until(due);
        }
    }

    IqRing* ring_ = nullptr;
    double rate_ = 0;
    std::atomic<bool> run_{false};
    std::thread th_;
};

// ---------------------------------------------------------------- file

class FileSource : public PacedSource {
public:
    FileSource(std::string path, FileFormat fmt, double rate, bool loop)
        : path_(std::move(path)), fmt_(fmt), fileRate_(rate), loop_(loop) {}
    ~FileSource() override { stop(); if (f_) fclose(f_); }

protected:
    double effectiveRate(const TuneSettings&) override { return fileRate_; }
    bool prepare(const TuneSettings&, std::string& err) override {
#ifdef _WIN32
        {   // the path is UTF-8 (file dialog): fopen would read it in the ANSI code page and fail on names outside it
            const int n = MultiByteToWideChar(CP_UTF8, 0, path_.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path_.c_str(), -1, &w[0], n);
            f_ = n > 0 ? _wfopen(w.c_str(), L"rb") : nullptr;
        }
#else
        f_ = fopen(path_.c_str(), "rb");
#endif
        if (!f_) { err = "cannot open " + path_ + ": " + strerror(errno); return false; }
        return true;
    }
    size_t produce(cf32* dst, size_t maxN) override {
        size_t bps = fmt_ == FileFormat::CF32 ? 8 : 2;
        raw_.resize(maxN * bps);
        size_t got = fread(raw_.data(), bps, maxN, f_);
        if (got == 0 && loop_) { rewind(f_); got = fread(raw_.data(), bps, maxN, f_); }
        for (size_t i = 0; i < got; i++) {
            switch (fmt_) {
            case FileFormat::CS8: dst[i] = cf32((int8_t)raw_[2*i] / 128.0f, (int8_t)raw_[2*i+1] / 128.0f); break;
            case FileFormat::CU8: dst[i] = cf32((raw_[2*i] - 127.5f) / 127.5f, (raw_[2*i+1] - 127.5f) / 127.5f); break;
            case FileFormat::CF32: { float v[2]; memcpy(v, &raw_[8*i], 8); dst[i] = cf32(v[0], v[1]); } break;
            }
        }
        return got;
    }

private:
    std::string path_;
    FileFormat fmt_;
    double fileRate_;
    bool loop_;
    FILE* f_ = nullptr;
    std::vector<uint8_t> raw_;
};

// ---------------------------------------------------------------- synthetic DVB-T2-like signal (and the test signals of the other modes, see mode_synth.h)

class SyntheticSource : public PacedSource {
public:
    ~SyntheticSource() override { stop(); }

    bool retune(const TuneSettings& s, std::string&) override {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = s.synth;
        gainDb_ = s.lnaDb + s.vgaDb + (s.ampOn ? 14 : 0);
        regen_ = true;
        return true;
    }

protected:
    double pace() override { std::lock_guard<std::mutex> lk(mu_); return cfg_.pace > 0.01 ? cfg_.pace : 1.0; }
    double effectiveRate(const TuneSettings& s) override { if (s.synth.mode >= 4) return s.sampleRate > 0 ? s.sampleRate : 2e6; return s.synth.atsc ? (s.sampleRate > 0 && s.sampleRate < 12e6 ? s.sampleRate : 8e6) : nativeRateHz(s.bandwidthMhz); }
    bool prepare(const TuneSettings& s, std::string&) override {
        cfg_ = s.synth;
        gainDb_ = s.lnaDb + s.vgaDb + (s.ampOn ? 14 : 0);
        regen_ = true;
        return true;
    }

    size_t produceMode(cf32* dst, size_t maxN) {   // the test signal of a mode added after FM
        if (regen_ || !ms_ || msMode_ != cfg_.mode) {
            ms_ = makeModeSynth(cfg_.mode, cfg_, rate_);
            msMode_ = cfg_.mode;
            regen_ = false;
            if (!ms_) log_("no built-in test signal for this mode yet: the source sends silence");
        }
        if (!ms_) { std::fill(dst, dst + maxN, cf32(0.f, 0.f)); return maxN; }
        ms_->generate(dst, maxN);
        const float g = cfg_.gainModel ? (float)std::pow(10.0, (gainDb_ - 62) / 20.0) : 1.f;
        auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
        for (size_t i = 0; i < maxN; i++) dst[i] = cf32(q(dst[i].real() * g), q(dst[i].imag() * g));
        return maxN;
    }
    static void log_(const char* m) { if (getenv("DECT2_DEBUG")) fprintf(stderr, "%s\n", m); }

    size_t produce(cf32* dst, size_t maxN) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (cfg_.mode >= 4) return produceMode(dst, maxN);
        if (cfg_.atsc) {
            if (regen_ || !agen_) {
                atsc::ChannelConfig cc;
                cc.snrDb = cfg_.snrDb; cc.cfoHz = cfg_.cfoHz; cc.sroPpm = cfg_.sroPpm;
                if (cfg_.echoDb > 0) cc.echoes.push_back({cfg_.echoDelay / rate_ * 1e6, -cfg_.echoDb, 0});
                agen_ = std::make_unique<atsc::Generator>(cfg_.demoTv ? demoTsSource(19.39e6) : dvbt::testTsSource(), cc, rate_);
                regen_ = false;
            }
            atmp_.clear();
            agen_->generate(maxN, atmp_);
            const float g = 0.22f / 6.7f * (cfg_.gainModel ? (float)std::pow(10.0, (gainDb_ - 62) / 20.0) : 1.f);   // the 8-VSB signal has an rms of about 6.7 units
            auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
            for (size_t i = 0; i < maxN; i++) dst[i] = cf32(q(atmp_[i].real() * g), q(atmp_[i].imag() * g));
            return maxN;
        }
        if (regen_) {
            if (cfg_.dvbt) {
                dvbt::Params tp;
                tp.mode = cfg_.dvbtMode; tp.guard = cfg_.dvbtGuard; tp.mod = cfg_.dvbtMod; tp.crHp = tp.crLp = cfg_.dvbtRate;
                tgen_ = std::make_unique<dvbt::Generator>(tp, cfg_.demoTv ? demoTsSource(dvbt::netBitrate(tp, 64e6 / 7)) : dvbt::testTsSource());
                gen_.reset();
            } else { gen_ = std::make_unique<T2Generator>(cfg_.tx); tgen_.reset(); }
            sbuf_.assign(4, cf32(0, 0));
            pos_ = 1;
            regen_ = false;
            delay_.assign(8192, cf32(0, 0));
            dpos_ = 0;
        }
        const double step = 1.0 + cfg_.sroPpm * 1e-6;
        const double nsig = std::pow(10.0, -cfg_.snrDb / 20.0) / std::sqrt(2.0);
        const float ea = cfg_.echoDb > 0 ? (float)std::pow(10.0, -cfg_.echoDb / 20.0) : 0.f;
        const int ed = std::min<int>(std::max(1, cfg_.echoDelay), (int)delay_.size() - 1);
        const double dph = 2 * M_PI * cfg_.cfoHz / rate_;
        // noise from a table and the carrier offset as an oscillator recurrence: std::normal_distribution and sin/cos per sample cost more than the generator
        nz_.assign(maxN, cf32(0, 0));
        noise_.add(nz_.data(), maxN, (float)nsig);
        cf32 osc = cf32((float)std::cos(phase_), (float)std::sin(phase_));
        const cf32 oscStep = cf32((float)std::cos(dph), (float)std::sin(dph));
        const float gain = 0.25f * (cfg_.gainModel ? (float)std::pow(10.0, (gainDb_ - 62) / 20.0) : 1.f); // leave ADC headroom: OFDM peak/rms is ~12 dB
        for (size_t i = 0; i < maxN; i++) {
            size_t i0 = (size_t)pos_;
            while (sbuf_.size() < i0 + 4) {
                if (tgen_) tgen_->nextSymbol(frame_); else gen_->nextFrame(frame_);
                sbuf_.insert(sbuf_.end(), frame_.begin(), frame_.end());
            }
            cf32 v;
            if (cfg_.sroPpm == 0) v = sbuf_[i0];
            else { // 4-point Lagrange interpolation
                float f = (float)(pos_ - i0);
                cf32 a = sbuf_[i0 - 1], b = sbuf_[i0], c = sbuf_[i0 + 1], d = sbuf_[i0 + 2];
                float w0 = -f * (f - 1) * (f - 2) / 6, w1 = (f + 1) * (f - 1) * (f - 2) / 2,
                      w2 = -(f + 1) * f * (f - 2) / 2, w3 = (f + 1) * f * (f - 1) / 6;
                v = a * w0 + b * w1 + c * w2 + d * w3;
            }
            pos_ += step;
            delay_[dpos_ & 8191] = v;
            if (ea > 0) v += ea * delay_[(dpos_ - ed) & 8191];
            dpos_++;
            phase_ += dph;
            if (phase_ > M_PI) phase_ -= 2 * M_PI;
            if (phase_ < -M_PI) phase_ += 2 * M_PI;
            if ((i & 255) == 255) osc = cf32((float)std::cos(phase_), (float)std::sin(phase_));   // back on the exact phase
            else osc *= oscStep;
            v *= osc;
            v += nz_[i];
            v *= gain;
            // 8-bit ADC, like the HackRF
            auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
            dst[i] = cf32(q(v.real()), q(v.imag()));
        }
        size_t drop = (size_t)pos_ - 2;
        if (drop > 0 && drop < sbuf_.size()) {
            sbuf_.erase(sbuf_.begin(), sbuf_.begin() + drop);
            pos_ -= drop;
        }
        return maxN;
    }

private:
    std::unique_ptr<atsc::Generator> agen_;
    std::unique_ptr<ModeSynth> ms_;
    int msMode_ = 0;
    std::vector<cf32> atmp_;
    std::mutex mu_;
    SynthConfig cfg_;
    bool regen_ = true;
    std::unique_ptr<T2Generator> gen_;
    std::unique_ptr<dvbt::Generator> tgen_;
    std::vector<cf32> sbuf_, frame_, delay_;
    double pos_ = 1, phase_ = 0;
    size_t dpos_ = 0;
    genutil::NoiseSource noise_{99};
    std::vector<cf32> nz_;
    int gainDb_ = 62;
};

} // namespace

// ---------------------------------------------------------------- public API

std::vector<DeviceInfo> listHackrfDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    if (!getenv("DECT2_LIBHACKRF")) {
        out = listUsbHackrfDevices(err);
        if (!out.empty()) return out;
    }
    if (!hackrfAcquire(err)) return out;
    hackrf_device_list_t* l = hackrf_device_list();
    if (l) {
        for (int i = 0; i < l->devicecount; i++) {
            DeviceInfo d;
            d.kind = DeviceInfo::HackRF;
            d.serial = l->serial_numbers[i] ? l->serial_numbers[i] : "";
            d.board = hackrf_usb_board_id_name(l->usb_board_ids[i]);
            // the USB product id does not distinguish a HackRF Pro from a One: ask the board itself
            {
                hackrf_device* dev = nullptr;
                if (hackrf_open_by_serial(d.serial.c_str(), &dev) == HACKRF_SUCCESS) {
                    uint8_t id = 0xFF;   // undetected (older libhackrf headers have no name for it)
                    if (hackrf_board_id_read(dev, &id) == HACKRF_SUCCESS) {
                        d.board = hackrf_board_id_name((hackrf_board_id)id); d.hasBiasTee = id == 2 || id == 4 || id == 5;
                        hackrfBoardRange(id, d.minFreqHz, d.maxFreqHz);
                    }
                    hackrf_close(dev);
                }
            }
            std::string tail = d.serial.size() > 8 ? d.serial.substr(d.serial.size() - 8) : d.serial;
            d.name = d.board + " (" + tail + ")";
            d.settings = {ppmSetting(0.1)};
            out.push_back(d);
        }
        hackrf_device_list_free(l);
    }
    hackrfRelease();
    return out;
}

#ifdef DECT2_HAVE_SOAPY
std::unique_ptr<IqSource> makeSoapySource(const DeviceInfo& d);
#else
bool soapySupported() { return false; }
std::vector<DeviceInfo> listSoapyDevices(std::string&) { return {}; }
#endif

std::vector<DeviceInfo> listRadios(std::string& err) {
    std::vector<DeviceInfo> out = listHackrfDevices(err);
    std::string e2;
    const std::vector<DeviceInfo> native = listNativeDevices(e2);
    for (auto& d : native) out.push_back(d);
    if (err.empty()) err = e2;
    // the SoapySDR driver names of the radios that are also available natively (rtlsdr, airspy, bladerf and sdrplay keep their names;
    // "sdrplay" is the SoapySDRPlay3 module that Linux systems may have next to the SDRplay API)
    auto soapyName = [](const std::string& b) { return b == "pluto" ? std::string("plutosdr") : b == "usrp" ? std::string("uhd") : b == "lime" ? std::string("lime") : b; };
    for (auto& d : listSoapyDevices(e2)) {
        bool dup = false;
        for (auto& n : native) if (soapyName(n.board) == d.board) dup = true;
        if (!dup) out.push_back(d);
    }
    if (err.empty()) err = e2;
    return out;
}

std::unique_ptr<IqSource> makeSource(const DeviceInfo& d) {
    switch (d.kind) {
    case DeviceInfo::HackRF: return std::make_unique<HackrfAuto>(d.serial);
#ifdef DECT2_HAVE_SOAPY
    case DeviceInfo::Soapy: return makeSoapySource(d);
#endif
    case DeviceInfo::Native: return makeNativeSource(d);
    case DeviceInfo::Synthetic: return std::make_unique<SyntheticSource>();
    default: return nullptr;
    }
}

FileFormat guessFormat(const std::string& path) {
    auto ends = [&](const char* e) { size_t n = strlen(e); return path.size() >= n && path.compare(path.size() - n, n, e) == 0; };
    if (ends(".cu8") || ends(".u8")) return FileFormat::CU8;
    if (ends(".cf32") || ends(".fc32") || ends(".cfile")) return FileFormat::CF32;
    return FileFormat::CS8;
}

double guessSampleRate(const std::string& path) {
    std::string n = path;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    size_t p = n.find("msps");
    if (p == std::string::npos || p == 0) return 0;
    size_t b = p;
    while (b > 0 && (isdigit((unsigned char)n[b - 1]) || n[b - 1] == '.')) b--;
    if (b == p) return 0;
    const double v = atof(n.substr(b, p - b).c_str());
    return v >= 0.5 && v <= 100 ? v * 1e6 : 0;
}

std::unique_ptr<IqSource> makeFileSource(const std::string& path, FileFormat fmt, double sampleRate, bool loop) {
    return std::make_unique<FileSource>(path, fmt, sampleRate, loop);
}

} // namespace dect2
