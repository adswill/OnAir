// ADS-L test signal: see adsl_gen.h.
#include "dect2/adsl/adsl_gen.h"
#include "dect2/adsl/adsl_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <random>

namespace dect2 {

using dect2::math::kPi;
using namespace adsl;

std::vector<uint8_t> adslTrafficNet(const Traffic& t, int version, int keyIndex, bool fec) {
    uint8_t p[15];
    packTraffic(t, p);
    return makeNetwork(makeData(TypeTraffic, t.amt, t.addr, t.relay, p, 15), version, keyIndex, fec);
}

std::vector<cf32> adslBurst(const AdslTx& tx, double rate, double clockPpm) {
    std::vector<uint8_t> chips = frameChips(tx.band, tx.net, tx.preamble);
    if (tx.garbleLength && tx.band != BandLdr) {
        const int cpb = tx.band == BandHdr ? 1 : 2;
        // the length byte is the first after the 16 sync bits; invert the chips of two of its bits (a Manchester pair stays a valid pair)
        size_t pre = tx.band == BandHdr ? 2 * (size_t)(tx.preamble < 0 ? 12 : tx.preamble) : 2 * (size_t)(tx.preamble < 0 ? 8 : tx.preamble) + 8;
        const size_t o = pre + 16 * (size_t)cpb;
        for (size_t i = 0; i < 2 * (size_t)cpb; i++) chips[o + i] ^= 1;
    }
    const double tchip = (1.0 + clockPpm * 1e-6) / kChipRate[tx.band];
    const double sigma = 0.1325 / kGaussBt[tx.band] * tchip * 1.0;      // sqrt(ln 2) / (2 pi BT) = 0.1325 / BT
    const size_t n = (size_t)((double)(chips.size() + 6) * tchip * rate);
    std::vector<float> fr(n, 0.f);
    const int lead = 3;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate / tchip - lead;
        const int c = std::max(0, std::min((int)chips.size() - 1, (int)std::floor(t)));
        fr[i] = chips[(size_t)c] ? 1.f : -1.f;
    }
    const int half = (int)std::ceil(3.0 * sigma * rate);
    std::vector<float> g((size_t)(2 * half + 1));
    float gs = 0;
    for (int k = -half; k <= half; k++) { const double x = (double)k / rate / sigma; g[(size_t)(k + half)] = (float)std::exp(-0.5 * x * x); gs += g[(size_t)(k + half)]; }
    for (float& v : g) v /= gs;
    std::vector<cf32> out(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double s = 0;
        for (int k = -half; k <= half; k++) {
            const long j = (long)i + k;
            s += g[(size_t)(k + half)] * (j < 0 ? fr[0] : j >= (long)n ? fr[n - 1] : fr[(size_t)j]);
        }
        ph += 2 * kPi * (kDeviationHz[tx.band] * s + tx.cfoHz) / rate;
        out[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    const size_t ramp = std::min<size_t>(n / 2, (size_t)(rate * 4e-6) + 1);
    for (size_t i = 0; i < ramp; i++) { const float a = (float)i / (float)ramp; out[i] *= a; out[n - 1 - i] *= a; }
    const float amp = (float)std::pow(10.0, tx.gainDb / 20.0);
    for (auto& v : out) v *= amp;
    return out;
}

std::vector<cf32> adslScene(double rate, double centreHz, double dur, const std::vector<AdslTx>& txs, double snrDb, double clockPpm, unsigned seed) {
    const size_t N = (size_t)(dur * rate);
    std::vector<cf32> out(N, cf32(0.f, 0.f));
    for (const AdslTx& tx : txs) {
        const std::vector<cf32> b = adslBurst(tx, rate, clockPpm);
        const size_t s0 = (size_t)(tx.timeSec * rate);
        const double w = 2 * kPi * (kChannelHz[tx.band] - centreHz) / rate;
        for (size_t i = 0; i < b.size() && s0 + i < N; i++) {
            const double a = w * (double)(s0 + i);
            out[s0 + i] += b[i] * cf32((float)std::cos(a), (float)std::sin(a));
        }
    }
    if (snrDb > -100) {
        const double sigma = std::sqrt(rate / 200000.0 / std::pow(10.0, snrDb / 10.0) / 2.0);
        std::mt19937 rng(seed);
        genutil::PortableNormal nd((float)sigma);
        for (auto& v : out) { const float re = nd(rng); v += cf32(re, nd(rng)); }
    }
    return out;
}

namespace {

class AdslSynth : public ModeSynth {
public:
    AdslSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfo_(cfg.cfoHz), rng_(9), nd_(1.f) {
        lat0_ = cfg.modeVal[0] == 0 && cfg.modeVal[1] == 0 ? 54.70 : cfg.modeVal[0];
        lon0_ = cfg.modeVal[0] == 0 && cfg.modeVal[1] == 0 ? 25.30 : cfg.modeVal[1];
        const double snr = cfg.snrDb;
        const double sig1 = snr < -100 ? 0.0 : std::sqrt(rate / 200000.0 / std::pow(10.0, snr / 10.0) / 2.0);
        amp_ = sig1 > 0.4 ? (float)(0.4 / sig1) : 0.5f;
        sigma_ = (float)(sig1 * amp_);
        nd_ = genutil::PortableNormal(sigma_);
        wide_ = rate >= 1.6e6;
        centre_ = wide_ ? 868.875e6 : 868.3e6;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const int64_t end = pos_ + (int64_t)n;
        while (scheduledTo_ < end + (int64_t)(rate_ * 0.5)) scheduleSecond();
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        for (auto it = bursts_.begin(); it != bursts_.end();) {
            const int64_t s0 = it->start, s1 = it->start + (int64_t)it->s.size();
            if (s1 <= pos_) { it = bursts_.erase(it); continue; }
            const int64_t a = std::max(s0, pos_), b = std::min(s1, end);
            const double w = 2 * kPi * (kChannelHz[it->band] - centre_) / rate_;
            for (int64_t k = a; k < b; k++) {
                const double ph = w * (double)k;
                out[(size_t)(k - pos_)] += it->s[(size_t)(k - s0)] * amp_ * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            ++it;
        }
        if (sigma_ > 0.f) for (size_t i = 0; i < n; i++) { const float re = nd_(rng_); out[i] += cf32(re, nd_(rng_)); }
        pos_ = end;
    }
private:
    struct Burst { int64_t start; Band band; std::vector<cf32> s; };
    Traffic craft(int i, double t) const {
        static const struct { uint32_t id; int amt, cat; } kAc[4] = {{0xA12345, AmtIcao, 1}, {0xDD4711, AmtFlarm, 4}, {0x7B0042, AmtOgn, 7}, {0x5C3901, AmtRandom, 3}};
        Traffic p;
        p.amt = kAc[i].amt; p.addr = kAc[i].id; p.category = kAc[i].cat; p.flightState = 2;
        const double r = 3000.0 + 1800.0 * i, v = 26.0 + 14.0 * i, th = v / r * t + i;
        p.hasPos = true;
        p.lat = lat0_ + r * std::cos(th) / 111320.0;
        p.lon = lon0_ + r * std::sin(th) / (111320.0 * std::cos(lat0_ * kPi / 180.0));
        p.hasAlt = true; p.altM = 800 + 150 * i;
        p.hasSpeed = true; p.speedMs = v; p.hasClimb = true; p.climbMs = -1.5 + i; p.hasTrack = true; p.trackDeg = std::fmod((th + kPi / 2) * 180.0 / kPi, 360.0);
        p.hAcc = 6; p.vAcc = 2; p.velAcc = 2;
        return p;
    }
    void add(double t, Band band, std::vector<uint8_t> net) {
        AdslTx tx; tx.timeSec = t; tx.band = band; tx.net = std::move(net); tx.cfoHz = cfo_;
        Burst b; b.start = (int64_t)(t * rate_); b.band = band; b.s = adslBurst(tx, rate_);
        bursts_.push_back(std::move(b));
    }
    void scheduleSecond() {
        const double sec0 = (double)scheduledTo_ / rate_;
        for (int i = 0; i < 4; i++) {
            const double t = sec0 + 0.05 + 0.20 * i + 0.03 * (double)(rng_() & 15) / 16.0;
            const Band band = wide_ && i == 2 ? BandLdr : (((int)sec0 + i) & 1) ? BandM1 : BandM0;
            add(t, band, adslTrafficNet(craft(i, t)));
        }
        if (((int64_t)sec0 % 5) == 0) {
            Status s; s.maxVersion = 1; s.mRx = 3;
            uint8_t p[7]; packStatus(s, p);
            add(sec0 + 0.9, BandM0, makeNetwork(makeData(TypeStatus, AmtFlarm, 0xDD4711, false, p, 7), 1, 0, false));
        }
        if (wide_ && ((int64_t)sec0 % 3) == 0) {
            std::vector<uint8_t> pay;
            for (int i = 0; i < 2; i++) {
                const Traffic o = craft(i, sec0);
                uint8_t e[19] = {};
                const uint32_t w = (uint32_t)(o.amt & 63) | (o.addr & 0xFFFFFF) << 6;
                e[0] = (uint8_t)w; e[1] = (uint8_t)(w >> 8); e[2] = (uint8_t)(w >> 16); e[3] = (uint8_t)(w >> 24);
                packTraffic(o, e + 4);
                pay.insert(pay.end(), e, e + 19);
            }
            while ((pay.size() + 5) % 4) pay.push_back(0);
            add(sec0 + 0.35, BandHdr, makeNetwork(makeData(TypeUplink, AmtOgn, 0xF00001, false, pay.data(), pay.size()), 1, 0, true));
        }
        scheduledTo_ += (int64_t)rate_;
    }
    double rate_, cfo_, lat0_, lon0_, centre_ = 868.875e6;
    bool wide_ = true;
    std::mt19937 rng_;
    genutil::PortableNormal nd_;
    float amp_ = 0.5f, sigma_ = 0.f;
    int64_t pos_ = 0, scheduledTo_ = 0;
    std::deque<Burst> bursts_;
};

} // namespace

std::unique_ptr<ModeSynth> makeAdslSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < adslTuning().minSampleRate - 1) return nullptr;
    return std::make_unique<AdslSynth>(cfg, sampleRate);
}

} // namespace dect2
