// LR-FHSS test signal (see lrfhss_gen.h). Synthetic tests only; not checked against a real transmitter.
#include "dect2/lrfhss/lrfhss_gen.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kPulseRes = 1024;          // table points per bit
constexpr double kPulseLo = -1.5, kPulseHi = 2.5;

// frequency pulse of one bit: a rectangle of one bit through a Gaussian filter of BT = 1, as a fraction of the full deviation
const std::vector<float>& pulseTable() {
    static const std::vector<float> t = [] {
        const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 1.0);
        const int n = (int)((kPulseHi - kPulseLo) * kPulseRes) + 1;
        std::vector<float> v((size_t)n);
        for (int i = 0; i < n; i++) {
            const double u = kPulseLo + (double)i / kPulseRes;
            v[(size_t)i] = (float)(0.5 * (std::erf(u / (std::sqrt(2.0) * sigma)) - std::erf((u - 1) / (std::sqrt(2.0) * sigma))));
        }
        return v;
    }();
    return t;
}

struct Tx {
    std::vector<int8_t> sym;             // +-1 per bit
    std::vector<double> hopStart;        // in bits from the frame start (the first symbol is unmodulated)
    std::vector<double> hopHz;           // carrier of each hop in the output
    double start = 0, amp = 0, clock = 1;   // clock: transmitter bits per nominal bit time (1 + ppm)
    double phase = 0;
    size_t hop = 0;
    double endBits() const { return 1.0 + (double)sym.size() + 0.5; }
};
} // namespace

double lrfhssFrameSeconds(const LrfhssGenFrame& f) {
    return (1.0 + lrfhss::digest(f.p, f.payload.size()).bits) / lrfhss::kBitRate;
}

struct LrfhssRenderer::Impl {
    double rate, dialMhz, dialOffsetHz, sroPpm;
    std::mt19937 rng;
    std::vector<Tx> act;
    Impl(double r, double d, double o, double s, uint32_t seed) : rate(r), dialMhz(d), dialOffsetHz(o), sroPpm(s), rng(seed * 2654435761u + 17u) {}
};

LrfhssRenderer::LrfhssRenderer(double rate, double dialMhz, double dialOffsetHz, double sroPpm, uint32_t seed)
    : p_(std::make_unique<Impl>(rate, dialMhz, dialOffsetHz, sroPpm, seed)) {}
LrfhssRenderer::~LrfhssRenderer() = default;
size_t LrfhssRenderer::active() const { return p_->act.size(); }

void LrfhssRenderer::add(const LrfhssGenFrame& f, double startSec, double amp, double extraCfoHz) {
    using namespace lrfhss;
    Tx t;
    const std::vector<uint8_t> bits = frameBits(f.p, f.hopSeqId, f.payload.data(), f.payload.size());
    t.sym.reserve(bits.size());
    for (uint8_t b : bits) t.sym.push_back((int8_t)(b ? 1 : -1));
    const Digest d = digest(f.p, f.payload.size());
    const HopParams hp = hopParams(f.p, f.hopSeqId);
    const std::vector<int> seq = gridSequence(f.p, f.hopSeqId, f.p.hopping ? d.hops : 1);
    const double centre = (f.mhz - p_->dialMhz) * 1e6 + p_->dialOffsetHz + f.cfoHz + extraCfoHz;
    double at = 0;
    for (size_t k = 0; k < seq.size(); k++) {
        const int s = (int)k < f.p.headers ? f.p.headers - 1 - (int)k : -1;
        t.hopStart.push_back(at);
        t.hopHz.push_back(centre + hopOffsetHz(f.p, hp.nGrid, seq[k], f.deviceOffset, s) * (1 + p_->sroPpm * 1e-6));   // the hop steps come from the same crystal
        if (k == 0) at += 1;   // the unmodulated first symbol
        at += (int)k < f.p.headers ? kHeaderBits : kBlockBits;
    }
    t.start = startSec;
    t.amp = amp;
    t.clock = 1 + p_->sroPpm * 1e-6;
    t.phase = std::uniform_real_distribution<double>(0, 2 * kPi)(p_->rng);
    p_->act.push_back(std::move(t));
}

void LrfhssRenderer::render(cf32* out, size_t n, double t0) {
    Impl& m = *p_;
    const auto& tab = pulseTable();
    const double dev = lrfhss::kBitRate / 4;           // modulation index 0.5
    for (Tx& t : m.act) {
        const double endSec = t.start + t.endBits() / (lrfhss::kBitRate * t.clock);
        if (t0 + (double)n / m.rate < t.start || t0 > endSec) continue;
        for (size_t i = 0; i < n; i++) {
            const double ts = t0 + (double)i / m.rate;
            if (ts < t.start || ts > endSec) continue;
            const double u = (ts - t.start) * lrfhss::kBitRate * t.clock;   // transmitter bit time
            while (t.hop + 1 < t.hopStart.size() && u >= t.hopStart[t.hop + 1]) t.hop++;
            // bit b occupies [1 + b, 2 + b)
            double f = 0;
            const long b0 = (long)std::floor(u) - 1;
            for (long b = b0 - 1; b <= b0 + 1; b++) {
                if (b < 0 || b >= (long)t.sym.size()) continue;
                const double x = u - 1 - (double)b;
                if (x < kPulseLo || x >= kPulseHi) continue;
                f += t.sym[(size_t)b] * tab[(size_t)((x - kPulseLo) * kPulseRes)];
            }
            t.phase += 2 * kPi * (t.hopHz[t.hop] + dev * f) / m.rate;
            if (t.phase > 2 * kPi || t.phase < -2 * kPi) t.phase = std::fmod(t.phase, 2 * kPi);
            out[i] += cf32((float)(t.amp * std::cos(t.phase)), (float)(t.amp * std::sin(t.phase)));
        }
    }
    const double t1 = t0 + (double)n / m.rate;
    m.act.erase(std::remove_if(m.act.begin(), m.act.end(), [&](const Tx& t) { return t.start + t.endBits() / (lrfhss::kBitRate * t.clock) < t1; }), m.act.end());
}

double lrfhssSceneSeconds(const std::vector<LrfhssGenFrame>& v, double leadSec) {
    double t = leadSec, end = 0;
    for (const auto& f : v) {
        const double s = f.startSec >= 0 ? f.startSec : t;
        const double e = s + lrfhssFrameSeconds(f);
        end = std::max(end, e);
        t = std::max(t, e + f.gapSec);
    }
    return end + 0.2;
}

struct LrfhssGenerator::Impl {
    LrfhssGenConfig c;
    LrfhssRenderer r;
    genutil::NoiseSource noise;
    int64_t pos = 0;
    size_t next = 0;
    double nextStart = 0, cycle = 0, lastEnd = 0;

    explicit Impl(const LrfhssGenConfig& cfg) : c(cfg), r(cfg.rate, cfg.dialMhz, cfg.dialOffsetHz, cfg.sroPpm, cfg.seed), noise(cfg.seed * 7919u + 11u) {
        nextStart = c.leadSec;
        cycle = 0;
    }
    double noiseRms() const {
        if (c.ebN0Db >= 99) return 0;
        // Eb = amp^2 / bit rate; noise power per sample = N0 * rate
        return c.amp * std::sqrt(c.rate / lrfhss::kBitRate / std::pow(10.0, c.ebN0Db / 10));
    }
    void schedule(double until) {
        while (!c.frames.empty()) {   // a frame with a fixed start may begin before the previous one ends
            if (next >= c.frames.size()) {
                if (!c.loop) return;
                next = 0;
                cycle = std::max(lastEnd + 0.3, nextStart);
                nextStart = cycle;
            }
            const LrfhssGenFrame& g = c.frames[next];
            const double base = g.startSec >= 0 ? cycle + g.startSec : nextStart;
            if (base > until) return;
            next++;
            r.add(g, base, c.amp * std::pow(10.0, g.snrOffDb / 20), c.cfoHz);
            const double e = base + lrfhssFrameSeconds(g) / (1 + c.sroPpm * 1e-6);
            nextStart = std::max(nextStart, e + g.gapSec);
            lastEnd = std::max(lastEnd, e);
        }
    }
};

LrfhssGenerator::LrfhssGenerator(const LrfhssGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
LrfhssGenerator::~LrfhssGenerator() = default;
double LrfhssGenerator::endSec() const { return p_->lastEnd; }

void LrfhssGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double t0 = (double)m.pos / m.c.rate, t1 = (double)(m.pos + (int64_t)n) / m.c.rate;
    m.schedule(t1 + 1);
    for (size_t i = 0; i < n; i++) out[i] = cf32(0, 0);
    m.r.render(out, n, t0);
    const double rms = m.noiseRms();
    if (rms > 0) m.noise.add(out, n, (float)(rms / std::sqrt(2.0)));
    m.pos += (int64_t)n;
}

} // namespace dect2
