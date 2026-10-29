// LoRaWAN FSK (DR7, 50 kbit/s GFSK): frame coding, airtime, modulator and receiver (see lorawan_fsk.h; the sources are listed there).
// Synthetic tests only; not checked against a real transmitter.
//
// Receiver, per FSK channel: two cascaded mix / low-pass / decimate stages (group_decimator.h) to about 250 ksps (5 samples per bit) with
// +-90 kHz kept (the +-50 kHz of the signal and a +-25 kHz crystal error at 868 MHz), an FM discriminator, a moving average over one bit (the
// matched filter of the frequency), then a normalised correlation against the end of the preamble and the sync word at every sample (two
// templates: the SX126x 0x55 preamble and the SX1276 0xAA one; either sign, so swapped tones are found too). A peak above the threshold gives
// the timing (parabolic interpolation), a least-squares fit over those 32 bits the carrier offset and the deviation. The bits after it are
// sliced at one-bit steps with a Gardner timing loop and a decision-directed threshold; the length byte (de-whitened) says how many follow,
// then de-whitening, CRC-16, and the PHY payload goes to lorawan::parse in the LoRaWAN monitor.
#include "dect2/lorawan/lorawan_fsk.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/group_decimator.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace dect2 {
namespace lorawan {
namespace fsk {

using math::kPi;

// ---------------------------------------------------------------- coding

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = kCrcSeed;
    for (size_t i = 0; i < n; i++) {
        uint8_t d = p[i];
        for (int b = 0; b < 8; b++) {   // the SX1276 datasheet's example (7.5): MSB first
            const bool x = (((crc & 0x8000) >> 8) ^ (d & 0x80)) != 0;
            crc = (uint16_t)(crc << 1);
            if (x) crc ^= kCrcPoly;
            d = (uint8_t)(d << 1);
        }
    }
    return (uint16_t)~crc;
}

void whiten(uint8_t* p, size_t n, uint16_t seed) {
    unsigned r = seed & 0x1FFu;          // X8 .. X0
    for (size_t i = 0; i < n; i++) {
        unsigned m = 0;
        for (int b = 7; b >= 0; b--) {   // the LSB at the LFSR output goes with the MSB of the data first
            m |= (r & 1u) << b;
            const unsigned fb = (r ^ (r >> 5)) & 1u;   // x^9 + x^5 + 1: X0 xor X5 into X8
            r = (r >> 1) | (fb << 8);
        }
        p[i] = (uint8_t)(p[i] ^ m);
    }
}

std::vector<uint8_t> encode(const uint8_t* phy, size_t n) {
    std::vector<uint8_t> v;
    if (n > kMaxPhyPayload) return v;
    v.reserve(n + 3);
    v.push_back((uint8_t)n);
    v.insert(v.end(), phy, phy + n);
    const uint16_t c = crc16(v.data(), v.size());
    v.push_back((uint8_t)(c >> 8));
    v.push_back((uint8_t)(c & 0xFF));
    whiten(v.data(), v.size());
    return v;
}

Unpacked decode(const uint8_t* onAir, size_t n) {
    Unpacked u;
    if (n < 1) return u;
    std::vector<uint8_t> b(onAir, onAir + n);
    whiten(b.data(), b.size());
    const size_t len = b[0];
    if (n < len + 3) return u;
    u.lengthOk = true;
    u.payload.assign(b.begin() + 1, b.begin() + 1 + (std::ptrdiff_t)len);
    u.crcCalc = crc16(b.data(), len + 1);
    u.crcRx = (uint16_t)(b[len + 1] << 8 | b[len + 2]);
    u.crcOk = u.crcCalc == u.crcRx;
    return u;
}

std::vector<uint8_t> frameBits(const uint8_t* phy, size_t n, uint8_t preambleByte, int preambleBytes) {
    std::vector<uint8_t> bytes((size_t)std::max(0, preambleBytes), preambleByte);
    bytes.push_back((uint8_t)(kSyncWord >> 16)); bytes.push_back((uint8_t)(kSyncWord >> 8)); bytes.push_back((uint8_t)kSyncWord);
    const std::vector<uint8_t> body = encode(phy, n);
    bytes.insert(bytes.end(), body.begin(), body.end());
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t v : bytes) for (int b = 7; b >= 0; b--) bits.push_back((uint8_t)((v >> b) & 1));
    return bits;
}

size_t frameBitCount(size_t phyLen, int preambleBytes) { return 8 * ((size_t)std::max(0, preambleBytes) + 3 + 1 + phyLen + 2); }
double airSeconds(size_t phyLen, int preambleBytes) { return (double)frameBitCount(phyLen, preambleBytes) / kBitRate; }
uint32_t airMsLoRaMacNode(size_t phyLen, int preambleBytes) {
    const uint64_t num = 1000ull * frameBitCount(phyLen, preambleBytes), den = (uint64_t)kBitRate;
    return (uint32_t)((num + den - 1) / den);
}

// ---------------------------------------------------------------- modulator

namespace {
// frequency response of one bit (a rectangle of one bit period through the Gaussian filter), x in bits from the bit's centre
double pulse(double x, double bt) {
    if (bt <= 0) return std::fabs(x) < 0.5 ? 1.0 : 0.0;
    const double s = std::sqrt(2 * std::log(2.0)) / (2 * kPi * bt);
    return 0.5 * (std::erf((x + 0.5) / s) - std::erf((x - 0.5) / s));
}
} // namespace

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double rate, double freqHz, double bt, double clockPpm, double startFrac, float amp, double phase0) {
    std::vector<cf32> out;
    if (bits.empty() || rate <= 0) return out;
    const double T = (1 + clockPpm * 1e-6) / kBitRate;   // bit period at the transmitter's clock
    const double ramp = 1.0;                              // the power amplifier ramps up during one bit before the first and down after the last
    const double nb = (double)bits.size();
    const size_t n = (size_t)std::ceil(((nb + 2 * ramp) * T) * rate - startFrac) + 1;
    out.resize(n);
    double ph = phase0;
    const int span = bt > 0 ? 3 : 1;
    for (size_t j = 0; j < n; j++) {
        const double tb = ((double)j + startFrac) / rate / T - ramp;   // time in bits from the start of the first bit
        const long k0 = (long)std::floor(tb);
        double f = 0;
        for (long k = k0 - span; k <= k0 + span; k++) {
            const size_t kb = (size_t)std::max(0L, std::min((long)bits.size() - 1, k));   // the ramps carry the first / last bit's tone
            f += (bits[kb] ? 1.0 : -1.0) * pulse(tb - ((double)k + 0.5), bt);
        }
        double a = amp;
        if (tb < 0) a *= 0.5 - 0.5 * std::cos(kPi * std::max(0.0, tb + ramp) / ramp);
        else if (tb > nb) a *= 0.5 - 0.5 * std::cos(kPi * std::max(0.0, nb + ramp - tb) / ramp);
        out[j] = cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
        ph += 2 * kPi * (freqHz + kDeviationHz * f) / rate;
        if (ph > 1e6 || ph < -1e6) ph = std::fmod(ph, 2 * kPi);
    }
    return out;
}

// ---------------------------------------------------------------- receiver

namespace {

constexpr int kTplBits = 32;               // the last preamble byte and the three sync bytes
constexpr double kSyncThreshold = 0.78;    // normalised correlation that starts a candidate
constexpr double kStrongSync = 0.9;        // above it a CRC failure is counted (below it it is most likely noise)
constexpr double kKeepHz = 90e3;           // half the band kept around the channel
constexpr size_t kMaxPending = 12;
constexpr size_t kNoiseBlock = 512, kNoiseBlocks = 64;

std::array<int, kTplBits> templateBits(uint8_t preamble) {
    std::array<int, kTplBits> t{};
    const uint32_t w = (uint32_t)preamble << 24 | kSyncWord;
    for (int k = 0; k < kTplBits; k++) t[(size_t)k] = (w >> (kTplBits - 1 - k)) & 1 ? 1 : -1;
    return t;
}

struct Cand {
    double p0 = 0;                 // sample (fractional, absolute) where the last sync bit ends in the bit-averaged stream
    double dev = 0, thr = 0, cfo = 0, tau = 0;
    double yPrev = 0;
    float score = 0;
    size_t need = 8, done = 0;
    std::vector<uint8_t> bits;
    double pwSum = 0; size_t pwN = 0;
};

struct Chan {
    double offHz = 0;
    bool on = false;
    bool two = false;              // stage 1 used
    GroupDecimator d1, d2;
    double r2 = 0, sps = 5;
    int W = 5;
    std::array<int, kTplBits> off{};
    std::array<double, kTplBits> tA{}, tB{};      // mean-removed templates (0xAA, 0x55 preamble)
    double nA = 1, nB = 1, mA = 0, mB = 0;        // their norms and means
    std::array<int, kTplBits> rawA{}, rawB{};
    // streams (absolute index base)
    std::vector<float> fr, mf, pw;
    int64_t base = 0;
    cf32 prev{0, 0};
    double maSum = 0;
    int64_t scanAt = 0, holdUntil = 0;
    // peak tracking
    bool inPeak = false;
    int64_t peakUntil = 0, bestIdx = 0;
    double bestC = 0;
    int bestTpl = 0;
    std::vector<Cand> pend;
    // noise floor
    double blk = 0; size_t blkN = 0;
    std::array<double, kNoiseBlocks> blocks{};
    size_t nBlocks = 0, blkPos = 0;
    double noise = 0;
    fsk::Receiver::Stats st;

    int64_t end() const { return base + (int64_t)fr.size(); }
    float at(const std::vector<float>& v, int64_t i) const { return v[(size_t)(i - base)]; }
    double interp(const std::vector<float>& v, double pos) const {
        const int64_t i = (int64_t)std::floor(pos);
        const double f = pos - (double)i;
        return (1 - f) * at(v, i) + f * at(v, i + 1);
    }
};

} // namespace

struct Receiver::Impl {
    double rate = 0;
    int64_t nIn = 0;
    std::vector<Chan> ch;
    std::vector<Stats> stats;

    void setup(Chan& c) {
        c.on = false;
        if (!Receiver::fits(rate, c.offHz)) return;
        const int D1 = rate >= 2e6 ? (int)std::floor(rate / 1e6) : 1;
        c.two = D1 >= 2;
        double r1 = rate;
        if (c.two) {
            if (!c.d1.configure(rate, c.offHz, 150e3, D1)) return;
            r1 = c.d1.outRate();
        }
        const int D2 = std::max(2, (int)std::floor(r1 / 250e3 + 1e-9));
        if (!c.d2.configure(r1, c.two ? 0.0 : c.offHz, kKeepHz, D2)) return;
        c.r2 = c.d2.outRate();
        c.sps = c.r2 / kBitRate;
        c.W = std::max(2, (int)std::lround(c.sps));
        for (int k = 0; k < kTplBits; k++) c.off[(size_t)k] = (int)std::lround((kTplBits - 1 - k) * c.sps);
        c.rawA = templateBits(0xAA); c.rawB = templateBits(0x55);
        auto prep = [](const std::array<int, kTplBits>& raw, std::array<double, kTplBits>& t, double& norm, double& mean) {
            mean = 0;
            for (int v : raw) mean += v;
            mean /= kTplBits;
            norm = 0;
            for (int k = 0; k < kTplBits; k++) { t[(size_t)k] = raw[(size_t)k] - mean; norm += t[(size_t)k] * t[(size_t)k]; }
        };
        prep(c.rawA, c.tA, c.nA, c.mA); prep(c.rawB, c.tB, c.nB, c.mB);
        c.on = true;
    }

    void clearStreams(Chan& c) {
        c.fr.clear(); c.mf.clear(); c.pw.clear();
        c.base = 0; c.prev = cf32(0, 0); c.maSum = 0;
        c.scanAt = 0; c.holdUntil = 0; c.inPeak = false; c.bestC = 0;
        c.pend.clear();
        c.blk = 0; c.blkN = 0; c.nBlocks = 0; c.blkPos = 0; c.noise = 0;
    }

    // normalised correlation at index i (the last template bit ends there) for template tpl (0: 0xAA, 1: 0x55); signed
    double corr(const Chan& c, int64_t i, int tpl) const {
        double sy = 0, syy = 0, d = 0;
        const auto& t = tpl ? c.tB : c.tA;
        for (int k = 0; k < kTplBits; k++) {
            const double y = c.at(c.mf, i - c.off[(size_t)k]);
            sy += y; syy += y * y; d += t[(size_t)k] * y;
        }
        const double var = syy - sy * sy / kTplBits;
        if (var <= 1e-9) return 0;
        return d / std::sqrt((tpl ? c.nB : c.nA) * var);
    }

    void startCand(Chan& c) {
        const int64_t i = c.bestIdx;
        const int tpl = c.bestTpl;
        if (i - 1 - c.off[0] < c.base || i + 1 >= c.end()) return;
        const double c0 = corr(c, i, tpl), cm = corr(c, i - 1, tpl), cp = corr(c, i + 1, tpl);
        const double sg = c0 < 0 ? -1 : 1;
        const double den = sg * (cm - 2 * c0 + cp);
        double delta = den < -1e-12 ? 0.5 * sg * (cm - cp) / den : 0;
        delta = std::max(-0.5, std::min(0.5, delta));
        Cand k;
        k.p0 = (double)i + delta;
        k.score = (float)std::fabs(c0);
        // carrier offset: the mean over the 16 alternating preamble bits before the sync word (symmetric, so the shaping does not bias it);
        // deviation: least squares over the 32 known bits, y = cfo + dev * t
        double sp = 0;
        for (int b = 0; b < 16; b++) {
            const double pos = k.p0 - (24 + 15 - b) * c.sps;
            if (pos < (double)c.base || pos + 1 >= (double)c.end()) return;
            sp += c.interp(c.mf, pos);
        }
        k.cfo = sp / 16;
        const auto& raw = tpl ? c.rawB : c.rawA;
        double stt = 0, sty = 0;
        for (int b = 0; b < kTplBits; b++) {
            const double pos = k.p0 - (kTplBits - 1 - b) * c.sps;
            if (pos < (double)c.base || pos + 1 >= (double)c.end()) return;
            const double t = raw[(size_t)b];
            stt += t * t; sty += t * (c.interp(c.mf, pos) - k.cfo);
        }
        k.dev = sty / stt;
        k.thr = k.cfo;
        if (std::fabs(k.dev) < 8e3 || std::fabs(k.dev) > 50e3 || std::fabs(k.cfo) > 70e3) return;
        // level over the preamble and the sync word
        const int64_t s0 = (int64_t)std::floor(k.p0 - (8.0 * (kPreambleBytes + 3)) * c.sps);
        for (int64_t j = std::max(c.base, s0); j <= i; j++) { k.pwSum += c.at(c.pw, j); k.pwN++; }
        if (c.pend.size() >= kMaxPending) return;
        if (k.score >= kStrongSync) c.st.syncs++;
        c.pend.push_back(std::move(k));
    }

    void scan(Chan& c) {
        const int64_t first = c.base + c.off[0] + 1;
        if (c.scanAt < first) c.scanAt = first;
        const int64_t e = c.end() - 1;    // corr at i needs i + 1 for the interpolation later
        for (int64_t i = c.scanAt; i < e; i++) {
            if (c.inPeak && i > c.peakUntil) { c.inPeak = false; startCand(c); c.holdUntil = c.bestIdx + (int64_t)(8 * c.sps); }
            if (i < c.holdUntil && !c.inPeak) continue;
            for (int tpl = 0; tpl < 2; tpl++) {
                const double v = corr(c, i, tpl);
                if (std::fabs(v) < kSyncThreshold) continue;
                if (!c.inPeak) { c.inPeak = true; c.peakUntil = i + (int64_t)std::ceil(c.sps); c.bestC = 0; }
                if (std::fabs(v) > c.bestC) { c.bestC = std::fabs(v); c.bestIdx = i; c.bestTpl = tpl; }
            }
        }
        c.scanAt = std::max(c.scanAt, e);
    }

    // slice the bits of a candidate that the stream already holds; true when it is finished (either way)
    bool advance(Chan& c, Cand& k, std::vector<RxFrame>& out, size_t chanIdx) {
        const double sgn = k.dev < 0 ? -1 : 1, adev = std::fabs(k.dev);
        while (k.done < k.need) {
            const double pos = k.p0 + (double)(k.done + 1) * c.sps + k.tau;
            if (pos + 2 >= (double)c.end()) return false;
            const double y = c.interp(c.mf, pos);
            const bool bit = (y - k.thr) * sgn > 0;
            const double ym = c.interp(c.mf, pos - 0.5 * c.sps) - k.thr;
            if (k.done > 0 && ((k.yPrev - k.thr) * sgn > 0) != bit) {   // Gardner, on transitions only
                const double e = ym * ((k.yPrev - k.thr) - (y - k.thr)) / (adev * adev);
                k.tau += std::max(-0.15, std::min(0.15, 0.25 * e * c.sps));
            }
            k.thr += 0.01 * (y - (k.thr + k.dev * (bit ? 1 : -1)) * 1.0);
            k.yPrev = y;
            k.pwSum += c.at(c.pw, (int64_t)std::floor(pos)); k.pwN++;
            k.bits.push_back(bit ? 1 : 0);
            k.done++;
            if (k.done == 8) {
                uint8_t len = 0;
                for (int b = 0; b < 8; b++) len = (uint8_t)(len << 1 | k.bits[(size_t)b]);
                whiten(&len, 1);
                if (len == 0) return true;
                k.need = 8 * (1 + (size_t)len + 2);
            }
        }
        std::vector<uint8_t> bytes(k.need / 8, 0);
        for (size_t b = 0; b < k.need; b++) bytes[b / 8] = (uint8_t)(bytes[b / 8] << 1 | k.bits[b]);
        Unpacked u = decode(bytes.data(), bytes.size());
        if (!u.lengthOk) return true;
        if (!u.crcOk) { if (k.score >= kStrongSync) c.st.crcBad++; return true; }
        c.st.frames++;
        RxFrame f;
        const double startIdx = k.p0 - (8.0 * (kPreambleBytes + 3)) * c.sps - c.W * 0.5;
        f.sec = startIdx / c.r2;
        f.chan = chanIdx;
        f.cfoHz = k.cfo; f.devHz = adev; f.inverted = k.dev < 0;
        const double P = k.pwN ? k.pwSum / (double)k.pwN : 0;
        f.levelDb = (float)(10 * std::log10(P + 1e-20));
        const double n100 = c.noise * 100e3 / c.r2;
        f.snrDb = n100 > 1e-20 ? (float)std::min(60.0, 10 * std::log10(std::max(1e-3, (P - c.noise) / n100))) : 60.f;
        f.crcOk = true;
        f.syncScore = k.score;
        f.payload = std::move(u.payload);
        out.push_back(std::move(f));
        // other candidates inside this frame were its own bits
        const double endPos = k.p0 + (double)k.need * c.sps;
        c.pend.erase(std::remove_if(c.pend.begin(), c.pend.end(), [&](const Cand& o) { return &o != &k && o.p0 > k.p0 && o.p0 < endPos; }), c.pend.end());
        return true;
    }

    void push(Chan& c, const std::vector<cf32>& z) {
        for (const cf32& v : z) {
            const cf32 m = v * std::conj(c.prev);
            c.prev = v;
            const float d = (float)(std::atan2((double)m.imag(), (double)m.real()) * c.r2 / (2 * kPi));
            const float p = std::norm(v);
            c.fr.push_back(d); c.pw.push_back(p);
            c.maSum += d;
            const int64_t idx = c.end() - 1;
            if (idx - c.W >= c.base) c.maSum -= c.at(c.fr, idx - c.W);
            if ((idx & 4095) == 0 && idx - c.W + 1 >= c.base) {   // against rounding drift
                double s = 0;
                for (int64_t j = idx - c.W + 1; j <= idx; j++) s += c.at(c.fr, j);
                c.maSum = s;
            }
            c.mf.push_back((float)(c.maSum / c.W));
            c.blk += p;
            if (++c.blkN == kNoiseBlock) {
                c.blocks[c.blkPos] = c.blk / (double)kNoiseBlock;
                c.blkPos = (c.blkPos + 1) % kNoiseBlocks;
                c.nBlocks = std::min(kNoiseBlocks, c.nBlocks + 1);
                c.noise = *std::min_element(c.blocks.begin(), c.blocks.begin() + (std::ptrdiff_t)c.nBlocks);
                c.blk = 0; c.blkN = 0;
            }
        }
    }

    void trim(Chan& c) {
        int64_t keep = c.scanAt - (int64_t)std::ceil(8.0 * (kPreambleBytes + 4) * c.sps) - c.W - 8;
        if (c.inPeak) keep = std::min(keep, c.bestIdx - c.off[0] - 8);
        for (const Cand& k : c.pend) keep = std::min(keep, (int64_t)std::floor(k.p0 + (double)k.done * c.sps + k.tau - c.sps) - 4);
        if (keep - c.base < (int64_t)1 << 16) return;
        const size_t drop = (size_t)(keep - c.base);
        c.fr.erase(c.fr.begin(), c.fr.begin() + (std::ptrdiff_t)drop);
        c.mf.erase(c.mf.begin(), c.mf.begin() + (std::ptrdiff_t)drop);
        c.pw.erase(c.pw.begin(), c.pw.begin() + (std::ptrdiff_t)drop);
        c.base = keep;
    }
};

Receiver::Receiver() : p_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;

bool Receiver::fits(double rate, double offsetHz) { return rate >= 500e3 && std::fabs(offsetHz) + 100e3 <= 0.45 * rate; }

void Receiver::configure(double rate, const std::vector<double>& offsetsHz) {
    Impl& m = *p_;
    m.rate = rate; m.nIn = 0;
    m.ch.assign(offsetsHz.size(), Chan());
    for (size_t i = 0; i < offsetsHz.size(); i++) { m.ch[i].offHz = offsetsHz[i]; m.setup(m.ch[i]); m.clearStreams(m.ch[i]); }
    m.stats.assign(offsetsHz.size(), Stats());
    for (size_t i = 0; i < m.ch.size(); i++) m.stats[i].on = m.ch[i].on;
}

void Receiver::reset() {
    Impl& m = *p_;
    std::vector<double> offs;
    for (const Chan& c : m.ch) offs.push_back(c.offHz);
    configure(m.rate, offs);
}

void Receiver::feed(const cf32* x, size_t n, std::vector<RxFrame>& out) {
    Impl& m = *p_;
    m.nIn += (int64_t)n;
    for (size_t ci = 0; ci < m.ch.size(); ci++) {
        Chan& c = m.ch[ci];
        if (!c.on) continue;
        if (c.two) {
            const std::vector<cf32>& a = c.d1.process(x, n);
            m.push(c, c.d2.process(a.data(), a.size()));
        } else m.push(c, c.d2.process(x, n));
        m.scan(c);
        for (size_t k = 0; k < c.pend.size();) {
            // advance() may remove later candidates (the frame's own bits); this one stays at k
            if (m.advance(c, c.pend[k], out, ci)) c.pend.erase(c.pend.begin() + (std::ptrdiff_t)k);
            else k++;
        }
        m.trim(c);
        m.stats[ci] = c.st;
        m.stats[ci].on = true;
    }
}

const std::vector<Receiver::Stats>& Receiver::stats() const { return p_->stats; }
double Receiver::timeSec() const { return p_->rate > 0 ? (double)p_->nIn / p_->rate : 0; }

} // namespace fsk
} // namespace lorawan
} // namespace dect2
