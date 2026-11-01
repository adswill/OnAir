#include "dect2/aero/aero_cgen.h"
#include <cmath>
#include <cstring>

namespace dect2 {

struct AeroCGenerator::Impl {
    AeroCGenConfig cfg;
    AeroModulator mod;
    AeroCFrameEncoder enc;
    uint64_t rng, nFrames = 0;
    std::vector<std::array<uint8_t, 12>> voice;
    double ph = 0;
    uint64_t nrng;
    explicit Impl(const AeroCGenConfig& c) : cfg(c), mod(kAeroCBitRate, c.fs, c.clockPpm), rng(0x9E3779B97F4A7C15ull ^ ((uint64_t)c.seed * 0x100000001B3ull)), nrng(0xD1B54A32D192ED03ull ^ c.seed) {}
    uint8_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint8_t)(rng >> 24); }
    float gauss() {
        auto u = [&]() { nrng ^= nrng << 13; nrng ^= nrng >> 7; nrng ^= nrng << 17; return (float)((nrng >> 11) & 0xFFFFFF) / 16777216.f - 0.5f; };
        return (u() + u() + u() + u()) * 1.7320508f;
    }
    void queueFrame() {
        uint8_t v[300], su[36];
        for (int i = 0; i < kAeroCVoiceFrames; i++) {
            std::array<uint8_t, 12> f;
            for (int k = 0; k < 12; k++) f[k] = cfg.voiceIdle ? (uint8_t)(0x5A + k) : rnd();
            std::memcpy(v + i * 12, f.data(), 12);
            voice.push_back(f);
        }
        for (int j = 0; j < kAeroCSus; j++) {
            uint8_t* s = su + j * 12;
            std::memset(s, 0, 12);
            const bool call = j == 2 && cfg.callEvery > 0 && nFrames % (uint64_t)cfg.callEvery == 1;
            if (call) {
                s[0] = kAeroCCallProgress;
                s[1] = (uint8_t)(cfg.aesId >> 16); s[2] = (uint8_t)(cfg.aesId >> 8); s[3] = (uint8_t)cfg.aesId;
                s[4] = cfg.gesId; s[5] = 0xD0; s[6] = rnd(); s[7] = rnd(); s[8] = 0x40;
            } else {
                s[0] = kAeroCFillIn; s[4] = cfg.gesId;
            }
            aeroSuSetCrc(s);
        }
        const std::vector<uint8_t> bits = enc.frame(v, su);
        mod.pushBits(bits.data(), bits.size());
        nFrames++;
    }
};

AeroCGenerator::AeroCGenerator(const AeroCGenConfig& cfg) : p_(std::make_unique<Impl>(cfg)) {}
AeroCGenerator::~AeroCGenerator() = default;
const std::vector<std::array<uint8_t, 12>>& AeroCGenerator::sentVoice() const { return p_->voice; }
uint64_t AeroCGenerator::framesSent() const { return p_->nFrames; }

void AeroCGenerator::generate(cf32* out, size_t n) {
    Impl& s = *p_;
    const double bitsPerSample = kAeroCBitRate * (1 + s.cfg.clockPpm * 1e-6) / s.cfg.fs;
    while (s.mod.queuedBits() < (size_t)(bitsPerSample * (double)n) + 64) s.queueFrame();
    s.mod.generate(out, n);
    const double w = 2 * M_PI * s.cfg.carrierHz / s.cfg.fs;
    const float sigma = s.cfg.ebn0Db > 0 ? (float)(s.cfg.amplitude * std::sqrt(s.cfg.fs / (2.0 * kAeroCBitRate * std::pow(10.0, s.cfg.ebn0Db / 10)))) : 0.f;
    for (size_t i = 0; i < n; i++) {
        cf32 v = out[i] * (float)s.cfg.amplitude;
        if (s.cfg.carrierHz != 0) { v *= cf32((float)std::cos(s.ph), (float)std::sin(s.ph)); s.ph += w; if (s.ph > M_PI) s.ph -= 2 * M_PI; else if (s.ph < -M_PI) s.ph += 2 * M_PI; }
        if (sigma > 0) v += cf32(s.gauss() * sigma, s.gauss() * sigma);
        out[i] = v;
    }
}

} // namespace dect2
