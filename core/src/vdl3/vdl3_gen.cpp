// VDL Mode 3 test signal: see vdl3_gen.h.
#include "dect2/vdl3/vdl3_gen.h"
#include "dect2/vdl3/vdl3_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include "dect2/vdl2/vdl2_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kSlotSec = 0.030;            // 4 slots in a 120 ms frame
constexpr int kGapSyms = 12;                  // quiet symbols between the end of the M burst and the V/D training (made up)

bool isUplink(Vdl3Kind k) { return k == Vdl3Kind::MUplink || k == Vdl3Kind::MUplink3T || k == Vdl3Kind::Handoff; }

Vdl2Wave waveOf(const std::vector<uint8_t>& bits) {
    Vdl2Wave w;
    static const float rampEnv[3] = {0.2f, 0.6f, 0.9f};   // vdl2Render's three ramp symbols, all at phase 0 ("000")
    for (float e : rampEnv) { w.ph.push_back(0); w.env.push_back(e); }
    int cur = 0;
    for (size_t i = 0; i < bits.size(); i += 3) {
        const int b = (bits[i] << 2) | ((i + 1 < bits.size() ? bits[i + 1] : 0) << 1) | (i + 2 < bits.size() ? bits[i + 2] : 0);
        cur = (cur + vdl3StepForBits(b)) % 8;
        w.ph.push_back((int8_t)cur);
        w.env.push_back(1.f);
    }
    w.ph.push_back((int8_t)cur); w.env.push_back(0.6f);
    w.ph.push_back((int8_t)cur); w.env.push_back(0.25f);
    return w;
}
} // namespace

std::vector<uint8_t> Vdl3TxBurst::bits() const { return vdl3BurstBits(kind, info12, data, voice); }

Vdl3Traffic::Vdl3Traffic(const Vdl3GenOptions& o) : o_(o), rng_(o.seed ? o.seed * 2654435761u + 12345u : 1u) {}

uint32_t Vdl3Traffic::rnd() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return rng_;
}

Vdl3TxBurst Vdl3Traffic::next() {
    const double t0 = (double)slot_ * kSlotSec + 0.002;
    Vdl3TxBurst b;
    auto aircraft = [&](Vdl3TxBurst& x) {
        x.levelDb = o_.aircraftLevelDb - (double)(rnd() % 60) / 10.0;
        x.carOffHz = ((double)(rnd() % 2001) / 1000.0 - 1.0) * o_.aircraftSpreadHz;
    };
    if (phase_ == 0) {
        static const Vdl3Kind kPattern[12] = {Vdl3Kind::MUplink, Vdl3Kind::MUplink, Vdl3Kind::MUplink, Vdl3Kind::MDownlink,
                                              Vdl3Kind::MUplink, Vdl3Kind::PollResponse, Vdl3Kind::MUplink, Vdl3Kind::NetEntry,
                                              Vdl3Kind::MUplink3T, Vdl3Kind::MUplink, Vdl3Kind::Handoff, Vdl3Kind::MDownlink};
        b.kind = kPattern[slot_ % 12];
        b.t = t0;
        for (int i = 0; i < vdl3GolayWords(b.kind); i++) b.info12.push_back(rnd() & 0xFFF);
        if (!isUplink(b.kind)) aircraft(b);
        if (b.kind == Vdl3Kind::MUplink3T) {   // a 3T M burst fills most of the slot: no V/D after it
            slot_++;
            return b;
        }
        vdAt_ = t0 + (double)(kVdl3SyncSyms + vdl3PayloadSymbols(b.kind) + kGapSyms) / kVdl3SymbolRate;
        phase_ = 1;
        return b;
    }
    if (phase_ == 1) {
        b.t = vdAt_;
        const bool data = slot_ % 3 != 2;
        b.kind = data ? Vdl3Kind::VdData : Vdl3Kind::VdVoice;
        b.info12.push_back(rnd() & 0xFFF);
        if (data) {
            char s[40];
            snprintf(s, sizeof s, " %06u", (unsigned)++counter_);
            const std::string txt = o_.text + s;
            b.data.assign(txt.begin(), txt.end());
            if (b.data.size() > (size_t)kVdl3RsK) b.data.resize(kVdl3RsK);
        } else {
            for (int i = 0; i < 576; i++) b.voice.push_back((uint8_t)(rnd() & 1));
        }
        if (slot_ & 1) aircraft(b);
        if (o_.adjacent) phase_ = 2;
        else { phase_ = 0; slot_++; }
        return b;
    }
    // the adjacent ground station: a data burst that overlaps this slot in time, 25 kHz above
    b.t = t0 + 0.001;
    b.kind = Vdl3Kind::VdData;
    b.info12.push_back(rnd() & 0xFFF);
    const std::string txt = "ADJACENT";
    b.data.assign(txt.begin(), txt.end());
    b.chanHz = 25000;
    phase_ = 0;
    slot_++;
    return b;
}

void vdl3Render(const std::vector<uint8_t>& bits, cf32* out, int64_t outStart, size_t n, double rate, double startSample, double carHz, double amp, double phase) {
    Vdl2Wave w = waveOf(bits);
    w.start = startSample;
    w.carHz = carHz;
    w.amp = amp;
    w.phase = phase;
    vdl2Render(w, out, outStart, n, rate);
}

double vdl3BurstSamples(size_t bits, double rate) { return ((double)(bits + 2) / 3 + 2 + 6) * rate / kVdl3SymbolRate; }

namespace {
struct Ev {
    double startS;
    std::vector<uint8_t> bits;
    double car, amp, phase;
    bool operator>(const Ev& o) const { return startS > o.startS; }
};

class Vdl3Synth : public ModeSynth {
public:
    Vdl3Synth(const SynthConfig& cfg, double rate)
        : cfg_(cfg), rate_(rate), fsEff_(rate * (1.0 + cfg.sroPpm * 1e-6)), traffic_(opts(cfg)), noise_(cfg.modeOpt[1] ? (uint32_t)cfg.modeOpt[1] : 1u) {
        if (cfg.snrDb < 90) {
            const double nPow = kAmp * kAmp / std::pow(10.0, cfg.snrDb / 10.0);
            sigma_ = (float)std::sqrt(nPow * rate / kVdl3SymbolRate / 2.0);
        }
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32(0, 0));
        const double endS = (double)(pos_ + (int64_t)n);
        while (nextBurst_.t * fsEff_ < endS + 0.05 * fsEff_ || !have_) {
            if (have_) {
                const Vdl3TxBurst& b = nextBurst_;
                Ev e;
                e.startS = b.t * fsEff_;
                e.bits = b.bits();
                // the radio sits tuneOffsetHz above the channel (vdl3Tuning), so the channel is that far below the centre of the capture; the engine mixes it back
                e.car = b.chanHz + b.carOffHz + cfg_.cfoHz + cfg_.modeVal[0] * b.t + cfg_.modeVal[1] - vdl3Tuning().tuneOffsetHz;
                e.amp = kAmp * std::pow(10.0, b.levelDb / 20.0);
                e.phase = 2 * kPi * (double)((phaseRng_ = phaseRng_ * 1103515245u + 12345u) >> 16 & 0xFFFF) / 65536.0;
                q_.push(std::move(e));
            }
            nextBurst_ = traffic_.next();
            have_ = true;
        }
        while (!q_.empty() && q_.top().startS - 8 * fsEff_ / kVdl3SymbolRate < endS) { active_.push_back(q_.top()); q_.pop(); }
        for (size_t k = 0; k < active_.size();) {
            const Ev& e = active_[k];
            vdl3Render(e.bits, out, pos_, n, fsEff_, e.startS, e.car, e.amp, e.phase);
            if (e.startS + vdl3BurstSamples(e.bits.size(), fsEff_) < endS) active_.erase(active_.begin() + (long)k);
            else k++;
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
        pos_ += (int64_t)n;
    }

private:
    static constexpr double kAmp = 0.1;
    static Vdl3GenOptions opts(const SynthConfig& c) {
        Vdl3GenOptions o;
        o.seed = c.modeOpt[1] ? (uint32_t)c.modeOpt[1] : 1u;
        o.adjacent = c.modeOpt[2] == 1;
        return o;
    }
    SynthConfig cfg_;
    double rate_, fsEff_;
    Vdl3Traffic traffic_;
    genutil::NoiseSource noise_;
    float sigma_ = 0;
    Vdl3TxBurst nextBurst_;
    bool have_ = false;
    uint32_t phaseRng_ = 99;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> q_;
    std::vector<Ev> active_;
    int64_t pos_ = 0;
};
} // namespace

std::unique_ptr<ModeSynth> makeVdl3Synth(const SynthConfig& cfg, double sampleRate) {
    if (!(sampleRate > 0) || !std::isfinite(sampleRate)) return nullptr;
    return std::make_unique<Vdl3Synth>(cfg, sampleRate);
}

} // namespace dect2
