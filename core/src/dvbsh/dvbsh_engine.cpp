// DVB-SH in the engine: see dvbsh_engine.h.
#include "dect2/dvbsh/dvbsh_engine.h"
#include "dect2/dvbsh/dvbsh_gen.h"
#include "dect2/dvbsh/dvbsh_rx.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include "dect2/tv/demo_ts.h"
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>

namespace dect2 {

namespace {
const double kWidths[] = {5, 8, 7, 6, 1.7};
constexpr double kTwoPi = 6.283185307179586;
}

struct DvbshEngineReceiver::Impl {
    std::mutex mu;
    DvbshTelemetry tel;
    double inRate = 0;
    double fixedBw = 0;      // 0 automatic
    int bwIdx = 0;
    double bw = 5;
    bool lp = false;
    double offsetHz = 0, ph = 0;
    ExactResampler rs;
    dvbsh::Receiver rx;
    std::vector<cf32> mixed, res;
    std::function<void(const uint8_t*, size_t, double)> cb;
    double pendingSecs = 0, sinceLock = 0, searchSecs = 0;
    uint64_t seq = 0;

    void setup() {
        bw = fixedBw > 0 ? fixedBw : kWidths[bwIdx];
        const double fs = dvbsh::sampleRate(bw);
        rs = ExactResampler();
        if (inRate > 0) rs.configure(inRate, fs);
        rx.configure(fs);
        rx.setLowPriority(lp);
        rx.setPacketCallback([this](const uint8_t* pk, size_t n) { if (cb) cb(pk, n, pendingSecs); pendingSecs = 0; });
        searchSecs = 0;
    }
    bool usable(double w) const { return inRate <= 0 || dvbsh::sampleRate(w) <= inRate * 1.02; }
    void nextWidth() {
        for (int i = 1; i <= 5; i++) {
            const int c = (bwIdx + i) % 5;
            if (usable(kWidths[c])) { bwIdx = c; break; }
        }
        setup();
    }
};

DvbshEngineReceiver::DvbshEngineReceiver() : p_(std::make_unique<Impl>()) { p_->setup(); }
DvbshEngineReceiver::~DvbshEngineReceiver() = default;

void DvbshEngineReceiver::configure(double r) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->inRate = r;
    p_->bwIdx = 0;
    if (!p_->usable(kWidths[0])) p_->nextWidth(); else p_->setup();
    p_->tel = DvbshTelemetry{};
    p_->tel.inputRate = r;
}
bool DvbshEngineReceiver::ready() const { return p_->inRate <= 0 || p_->inRate >= dvbsh::sampleRate(1.7) * 0.98; }
void DvbshEngineReceiver::reset() { std::lock_guard<std::mutex> l(p_->mu); p_->setup(); }
void DvbshEngineReceiver::setBandwidth(double mhz) { std::lock_guard<std::mutex> l(p_->mu); p_->fixedBw = mhz; p_->setup(); }
void DvbshEngineReceiver::setLowPriority(bool lp) { std::lock_guard<std::mutex> l(p_->mu); p_->lp = lp; p_->rx.setLowPriority(lp); }
void DvbshEngineReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; }
void DvbshEngineReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->cb = std::move(cb); }

void DvbshEngineReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& m = *p_;
    if (m.inRate <= 0 || !ready()) return;
    const cf32* in = x;
    if (m.offsetHz != 0) {
        m.mixed.resize(n);
        const double d = -kTwoPi * m.offsetHz / m.inRate;
        for (size_t i = 0; i < n; i++) { m.mixed[i] = x[i] * cf32((float)std::cos(m.ph), (float)std::sin(m.ph)); m.ph += d; }
        m.ph = std::remainder(m.ph, kTwoPi);
        in = m.mixed.data();
    }
    m.res.clear();
    m.rs.process(in, n, m.res);
    const double secs = (double)n / m.inRate;
    m.pendingSecs += secs;
    m.rx.feed(m.res.data(), m.res.size());
    const dvbsh::RxStatus s = m.rx.status();
    // the automatic channel width: no OFDM symbols at this width for a while, try the next
    if (s.level == 0) {
        m.searchSecs += secs;
        if (m.fixedBw <= 0 && m.searchSecs > 0.3) m.nextWidth();
    } else m.searchSecs = 0;
    DvbshTelemetry& t = m.tel;
    t.seq = ++m.seq;
    t.state = s.level + 1;
    t.cfoHz = s.cfoHz;
    t.snrDb = (float)s.snrDb;
    t.locked = s.level >= 2;
    t.dataValid = s.level >= 3;
    t.blocksOk = (uint64_t)s.codewordsOk; t.blocksBad = (uint64_t)s.codewordsBad;
    t.packetsOk = (uint64_t)s.packetsOk; t.packetsBad = (uint64_t)s.packetsBad; t.tpsFrames = (uint64_t)s.tpsFrames;
    t.bwMhz = m.bw;
    t.params = s.p;
    t.tiLmax = s.tiLmax;
    const double fs = dvbsh::sampleRate(m.bw);
    if (s.level >= 2) {
        const double symSec = (dvbsh::fftN(s.p.mode) + dvbsh::guardSamples(s.p.mode, s.p.guard)) / fs;
        const double iuSec = symSec / (dvbsh::streamBitsPerSymbol(s.p) / (double)dvbsh::kIuBits);
        t.tiSeconds = s.tiLmax * 48.0 * iuSec;
    }
    t.tiTooLarge = s.tiTooLarge;
    t.avgIterations = s.avgIterations;
    t.inputRate = m.inRate;
    t.lowPriority = m.lp;
}

bool DvbshEngineReceiver::telemetry(DvbshTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}

ModeTuning dvbshTuning() {
    ModeTuning t;
    t.stdMode = 748;   // family 746
    t.id = "dvbsh";
    t.name = "DVB-SH";
    t.minMhz = 400; t.maxMhz = 3000; t.defMhz = 2177.5;   // S band 2170 - 2200 MHz (5 MHz channels); also UHF and L band
    t.sampleRate = 8e6;
    t.bandwidthMhz = 5;
    t.minSampleRate = dvbsh::sampleRate(1.7);
    return t;
}

std::string dvbshSummary(const DvbshTelemetry& t) {
    char b[240];
    if (t.state <= 1) return "DVB-SH: no input";
    if (t.state == 2) { snprintf(b, sizeof b, "DVB-SH: searching (%.1f MHz channel)", t.bwMhz); return b; }
    if (t.state == 3 && !t.locked) return "DVB-SH: OFDM symbols found, waiting for the TPS";
    const auto& p = t.params;
    snprintf(b, sizeof b, "DVB-SH %s %s%s GI 1/%d rate %s, interleaver %.2f s%s, codewords %llu ok %llu bad, SNR %.1f dB", dvbsh::modeName(p.mode),
             p.mod ? "16-QAM" : "QPSK", p.hier ? " hierarchical" : "", 32 >> p.guard, dvbsh::rateName(t.lowPriority && p.hier ? p.patLp : p.patHp), t.tiSeconds,
             t.tiTooLarge ? " (too long for this receiver)" : "", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.snrDb);
    return b;
}

// ---------------------------------------------------------------------------- test signal
namespace {
class DvbshSynth : public ModeSynth {
public:
    DvbshSynth(const SynthConfig& c, double rate) : rate_(rate), noise_(7) {
        static const double widths[5] = {5, 8, 7, 6, 1.7};
        dvbsh::Params p;
        p.bwMhz = widths[c.modeOpt[0] >= 0 && c.modeOpt[0] < 5 ? c.modeOpt[0] : 0];
        p.mode = c.modeOpt[1] >= 0 && c.modeOpt[1] < 4 ? c.modeOpt[1] : dvbsh::k2K;
        if (p.bwMhz < 3) p.mode = dvbsh::k1K;   // the 1.7 MHz channel is defined for the 1K mode only (table 5.25)
        p.mod = c.modeOpt[2] == 1 ? dvbsh::k16Qam : dvbsh::kQpsk;
        p.patHp = c.modeOpt[3] >= 1 && c.modeOpt[3] <= dvbsh::kNumPatterns ? c.modeOpt[3] - 1 : 4;
        p.guard = c.modeOpt[4] >= 1 && c.modeOpt[4] <= 4 ? c.modeOpt[4] - 1 : 2;
        p.ti = dvbsh::TiConfig{4, 48, 1, 0, 0};
        const double fs = dvbsh::sampleRate(p.bwMhz);
        gen_ = std::make_unique<dvbsh::Generator>(p, demoTsSource(dvbsh::netBitrate(p, fs) * 0.95));
        rs_.configure(fs, rate * (1.0 + c.sroPpm * 1e-6));
        sigma_ = (float)(0.22 * std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0));
        dph_ = kTwoPi * c.cfoHz / rate;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> g(0.f, 1.f);
        while (q_.size() - head_ < n) {
            gen_->nextSymbol(sym_);
            if (head_ > 0) { q_.erase(q_.begin(), q_.begin() + (long)head_); head_ = 0; }
            rs_.process(sym_.data(), sym_.size(), q_);
        }
        for (size_t i = 0; i < n; i++) {
            out[i] = q_[head_ + i] * 0.22f * cf32((float)std::cos(ph_), (float)std::sin(ph_)) + cf32(sigma_ * g(noise_), sigma_ * g(noise_));
            ph_ = std::remainder(ph_ + dph_, kTwoPi);
        }
        head_ += n;
    }
private:
    double rate_;
    std::unique_ptr<dvbsh::Generator> gen_;
    ExactResampler rs_;
    std::vector<cf32> sym_, q_;
    size_t head_ = 0;
    float sigma_ = 0;
    double ph_ = 0, dph_ = 0;
    std::mt19937 noise_;
};
}

std::unique_ptr<ModeSynth> makeDvbshSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < dvbsh::sampleRate(1.7) * 0.98) return nullptr;
    return std::make_unique<DvbshSynth>(cfg, sampleRate);
}

} // namespace dect2
