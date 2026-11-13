// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
    dvbsh::TdmReceiver tdm;
    int waveform = 0;        // 0 automatic, 1 OFDM, 2 TDM
    bool useTdm = false;     // automatic: which one is listening now
    int goldN = 0;
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
        if (waveform != 0) useTdm = waveform == 2;
        if (inRate > 0) tdm.configure(inRate, bw);
        tdm.setGoldCode(goldN);
        tdm.setPacketCallback([this](const uint8_t* pk, size_t n) { if (cb) cb(pk, n, pendingSecs); pendingSecs = 0; });
        searchSecs = 0;
    }
    // automatic: every width as OFDM first (0.3 s each, as before), then every width as TDM (1.3 s each: its own search runs over the 12
    // symbol rates of table 5.13), then OFDM again
    int phaseTries = 0;
    int usableWidths() const { int n = 0; for (double w : kWidths) n += usable(w); return std::max(1, n); }
    void nextTry() {
        searchSecs = 0;
        if (waveform != 0) { if (fixedBw <= 0) nextWidth(); else setup(); return; }
        if (++phaseTries >= (fixedBw > 0 ? 1 : usableWidths())) { phaseTries = 0; useTdm = !useTdm; }
        if (fixedBw <= 0) nextWidth(); else setup();
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
void DvbshEngineReceiver::setWaveform(int w) { std::lock_guard<std::mutex> l(p_->mu); if (w != p_->waveform) { p_->waveform = w < 0 || w > 2 ? 0 : w; p_->setup(); } }
void DvbshEngineReceiver::setTdmGoldCode(int n) { std::lock_guard<std::mutex> l(p_->mu); if (n != p_->goldN) { p_->goldN = std::max(0, n); p_->tdm.setGoldCode(p_->goldN); } }
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
    const double secs = (double)n / m.inRate;
    m.pendingSecs += secs;
    DvbshTelemetry& t = m.tel;
    if (m.useTdm) {
        // SH-B: the TDM receiver works at the input rate (its front end resamples to two samples per symbol itself)
        m.tdm.feed(in, n);
        const dvbsh::TdmStatus s = m.tdm.status();
        if (s.level == 0) {
            m.searchSecs += secs;
            if (m.searchSecs > 1.3 && (m.waveform == 0 || m.fixedBw <= 0)) m.nextTry();
        } else m.searchSecs = 0;
        t.seq = ++m.seq;
        t.tdm = true;
        t.state = s.level == 0 ? 1 : s.level == 1 ? 2 : s.level + 1 > 4 ? 4 : s.level + 1;   // 2 slots found, 3 frames / signalling, 4 transport stream
        t.cfoHz = s.cfoHz; t.snrDb = (float)s.snrDb;
        t.locked = s.level >= 3; t.dataValid = s.level >= 4;
        t.blocksOk = (uint64_t)s.codewordsOk; t.blocksBad = (uint64_t)s.codewordsBad;
        t.packetsOk = (uint64_t)s.packetsOk; t.packetsBad = (uint64_t)s.packetsBad; t.tpsFrames = 0;
        t.bwMhz = m.bw;
        t.tdmMod = s.mod; t.symbolRate = s.symbolRate; t.clockPpm = s.clockPpm; t.sf = s.sf; t.tdmFrames = (uint64_t)s.frames;
        t.tiLmax = s.tiLmax;
        if (s.level >= 3 && s.symbolRate > 0 && s.mod >= 0) t.tiSeconds = s.tiLmax * 48.0 * dvbsh::kIuBits / (s.symbolRate * dvbsh::tdmBitsPerSymbol(s.mod) * dvbsh::kSlotData / dvbsh::kSlotSymbols);
        t.tiTooLarge = s.tiTooLarge;
        t.avgIterations = s.avgIterations;
        t.inputRate = m.inRate;
        t.lowPriority = false;
        return;
    }
    m.res.clear();
    m.rs.process(in, n, m.res);
    m.rx.feed(m.res.data(), m.res.size());
    const dvbsh::RxStatus s = m.rx.status();
    // the automatic channel width: no OFDM symbols at this width for a while, try the next
    if (s.level == 0) {
        m.searchSecs += secs;
        if (m.searchSecs > 0.3 && (m.waveform == 0 || m.fixedBw <= 0)) m.nextTry();
    } else m.searchSecs = 0;
    t.seq = ++m.seq;
    t.tdm = false;
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
    if (t.tdm) {
        if (t.state == 2) { snprintf(b, sizeof b, "DVB-SH TDM: PL slots found (%.4f Msym/s), waiting for a frame", t.symbolRate / 1e6); return b; }
        if (!t.locked) { snprintf(b, sizeof b, "DVB-SH TDM %s: frames found, waiting for the signalling field", t.tdmMod >= 0 ? dvbsh::tdmModName(t.tdmMod) : "?"); return b; }
        snprintf(b, sizeof b, "DVB-SH TDM %s %.4f Msym/s rate %s, %d CU frames, interleaver %.2f s%s, codewords %llu ok %llu bad, Es/N0 %.1f dB", dvbsh::tdmModName(t.tdmMod),
                 t.symbolRate / 1e6, dvbsh::rateName(t.sf.pat), t.sf.frameWidthCu, t.tiSeconds, t.tiTooLarge ? " (too long for this receiver)" : "", (unsigned long long)t.blocksOk,
                 (unsigned long long)t.blocksBad, t.snrDb);
        return b;
    }
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
        if (c.modeOpt[5] >= 1 && c.modeOpt[5] <= 3) {   // SH-B (TDM)
            dvbsh::TdmSignalConfig tc;
            tc.p.mod = c.modeOpt[5] - 1; tc.p.pat = p.patHp; tc.p.bwMhz = p.bwMhz; tc.p.ofdmGuard = p.guard; tc.p.rollIdx = 1; tc.p.ti = p.ti;
            tc.sampleRate = rate; tc.snrDb = c.snrDb; tc.cfoHz = c.cfoHz; tc.clockPpm = c.sroPpm; tc.level = 0.22;
            const double cu = tc.p.frameCuOrTable(), sec = cu / dvbsh::tdmCuPerSlot(tc.p.mod) * dvbsh::kSlotSymbols / tc.p.symbolRate();
            const double net = dvbsh::tdmCodewords((int)cu, tc.p.pat) * 8.0 * 188 * 8 / sec;
            tdm_ = std::make_unique<dvbsh::TdmSignal>(tc, demoTsSource(net * 0.95));
            return;
        }
        const double fs = dvbsh::sampleRate(p.bwMhz);
        gen_ = std::make_unique<dvbsh::Generator>(p, demoTsSource(dvbsh::netBitrate(p, fs) * 0.95));
        rs_.configure(fs, rate * (1.0 + c.sroPpm * 1e-6));
        sigma_ = (float)(0.22 * std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0));
        dph_ = kTwoPi * c.cfoHz / rate;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        if (tdm_) { tdm_->generate(out, n); return; }
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
    std::unique_ptr<dvbsh::TdmSignal> tdm_;
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
