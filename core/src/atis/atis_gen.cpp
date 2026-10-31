#include "dect2/atis/atis_gen.h"
#include "dect2/atis/atis_proto.h"
#include "dect2/atis/atis_rx.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <random>

namespace dect2 {

namespace {
constexpr double kBaud = 1200.0, kYHz = 1300.0, kBHz = 2100.0;
}

struct AtisGenerator::Impl {
    AtisGenConfig c;
    std::mt19937 rng;
    std::normal_distribution<double> nd{0.0, 1.0};
    double sigma = 0;
    // the transmission being sent: per-sample audio is made as it goes
    enum St { Gap, Voice, After, Data } st = Gap;
    double left = 0;                 // seconds left in the state
    std::vector<uint8_t> bits;
    double bitPos = 0;               // in bits
    size_t idx = 0;
    double carrierPh = 0, audPh = 0, vPh[3] = {0, 0, 0}, fmPh = 0, tAm = 0;

    explicit Impl(const AtisGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        if (c.ids.empty()) c.ids = atisTestIds();
        left = c.silent ? 1e12 : 0.05;
        if (c.snrDb < 99) sigma = std::sqrt(c.amp * c.amp / std::pow(10.0, c.snrDb / 10.0) * (c.rate / 12.5e3) / 2);
    }
    void begin() {
        st = c.voiceSec > 0 ? Voice : After;
        left = c.voiceSec > 0 ? c.voiceSec : c.afterVoiceSec;
        bits = atis::frameBits(c.ids[idx % c.ids.size()], c.dotBits);
        idx++;
        bitPos = 0; audPh = 0;
    }
    // one sample: audio in -1..1 and whether the carrier is on
    bool step(double& a) {
        a = 0;
        const double dt = 1.0 / c.rate;
        switch (st) {
        case Gap:
            left -= dt;
            if (left <= 0) begin();
            return false;
        case Voice: {
            // speech-like: three tones with a syllable rate envelope
            tAm += dt;
            const double env = 0.5 + 0.5 * std::sin(2 * math::kPi * 3.5 * tAm);
            const double f[3] = {420.0, 870.0, 1550.0};
            double s = 0;
            for (int k = 0; k < 3; k++) { vPh[k] += 2 * math::kPi * f[k] * dt; s += std::sin(vPh[k]) * (k == 0 ? 0.5 : k == 1 ? 0.3 : 0.15); }
            a = 0.7 * env * s;
            left -= dt;
            if (left <= 0) { st = After; left = c.afterVoiceSec; }
            return true;
        }
        case After:
            left -= dt;
            if (left <= 0) st = Data;
            return true;
        case Data: {
            const size_t bi = (size_t)bitPos;
            if (bi >= bits.size()) { st = Gap; left = c.gapSec; return false; }
            const double f = bits[bi] ? kYHz : kBHz;
            audPh += 2 * math::kPi * f * dt;
            a = std::cos(audPh);
            bitPos += kBaud * (1.0 + c.clockPpm * 1e-6) * dt;
            return true;
        }
        }
        return false;
    }
    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            double re = 0, im = 0;
            double a;
            const bool on = step(a);
            carrierPh += 2 * math::kPi * (c.dialOffsetHz + c.cfoHz) / c.rate;
            if (carrierPh > math::kPi) carrierPh -= 2 * math::kPi; else if (carrierPh < -math::kPi) carrierPh += 2 * math::kPi;
            if (on) {
                double th = carrierPh;
                if (c.phaseMod) th += c.modIndex * a;
                else { fmPh += 2 * math::kPi * (c.modIndex * 1700.0 * a) / c.rate; th += fmPh; }
                re = c.amp * std::cos(th); im = c.amp * std::sin(th);
            }
            if (sigma > 0) { re += sigma * nd(rng); im += sigma * nd(rng); }
            out[i] = cf32((float)re, (float)im);
        }
    }
};

AtisGenerator::AtisGenerator(const AtisGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
AtisGenerator::~AtisGenerator() = default;
void AtisGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

std::vector<cf32> AtisGenerator::transmission(const std::string& id, const AtisGenConfig& cfg) {
    AtisGenConfig c = cfg;
    c.ids = {id}; c.snrDb = 99; c.gapSec = 0;
    Impl m(c);
    m.left = 0; m.begin();
    std::vector<cf32> v;
    while (m.st != Impl::Gap) {
        cf32 s;
        m.generate(&s, 1);
        if (m.st == Impl::Gap) break;
        v.push_back(s);
    }
    return v;
}

std::vector<std::string> atisTestIds() {
    return {
        atis::idFromCallsign("PC8075", 244),   // the example of the standard: 9244038075
        atis::idFromCallsign("DB1234", 211),   // German
        atis::idFromCallsign("OT5678", 205),   // Belgian
        atis::idFromMmsi("207072225"),         // a radio programmed with 9 + MMSI (Bulgaria)
        atis::idFromCallsign("HB0042", 269),   // Swiss
    };
}

namespace {
class AtisSynth : public ModeSynth {
public:
    AtisSynth(const SynthConfig& s, double rate) : rate_(rate) {
        AtisGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -atisTuning().tuneOffsetHz;
        c.cfoHz = s.cfoHz;
        c.snrDb = s.snrDb;
        c.phaseMod = s.modeOpt[0] != 1;
        if (s.modeOpt[1] == 1) c.voiceSec = 0;
        if (s.modeOpt[2] == 1) c.dotBits = 0;
        c.clockPpm = s.modeOpt[3];
        gen_ = std::make_unique<AtisGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<AtisGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeAtisSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<AtisSynth>(cfg, sampleRate); }

} // namespace dect2
