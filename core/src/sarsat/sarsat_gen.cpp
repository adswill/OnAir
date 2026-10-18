// COSPAS-SARSAT test signal (see sarsat_gen.h).
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/sarsat/sarsat_rx.h"
#include <algorithm>
#include <cmath>

namespace dect2::sarsat {

using dect2::math::kPi;

std::vector<Spec> testBeacons() {
    std::vector<Spec> v;
    {   // EPIRB with an MMSI, long message with the fine position offsets: off the Lithuanian coast
        Spec s; s.protoCode = 2; s.mid = 277; s.ident = (123456u << 4) | 3u; s.lat = 55.7041; s.lon = 20.9873; s.fine = true; s.homing = true;
        v.push_back(s);
    }
    {   // PLB with a serial number, short message (position to 15 minutes)
        Spec s; s.longMsg = false; s.protoCode = 7; s.mid = 366; s.ident = (0x1A5u << 14) | 4021u; s.lat = 37.7749; s.lon = -122.4194; s.homing = false;
        v.push_back(s);
    }
    {   // ELT with a 24 bit aircraft address, long message: over the Baltic Sea
        Spec s; s.protoCode = 3; s.mid = 211; s.ident = 0x3C6444; s.lat = 54.1310; s.lon = 12.0950; s.fine = true; s.homing = true;
        v.push_back(s);
    }
    {   // a maritime user protocol beacon (no position in the message), short
        Spec s; s.longMsg = false; s.userProto = true; s.protoCode = 2; s.mid = 219; s.userBits = (baudotPack("OXTEST", 6) << 10) | (baudotPack("1", 1) << 4); s.hasPos = false;
        v.push_back(s);
    }
    {   // an EPIRB in the southern hemisphere, long
        Spec s; s.protoCode = 6; s.mid = 512; s.ident = (0x2B7u << 14) | 777u; s.lat = -36.8485; s.lon = 174.7633; s.fine = true;
        v.push_back(s);
    }
    return v;
}

std::vector<std::complex<float>> burstSamples(const std::vector<uint8_t>& bits, double rate, double carrierHz, double ratePct, bool inverted) {
    const double bitT = 1.0 / (kBitRate * (1 + ratePct / 100.0));
    const double pre = 0.160, edge = 0.0003;
    const double total = pre + (double)bits.size() * bitT;
    // the message bits include the 24 sync bits; the preamble comes before them
    const size_t n = (size_t)std::llround((total + 0.02) * rate);
    std::vector<std::complex<float>> out(n);
    const double dev = 1.1;
    auto level = [&](long half) -> double {   // phase of half-bit number `half` (two per bit)
        if (half < 0 || half >= (long)bits.size() * 2) return 0.0;
        const bool one = bits[(size_t)(half / 2)] != 0;
        const bool first = half % 2 == 0;
        return ((one == first) ? 1.0 : -1.0) * (inverted ? -1.0 : 1.0) * dev;
    };
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        double m = 0, amp = 1;
        if (t >= pre) {
            const double u = (t - pre) / (bitT / 2);
            const long h = (long)std::floor(u);
            const double f = (u - (double)h) * (bitT / 2);   // seconds into this half bit
            m = level(h);
            const double halfT = bitT / 2;
            if (f < edge / 2) m = level(h - 1) + (m - level(h - 1)) * (0.5 + 0.5 * std::sin(kPi * f / edge));
            else if (halfT - f < edge / 2) m = m + (level(h + 1) - m) * (0.5 - 0.5 * std::sin(kPi * (halfT - f) / edge));
        }
        if (t > total) amp = std::max(0.0, 1.0 - (t - total) / 0.0005);
        if (t < 0.0005) amp = t / 0.0005;
        const double a = ph + m;
        out[i] = std::complex<float>((float)(amp * std::cos(a)), (float)(amp * std::sin(a)));
        ph += 2 * kPi * carrierHz / rate;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    return out;
}

} // namespace dect2::sarsat

namespace dect2 {

namespace {

class SarsatSynth : public ModeSynth {
public:
    SarsatSynth(const SynthConfig& cfg, double rate) : rate_(rate), noise_(cfg.modeOpt[3] > 0 ? (uint32_t)cfg.modeOpt[3] : 1u) {
        const std::vector<sarsat::Spec> all = sarsat::testBeacons();
        const double carrier = cfg.cfoHz - sarsatTuning().tuneOffsetHz;
        const int pick = cfg.modeOpt[0];
        for (size_t i = 0; i < all.size(); i++) {
            if (pick > 0 && (int)i != pick - 1) continue;
            bursts_.push_back(sarsat::burstSamples(sarsat::encode(all[i]), rate, carrier, cfg.modeVal[0], cfg.modeOpt[1] == 1));
        }
        gap_ = (int64_t)std::llround((cfg.modeVal[1] > 0 ? cfg.modeVal[1] : 3.0) * rate);
        amp_ = 0.45f;
        sigma_ = 0.f;
        if (cfg.snrDb > -100) {
            const double sigRel = std::sqrt(rate / (2.0 * 2400.0 * std::pow(10.0, cfg.snrDb / 10.0)));
            amp_ = (float)std::min(0.45, 0.85 / (4.0 * sigRel));
            sigma_ = (float)(sigRel * amp_);
        }
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            const std::vector<std::complex<float>>& b = bursts_[idx_];
            out[i] = pos_ < (int64_t)b.size() ? b[(size_t)pos_] * amp_ : cf32(0.f, 0.f);
            if (++pos_ >= (int64_t)b.size() + gap_) { pos_ = 0; idx_ = (idx_ + 1) % bursts_.size(); }
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }

private:
    double rate_;
    genutil::NoiseSource noise_;
    std::vector<std::vector<std::complex<float>>> bursts_;
    int64_t gap_ = 0, pos_ = 0;
    size_t idx_ = 0;
    float amp_ = 0.45f, sigma_ = 0.f;
};

} // namespace

std::unique_ptr<ModeSynth> makeSarsatSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<SarsatSynth>(cfg, sampleRate);
}

} // namespace dect2
