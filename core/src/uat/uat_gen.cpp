// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UAT test signal: modulated downlink frames (DO-282 vectors via dump978 fec_tests), binary CPFSK at the real bit rate (3.9).
#include "dect2/uat/uat_gen.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

constexpr int kSyncBits = 36;
constexpr uint64_t kAdsbSync = 0xEACDDA4E2UL;
constexpr uint64_t kUplinkSync = 0x153225B1DUL;
using dect2::math::kPi;

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parseHex(const char* s, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        const int hi = hexNibble(s[i * 2]), lo = hexNibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Binary CPFSK as DO-282B sends it: 1.041667 Mbit/s, modulation index 0.6 (+-312.5 kHz), MSB first, a 1 above the carrier. The frequency
// steps are smoothed by a Gaussian of BT 0.5 (the standard limits the spectrum, not the pulse: an approximation of a real transmitter).
// Up to 3.8 the test signal sent one bit per two samples of whatever rate it was made at, +-2.2 degrees a sample: only a demodulator built the
// same way could read it.
constexpr double kBitRate = 1041666.6666667, kDevHz = 312500.0;
constexpr int kOs = 16;   // frequency samples per bit

static std::vector<cf32> modulateBits(const std::vector<uint8_t>& bits, double rate, double cfoHz, float gain) {
    // Gaussian frequency pulse, BT 0.5, over 3 bits
    const double bt = 0.5, sig = std::sqrt(std::log(2.0)) / (2 * kPi * bt);   // in bits
    std::vector<double> g(3 * kOs + 1);
    double gs = 0;
    for (size_t k = 0; k < g.size(); k++) { const double t = ((double)k - 1.5 * kOs) / kOs; g[k] = std::exp(-t * t / (2 * sig * sig)); gs += g[k]; }
    for (double& v : g) v /= gs;
    const size_t nb = bits.size(), nf = (nb + 4) * kOs;
    std::vector<double> nrz(nf, 0.0), ph(nf + 1, 0.0);
    for (size_t b = 0; b < nb; b++) for (int k = 0; k < kOs; k++) nrz[(b + 2) * kOs + (size_t)k] = bits[b] ? 1.0 : -1.0;
    const double step = 2 * kPi * kDevHz / (kBitRate * kOs);
    for (size_t k = 0; k < nf; k++) {
        double f = 0;
        for (size_t j = 0; j < g.size(); j++) { const long q = (long)k + (long)j - (long)(1.5 * kOs); if (q >= 0 && q < (long)nf) f += g[j] * nrz[(size_t)q]; }
        ph[k + 1] = ph[k] + step * f;
    }
    // the first bit starts 2 bits into the waveform (room for the pulse's rise)
    const double t0 = 2.0 / kBitRate, dur = (double)(nb + 2) / kBitRate;
    const size_t ns = (size_t)std::ceil(dur * rate);
    std::vector<cf32> y(ns);
    const double w = 2 * kPi * cfoHz / rate;
    for (size_t i = 0; i < ns; i++) {
        const double pos = ((double)i / rate + t0) * kBitRate * kOs;
        const size_t k = std::min((size_t)pos, nf - 1);
        const double fr = pos - (double)k;
        const double a = ph[k] * (1 - fr) + ph[std::min(k + 1, nf)] * fr + w * (double)i;
        y[i] = cf32((float)(gain * std::cos(a)), (float)(gain * std::sin(a)));
    }
    return y;
}

static std::vector<cf32> modulateCodeword(const uint8_t* codeword, int codewordBytes, bool uplink, double rate, double cfoHz, float gain) {
    std::vector<uint8_t> bits;
    const uint64_t sync = uplink ? kUplinkSync : kAdsbSync;
    for (int i = 0; i < kSyncBits; i++) bits.push_back((uint8_t)((sync >> (35 - i)) & 1));
    for (int i = 0; i < codewordBytes; i++) for (int b = 7; b >= 0; b--) bits.push_back((uint8_t)((codeword[i] >> b) & 1));
    return modulateBits(bits, rate, cfoHz, gain);
}

} // namespace

std::vector<cf32> uatBurst(const UatTx& tx, double rate) {
    const int cw = tx.uplink ? UAT_UPLINK_FRAME_BYTES : ((tx.frame[0] >> 3) == 0 ? UAT_SHORT_FRAME_BYTES : UAT_LONG_FRAME_BYTES);
    const float gain = (float)std::pow(10.0, tx.gainDb / 20.0) * 0.08f;
    return modulateCodeword(tx.frame, cw, tx.uplink, rate, tx.cfoHz, gain);
}

std::vector<cf32> uatScene(double rate, double dur, const std::vector<UatTx>& txs, double snrDb, unsigned seed) {
    const size_t N = (size_t)(dur * rate);
    std::vector<cf32> out(N);
    for (const UatTx& tx : txs) {
        auto b = uatBurst(tx, rate);
        const size_t s0 = (size_t)(tx.timeSec * rate);
        for (size_t i = 0; i < b.size() && s0 + i < N; i++) out[s0 + i] += b[i];
    }
    double pwr = 0;
    for (auto& v : out) pwr += std::norm(v);
    const float sig = out.empty() ? 0 : (float)std::sqrt(pwr / (double)out.size());
    const float sigma = sig / (float)std::pow(10.0, snrDb / 20.0);
    genutil::NoiseSource ns(seed);
    ns.add(out.data(), out.size(), sigma);
    return out;
}

struct UatSynth : ModeSynth {
    double rate = 0;
    double sampleRate() const override { return rate; }
    SynthConfig cfg;
    size_t idx = 0;
    std::vector<UatTx> sched;
    std::vector<cf32> buf;
    size_t pos = 0;
    bool gap = false;   // the next piece is the silence after a burst

    bool configure(const SynthConfig& c) override {
        if (c.modeOpt[0] != cfg.modeOpt[0] && !sched.empty()) return false;
        cfg = c;
        idx = 0;
        pos = 0;
        gap = false;
        buf.clear();
        sched.clear();
        // the DO-282 basic frame of dump978's fec_tests after its 6 byte errors were corrected (3.9: the synth sent the vector with the errors, so a
        // receiver was at its correction limit before any noise)
        static const char* kVec = "007E6987D2D74238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8";
        const int nAc = cfg.modeOpt[0] > 0 ? cfg.modeOpt[0] : 4;
        for (int a = 0; a < nAc; a++) {
            UatTx tx;
            tx.timeSec = 0.4 + a * 1.7;
            if (!parseHex(kVec, tx.frame, UAT_SHORT_FRAME_BYTES)) return false;
            tx.frameBytes = UAT_SHORT_FRAME_DATA_BYTES;
            tx.gainDb = -6.0f * (float)a;
            tx.cfoHz = (a - 1) * 1200.0;
            sched.push_back(tx);
        }
        return true;
    }

    void generate(cf32* x, size_t n) override {
        while (n) {
            if (pos >= buf.size()) {
                if (sched.empty()) {
                    std::fill(x, x + n, cf32(0, 0));
                    return;
                }
                // the bursts with 10 ms of silence after each and the whole schedule again after a second: as a transmitter it used to send
                // the bursts back to back as one and then stay silent for good
                if (gap) { gap = false; buf.assign((size_t)((idx >= sched.size() ? 1.0 : 0.01) * rate), cf32(0, 0)); if (idx >= sched.size()) idx = 0; }
                else { buf = uatBurst(sched[idx++], rate); gap = true; }
                pos = 0;
            }
            const size_t m = std::min(n, buf.size() - pos);
            for (size_t i = 0; i < m; i++) x[i] = buf[pos + i];
            pos += m;
            x += m;
            n -= m;
        }
    }
};

std::unique_ptr<ModeSynth> makeUatSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < uatTuning().minSampleRate - 1) return nullptr;
    auto s = std::make_unique<UatSynth>();
    s->rate = sampleRate;
    if (!s->configure(cfg)) return nullptr;
    return s;
}

} // namespace dect2
