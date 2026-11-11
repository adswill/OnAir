// UAT test signal: modulated downlink frames (DO-282 vectors via dump978 fec_tests).
#include "dect2/uat/uat_gen.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/uat/uat_demod.h"
#include <algorithm>
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

// binary CPFSK as DO-282B sends it: 1.041667 Mbit/s, modulation index 0.6 (+-312.5 kHz), a one above the carrier. The bits are timed at
// the bit rate whatever the sample rate (they used to be two samples a bit at any rate, so at 2 Msps 1 Mbit/s, with a deviation of 6 kHz)
static std::vector<cf32> modulateCodeword(const uint8_t* codeword, int codewordBytes, double rate, double cfoHz, float gain) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < kSyncBits; i++) bits.push_back((uint8_t)((kAdsbSync >> (35 - i)) & 1));
    for (int i = 0; i < codewordBytes; i++) for (int b = 7; b >= 0; b--) bits.push_back((uint8_t)((codeword[i] >> b) & 1));
    const double rb = kUatBitRate, dev = 0.6 * rb / 2;
    const size_t n = (size_t)std::ceil((double)bits.size() / rb * rate);
    std::vector<cf32> y(n);
    double ph = 0;
    for (size_t k = 0; k < n; k++) {
        const size_t bi = std::min(bits.size() - 1, (size_t)((double)k * rb / rate));
        y[k] = cf32((float)(gain * std::cos(ph)), (float)(gain * std::sin(ph)));
        ph += 2 * kPi * ((bits[bi] ? dev : -dev) + cfoHz) / rate;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    return y;
}

} // namespace

std::vector<cf32> uatBurst(const UatTx& tx, double rate) {
    const int cw = tx.uplink ? UAT_UPLINK_FRAME_BYTES : ((tx.frame[0] >> 3) == 0 ? UAT_SHORT_FRAME_BYTES : UAT_LONG_FRAME_BYTES);
    const float gain = (float)std::pow(10.0, tx.gainDb / 20.0) * 0.08f;
    return modulateCodeword(tx.frame, cw, rate, tx.cfoHz, gain);
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
        static const char* kVec = "FF8196782DD44238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8761BBCD9FCC817D82E2D1ACF90CA78DA3C49";
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
