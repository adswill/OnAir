// MIL-STD-188-110B/C Appendix C high-rate serial tone (STANAG 4539 waveform): receiver and test generator; see hfu_hdr.h.
// Clean-room from the text of MIL-STD-188-110B Appendix C (C.5.1 to C.5.4).
#include "dect2/hfutil/hfu_hdr.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cf = std::complex<float>;
constexpr double kFs = 24000.0, kBaud = 2400.0, kCarrier = 1800.0;
constexpr int kSps = 10;
constexpr int kData = 256, kProbe = 31, kFrame = kData + kProbe, kBlock = 103, kFramesPerSuper = 72;
constexpr int kSuper = kFramesPerSuper * kData + (kFramesPerSuper - 1) * kProbe + kBlock;   // 20736 symbols, 8.64 s

// C.5.2.1.1: the first 184 symbols of the synchronization preamble
constexpr uint8_t kSync184[184] = {1,5,1,3,6,1,3,1,1,6,3,7,7,3,5,4,3,6,6,4,5,4,0,2,2,2,6,0,7,5,7,4,0,7,5,7,1,6,1,0,5,2,2,6,2,3,6,0,0,5,1,4,2,2,2,3,4,0,6,2,7,
    4,3,3,7,2,0,2,6,4,4,1,7,6,2,0,6,2,3,6,7,4,3,6,1,3,7,4,6,5,7,2,0,1,1,1,4,4,0,0,5,7,7,4,7,3,5,4,1,6,5,6,6,4,6,3,4,3,0,7,1,3,4,7,0,1,4,3,3,3,5,1,1,
    1,4,6,1,0,6,0,1,3,1,4,1,7,7,6,3,0,0,7,2,7,2,0,2,6,1,1,1,2,7,7,5,3,3,6,0,5,3,3,1,0,7,1,1,0,3,0,4,0,7,3};
// C.5.2.2: the '+' mini-probe (repeated Frank-Heimiller sequence)
constexpr uint8_t kProbePlus[31] = {0,0,0,0,0,2,4,6,0,4,0,4,0,6,4,2,0,0,0,0,0,2,4,6,0,4,0,4,0,6,4};
constexpr uint8_t kBarker[13] = {0,4,0,4,0,0,4,4,0,0,0,0,0};   // 0101001100000 as phases

const int kRates[6] = {3200, 4800, 6400, 8000, 9600, 12800};
const int kIntl[6] = {1, 3, 9, 18, 36, 72};
const int kIncr[5][6] = {{97, 229, 805, 1393, 3281, 6985}, {145, 361, 1045, 2089, 5137, 10273}, {189, 481, 1393, 3281, 6985, 11141},
                         {201, 601, 1741, 3481, 8561, 14441}, {229, 805, 2089, 5137, 10273, 17329}};
int rateIdx(int bps) { for (int i = 0; i < 6; i++) if (kRates[i] == bps) return i; return -1; }
int intlIdx(int f) { for (int i = 0; i < 6; i++) if (kIntl[i] == f) return i; return -1; }
const uint8_t kDibit[4] = {0, 2, 6, 4};          // table C-III: 00 0, 01 2, 10 6, 11 4
const uint8_t kTribit[8] = {1, 0, 2, 3, 6, 7, 5, 4};   // table C-IV

cf psk8(int n) { const double a = (n & 7) * kPi / 4; return cf((float)std::cos(a), (float)std::sin(a)); }

const float kQam16[16][2] = {{0.866025f, 0.5f}, {0.5f, 0.866025f}, {1, 0}, {0.258819f, 0.258819f}, {-0.5f, 0.866025f}, {0, 1}, {-0.866025f, 0.5f}, {-0.258819f, 0.258819f},
    {0.5f, -0.866025f}, {0, -1}, {0.866025f, -0.5f}, {0.258819f, -0.258819f}, {-0.866025f, -0.5f}, {-0.5f, -0.866025f}, {-1, 0}, {-0.258819f, -0.258819f}};
const float kQam32[32][2] = {{0.866380f, 0.499386f}, {0.984849f, 0.173415f}, {0.499386f, 0.866380f}, {0.173415f, 0.984849f}, {0.520246f, 0.520246f}, {0.520246f, 0.173415f},
    {0.173415f, 0.520246f}, {0.173415f, 0.173415f}, {-0.866380f, 0.499386f}, {-0.984849f, 0.173415f}, {-0.499386f, 0.866380f}, {-0.173415f, 0.984849f}, {-0.520246f, 0.520246f},
    {-0.520246f, 0.173415f}, {-0.173415f, 0.520246f}, {-0.173415f, 0.173415f}, {0.866380f, -0.499386f}, {0.984849f, -0.173415f}, {0.499386f, -0.866380f}, {0.173415f, -0.984849f},
    {0.520246f, -0.520246f}, {0.520246f, -0.173415f}, {0.173415f, -0.520246f}, {0.173415f, -0.173415f}, {-0.866380f, -0.499386f}, {-0.984849f, -0.173415f}, {-0.499386f, -0.866380f},
    {-0.173415f, -0.984849f}, {-0.520246f, -0.520246f}, {-0.520246f, -0.173415f}, {-0.173415f, -0.520246f}, {-0.173415f, -0.173415f}};
// table C-VII, symbols 0..31; symbol 32 + k is symbol k mirrored (I -> -I) except the four axis points (32: (0,1), 36: (-1,0), 4: (0,-1), 0: (1,0))
const float kQam64a[32][2] = {{1, 0}, {0.822878f, 0.568218f}, {0.821137f, 0.152996f}, {0.932897f, 0.360142f}, {0, -1}, {0.822878f, -0.568218f}, {0.821137f, -0.152996f},
    {0.932897f, -0.360142f}, {0.568218f, 0.822878f}, {0.588429f, 0.588429f}, {0.588429f, 0.117686f}, {0.588429f, 0.353057f}, {0.568218f, -0.822878f}, {0.588429f, -0.588429f},
    {0.588429f, -0.117686f}, {0.588429f, -0.353057f}, {0.152996f, 0.821137f}, {0.117686f, 0.588429f}, {0.117686f, 0.117686f}, {0.117686f, 0.353057f}, {0.152996f, -0.821137f},
    {0.117686f, -0.588429f}, {0.117686f, -0.117686f}, {0.117686f, -0.353057f}, {0.360142f, 0.932897f}, {0.353057f, 0.588429f}, {0.353057f, 0.117686f}, {0.353057f, 0.353057f},
    {0.360142f, -0.932897f}, {0.353057f, -0.588429f}, {0.353057f, -0.117686f}, {0.353057f, -0.353057f}};

cf qamPoint(int bps, int sym) {
    if (bps == 6400) return cf(kQam16[sym & 15][0], kQam16[sym & 15][1]);
    if (bps == 8000) return cf(kQam32[sym & 31][0], kQam32[sym & 31][1]);
    const int k = sym & 63;
    if (k == 32) return cf(0, 1);
    if (k == 36) return cf(-1, 0);
    if (k < 32) return cf(kQam64a[k][0], kQam64a[k][1]);
    return cf(-kQam64a[k - 32][0], kQam64a[k - 32][1]);
}

// the point sent for data label L (bits, the oldest the most significant) with scrambler value s
cf dataPoint(int bps, int label, int s) {
    if (bps == 3200) return psk8(kDibit[label & 3] + s);
    if (bps == 4800) return psk8(kTribit[label & 7] + s);
    return qamPoint(bps, label ^ s);
}

// scrambler (C.5.1.3): x^9 + x^4 + 1, initialised to 1 at the start of every data frame, the value is the rightmost bits, shifted after use
struct ScrTable { std::vector<uint16_t> v[7]; };
const ScrTable& scrTable() {
    static const ScrTable t = [] {
        ScrTable r;
        for (int it = 3; it <= 6; it++) {
            uint32_t s = 1;
            r.v[it].resize(kData);
            for (int i = 0; i < kData; i++) {
                r.v[it][(size_t)i] = (uint16_t)(s & ((1u << it) - 1));
                for (int k = 0; k < it; k++) { const uint32_t fb = ((s >> 8) ^ (s >> 3)) & 1u; s = ((s << 1) | fb) & 0x1FFu; }
            }
        }
        return r;
    }();
    return t;
}
int scrIters(int bps) { return bps <= 4800 ? 3 : bps == 6400 ? 4 : bps == 8000 ? 5 : 6; }

int rate3(int bps) { return rateIdx(bps) + 1; }
int intl3(int frames) { return intlIdx(frames) + 1; }

// the sign (+1 / -1) of mini-probe f (1..72) of a superframe
int probeSign(int f, int bps, int frames) {
    const int j = (f - 1) % 18, set = (f - 1) / 18 + 1;
    if (j < 7) return -1;
    if (j == 7 || j == 17) return 1;
    int bit;
    if (j < 11) bit = (rate3(bps) >> (10 - j)) & 1;          // S0 S1 S2
    else if (j < 14) bit = (intl3(frames) >> (13 - j)) & 1;  // S3 S4 S5
    else bit = (set >> (16 - j)) & 1;                         // S6 S7 S8
    return bit ? -1 : 1;
}

// the 103 known symbols (mini-probe 72, the D block, mini-probe 0) as 8PSK numbers
std::array<uint8_t, kBlock> block103(const int d[3]) {
    std::array<uint8_t, kBlock> b{};
    for (int i = 0; i < 31; i++) b[(size_t)i] = kProbePlus[i];
    b[31] = 2;
    for (int k = 0; k < 3; k++) for (int i = 0; i < 13; i++) b[(size_t)(32 + 13 * k + i)] = (uint8_t)((d[k] + kBarker[i]) & 7);
    b[71] = 6;
    for (int i = 0; i < 31; i++) b[(size_t)(72 + i)] = (uint8_t)((kProbePlus[i] + 4) & 7);
    return b;
}
bool knownFixed(int k) { return k < 32 || k >= 71; }   // positions of the 103 block that do not depend on D

std::vector<float> rrc() {
    const int half = 60;
    const double b = 0.35;
    std::vector<float> h(2 * half + 1);
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double t = (double)i / kSps;
        double v;
        if (i == 0) v = 1 - b + 4 * b / kPi;
        else if (std::fabs(std::fabs(4 * b * t) - 1) < 1e-9) v = b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
        else v = (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / (kPi * t * (1 - 16 * b * b * t * t));
        h[(size_t)(i + half)] = (float)v;
        e += v * v;
    }
    for (auto& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

inline int par(uint32_t v) { return __builtin_parity(v); }
}  // namespace

int hdrBitsPerSymbol(int bps) { switch (bps) { case 3200: return 2; case 4800: return 3; case 6400: return 4; case 8000: return 5; case 9600: case 12800: return 6; default: return 0; } }
bool hdrValid(int bps, int frames) {
    if (bps == 12800) return frames == 1;
    return rateIdx(bps) >= 0 && rateIdx(bps) < 5 && intlIdx(frames) >= 0;
}
int hdrInterleaverBits(int bps, int frames) { return hdrValid(bps, frames) ? kData * hdrBitsPerSymbol(bps) * frames : 0; }
int hdrInputBits(int bps, int frames) { return !hdrValid(bps, frames) ? 0 : bps == 12800 ? hdrInterleaverBits(bps, frames) : hdrInterleaverBits(bps, frames) * 3 / 4; }
int hdrIncrement(int bps, int frames) { return hdrValid(bps, frames) && bps != 12800 ? kIncr[rateIdx(bps)][intlIdx(frames)] : 1; }
const char* hdrInterleaverName(int frames) {
    switch (frames) { case 1: return "ultra short"; case 3: return "very short"; case 9: return "short"; case 18: return "medium"; case 36: return "long"; case 72: return "very long"; default: return "?"; }
}
void hdrDValues(int bps, int frames, int d[3]) {
    const int r = rate3(bps), l = intl3(frames);
    for (int i = 0; i < 3; i++) d[i] = kDibit[(((r >> (2 - i)) & 1) << 1) | ((l >> (2 - i)) & 1)];
}
bool hdrFromD(const int d[3], int& bps, int& frames) {
    for (int ri = 0; ri < 6; ri++) for (int li = 0; li < 6; li++) {
        if (!hdrValid(kRates[ri], kIntl[li])) continue;
        int e[3];
        hdrDValues(kRates[ri], kIntl[li], e);
        if (e[0] == d[0] && e[1] == d[1] && e[2] == d[2]) { bps = kRates[ri]; frames = kIntl[li]; return true; }
    }
    return false;
}
int hdrScrambleValue(int i, int it) { return it >= 3 && it <= 6 && i >= 0 && i < kData ? scrTable().v[it][(size_t)i] : 0; }

std::vector<uint8_t> hdrEncodeBlock(const std::vector<uint8_t>& in, int bps, int frames) {
    const int S = hdrInterleaverBits(bps, frames), K = hdrInputBits(bps, frames);
    if (S == 0 || (int)in.size() != K) return {};
    if (bps == 12800) return in;
    // full tail-biting (C.5.3.2.2): preload six bits, encode the rest, then the six saved bits
    std::vector<uint8_t> coded;
    coded.reserve((size_t)S);
    uint32_t r = 0;
    for (int i = 0; i < 6; i++) r = (r >> 1) | ((uint32_t)in[(size_t)i] << 6);
    for (int k = 0; k < K; k++) {
        const uint8_t u = in[(size_t)((k + 6) % K)];
        r = (r >> 1) | ((uint32_t)u << 6);
        const uint8_t t1 = (uint8_t)par(r & 0b1011011u), t2 = (uint8_t)par(r & 0b1111001u);
        // puncturing 1 1 1 0 0 1 (C.5.3.2.3)
        if (k % 3 == 0) { coded.push_back(t1); coded.push_back(t2); }
        else if (k % 3 == 1) coded.push_back(t1);
        else coded.push_back(t2);
    }
    std::vector<uint8_t> arr((size_t)S, 0);
    const int64_t inc = hdrIncrement(bps, frames);
    for (int64_t n = 0; n < S; n++) arr[(size_t)((n * inc) % S)] = coded[(size_t)n];
    return arr;
}

std::vector<uint8_t> hdrDecodeBlock(const std::vector<float>& soft, int bps, int frames) {
    const int S = hdrInterleaverBits(bps, frames), K = hdrInputBits(bps, frames);
    if (S == 0 || (int)soft.size() != S) return {};
    if (bps == 12800) { std::vector<uint8_t> o((size_t)S); for (int i = 0; i < S; i++) o[(size_t)i] = soft[(size_t)i] < 0 ? 1 : 0; return o; }
    const int64_t inc = hdrIncrement(bps, frames);
    std::vector<float> a((size_t)K, 0.f), b((size_t)K, 0.f);
    int64_t n = 0;
    for (int k = 0; k < K; k++) {
        auto take = [&]() { const float v = soft[(size_t)((n * inc) % S)]; n++; return v; };
        if (k % 3 == 0) { a[(size_t)k] = take(); b[(size_t)k] = take(); }
        else if (k % 3 == 1) a[(size_t)k] = take();
        else b[(size_t)k] = take();
    }
    // tail-biting: run round the circle with a lead-in and a lead-out and keep the middle
    const int W = std::min(K, 120);
    Viterbi v;
    for (int k = K - W; k < K; k++) v.push(a[(size_t)k], b[(size_t)k]);
    for (int k = 0; k < K; k++) v.push(a[(size_t)k], b[(size_t)k]);
    for (int k = 0; k < W; k++) v.push(a[(size_t)k], b[(size_t)k]);
    std::vector<uint8_t> d;
    v.flush(d);
    std::vector<uint8_t> out((size_t)K, 0);
    if ((int)d.size() < K + 2 * W) return {};
    for (int k = 0; k < K; k++) out[(size_t)((k + 6) % K)] = d[(size_t)(W + k)];
    return out;
}

std::vector<uint8_t> hdrBytesToBits(const std::string& s) {
    std::vector<uint8_t> b;
    for (unsigned char c : s) for (int i = 0; i < 8; i++) b.push_back((uint8_t)((c >> i) & 1));
    return b;
}

// ---- generator ---------------------------------------------------------------------------------------------------------------------

std::vector<cf32> genHighRate(const HdrGenCfg& c, const std::vector<uint8_t>& dataBits) {
    if (!hdrValid(c.bps, c.frames)) return {};
    const int bpsym = hdrBitsPerSymbol(c.bps), K = hdrInputBits(c.bps, c.frames);
    std::vector<cf> sy;
    for (int n = 0; n < std::clamp(c.agcBlocks, 0, 7); n++) for (int i = 0; i < 184; i++) sy.push_back(std::conj(psk8(kSync184[i])));
    for (int i = 0; i < 184; i++) sy.push_back(psk8(kSync184[i]));
    int d[3];
    hdrDValues(c.bps, c.frames, d);
    const auto blk = block103(d);
    for (uint8_t s : blk) sy.push_back(psk8(s));
    // the message, the end-of-message pattern, zeros to the end of the interleaver block
    std::vector<uint8_t> bits = dataBits;
    if (c.eom) for (int i = 31; i >= 0; i--) bits.push_back((uint8_t)((kHdrEom >> i) & 1));
    while (bits.empty() || bits.size() % (size_t)K) bits.push_back(0);
    const ScrTable& st = scrTable();
    const int it = scrIters(c.bps);
    int f = 1;   // frame number in the superframe
    for (size_t b0 = 0; b0 < bits.size(); b0 += (size_t)K) {
        const std::vector<uint8_t> in(bits.begin() + (long)b0, bits.begin() + (long)b0 + K);
        const std::vector<uint8_t> tx = hdrEncodeBlock(in, c.bps, c.frames);
        const bool lastBlock = b0 + (size_t)K >= bits.size();
        for (int fr = 0; fr < c.frames; fr++) {
            for (int i = 0; i < kData; i++) {
                int label = 0;
                for (int k = 0; k < bpsym; k++) label = (label << 1) | tx[(size_t)((fr * kData + i) * bpsym + k)];
                sy.push_back(dataPoint(c.bps, label, st.v[it][(size_t)i]));
            }
            const bool last = lastBlock && fr == c.frames - 1;
            if (f < kFramesPerSuper || last) {
                const int sg = f < kFramesPerSuper ? probeSign(f, c.bps, c.frames) : 1;
                for (int i = 0; i < kProbe; i++) sy.push_back(psk8(kProbePlus[i] + (sg < 0 ? 4 : 0)));
            } else {
                for (uint8_t s : blk) sy.push_back(psk8(s));   // mini-probe 72, the reinserted preamble, mini-probe 0
            }
            f = f % kFramesPerSuper + 1;
        }
    }
    // pulse shaping at 24 kHz and the 1800 Hz sub-carrier
    const std::vector<float> h = rrc();
    const int half = (int)h.size() / 2;
    const size_t lead = (size_t)(c.leadSec * kFs), tail = (size_t)(c.tailSec * kFs);
    std::vector<cf32> out(lead + sy.size() * kSps + h.size() + tail, cf32(0, 0));
    for (size_t s = 0; s < sy.size(); s++) {
        const size_t c0 = lead + s * kSps;
        for (size_t k = 0; k < h.size(); k++) out[c0 + k] += sy[s] * h[k];
    }
    (void)half;
    double ph = 0;
    const double w = 2 * kPi * kCarrier / kFs;
    for (size_t i = 0; i < out.size(); i++) {
        out[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)) * 3.16f;   // about the level of a full-scale 8PSK carrier
        ph += w;
        if (ph > kPi) ph -= 2 * kPi;
    }
    return out;
}

// ---- receiver ----------------------------------------------------------------------------------------------------------------------

struct HighRateRx::Impl {
    std::vector<float> h = rrc();
    std::vector<cf> hist;            // FIR input history
    size_t hpos = 0;
    double nco = 0;
    std::vector<cf> y;               // matched-filter output at 24 kHz
    int64_t y0 = 0, nY = 0;          // absolute index of y[0], samples produced
    // acquisition template: the differential products of the 103 block
    std::vector<int> tk;             // positions k (the product y[k] conj(y[k - 1]))
    std::vector<cf> te;              // the expected product, conjugated
    int64_t scan = 0, lockScan = 0;
    uint64_t goodProbes = 0;
    double candM = 0; int64_t candT = -1; cf candC;
    // lock
    int state = 0;
    double t0 = 0, sps = kSps, cfo = 0;   // t0: sample position of symbol 0 of the 103 block
    int bps = 0, frames = 0, frame = 1, superN = 0, badProbes = 0;
    cf hPrev; double hPrevPos = 0;
    double noise = 1e-3, sig = 1;
    double phRef = 0, phPos = 0;     // the frequency correction's phase at phPos: a change of frequency keeps the phase continuous
    struct PhSeg { double pos, ph, f; };
    std::vector<PhSeg> phHist;       // the corrections before phPos
    // decision-feedback equaliser: a 16-tap channel estimate from every mini-probe and reinserted preamble (the probes are a periodic
    // Frank sequence, so the estimate is exact over 16 symbols), interpolated between the probes, and the MMSE feed-forward and feedback
    // filters computed from it every 16 symbols
    static constexpr int kCirN = 16;                             // channel taps cirL0 .. cirL0 + 15 (in symbols)
        static constexpr int kFfLo = -8, kFfHi = 12, kNff = kFfHi - kFfLo + 1, kMaxFb = 48;
    int cirL0 = -6;                         // the first lag of the channel window, placed where the delay profile has least power
    double profC[16] = {};                  // the averaged power at each lag, modulo 16 (the estimate is cyclic)
    int l0Votes = 0;
    static constexpr int kEqSeg = 16;
    bool useEq = true;
    struct ProbeEst { double c = 0; int64_t p0 = 0; cf h[kCirN]; bool ok = false; double e = 0; };   // e: the energy of the taps   // c: the centre, in symbols from the lock block
    std::vector<ProbeEst> prs;
    int64_t frameK = 1, probedFrames = 0;   // the next frame to read (from the lock), frames whose following probes are estimated
    bool syncBefore = false;                // the 184 sync symbols precede the lock block
    struct SuperInfo { int bps = 0, frames = 0; std::array<uint8_t, kBlock> blk{}; };
    std::vector<SuperInfo> supers;          // per superframe since the lock: the rate and the 103 known symbols
    float inScale = 1;
    double eqNoise = 0.05;                  // noise per sample, relative to the signal at the lock
    std::vector<float> soft;
    std::vector<uint8_t> bitHist;    // for the EOM search
    uint32_t eomWin = 0;
    std::vector<uint8_t> pendingBits;   // decoded bits held back 32 bits (EOM)
    uint8_t curByte = 0; int curBits = 0;
    Parser5066 parser;
    HdrStatus st;
    std::function<void(uint8_t)> byteCb;
    std::function<void(const SerialEvent&)> evCb;
    std::function<void(const D5066&)> pduCb;

    Impl() {
        hist.assign(h.size(), cf(0, 0));
        int d0[3] = {0, 0, 0};
        const auto b = block103(d0);
        for (int k = 1; k < kBlock; k++) {
            const bool sameBarker = k >= 33 && k <= 70 && (k - 32) % 13 != 0;
            if (!(knownFixed(k) && knownFixed(k - 1)) && !sameBarker) continue;
            tk.push_back(k);
            te.push_back(std::conj(psk8(b[(size_t)k]) * std::conj(psk8(b[(size_t)k - 1]))));
        }
    }

    void event(const std::string& s) { if (evCb) { SerialEvent e; e.text = s; e.timeSec = (double)nY / kFs; evCb(e); } }

    cf at(double pos) const {   // interpolated sample at an absolute position, with the frequency error removed
        const int64_t i = (int64_t)std::floor(pos);
        const float fr = (float)(pos - (double)i);
        const int64_t a = i - y0;
        if (a < 0 || a + 1 >= (int64_t)y.size()) return cf(0, 0);
        const cf v = y[(size_t)a] * (1 - fr) + y[(size_t)a + 1] * fr;
        double ph;
        if (pos >= phPos || phHist.empty()) ph = phRef - 2 * kPi * cfo * (pos - phPos) / kFs;
        else {   // before the last change of frequency: the correction that was in force there
            size_t k = phHist.size();
            while (k > 1 && phHist[k - 1].pos > pos) k--;
            const PhSeg& g = phHist[k - 1];
            ph = g.ph - 2 * kPi * g.f * (pos - g.pos) / kFs;
        }
        return v * cf((float)std::cos(ph), (float)std::sin(ph));
    }
    // a new frequency from sample pos on; the phase stays continuous and earlier samples keep the correction they had
    void setCfo(double f, double pos) {
        phHist.push_back({phPos, phRef, cfo});
        if (phHist.size() > 64) phHist.erase(phHist.begin());
        phRef = std::remainder(phRef - 2 * kPi * cfo * (pos - phPos) / kFs, 2 * kPi);
        phPos = pos;
        cfo = f;
    }
    double posOf(int64_t m) const { return t0 + (double)m * sps; }
    cf symAt(int64_t m) const { return at(posOf(m)); }

    void push(const cf32* x, size_t n) {
        const double w = -2 * kPi * kCarrier / kFs;
        const size_t L = h.size();
        for (size_t i = 0; i < n; i++) {
            hist[hpos] = cf(x[i]) * cf((float)std::cos(nco), (float)std::sin(nco));
            nco += w;
            if (nco < -kPi) nco += 2 * kPi;
            cf acc(0, 0);
            size_t j = hpos;
            for (size_t k = 0; k < L; k++) { acc += hist[j] * h[k]; j = j == 0 ? L - 1 : j - 1; }
            hpos = (hpos + 1) % L;
            y.push_back(acc);
            nY++;
        }
        work();
        // keep about two superframes' worth behind the read point
        int64_t keepFrom = nY - 60000;
        if (state == 2) keepFrom = std::min<int64_t>(keepFrom, (int64_t)hPrevPos - 4000);
        else keepFrom = std::min<int64_t>(keepFrom, scan - 4000);
        if (keepFrom > y0 + 48000) { y.erase(y.begin(), y.begin() + (long)(keepFrom - y0)); y0 = keepFrom; }
    }

    void work() {
        for (;;) {
            if (state == 0) { if (!acquire()) return; }
            else if (!track()) return;
        }
    }

    // differential correlation with the 103 block at every sample position
    bool acquire() {
        const int64_t span = (int64_t)(kBlock * kSps) + 4;
        if (scan < y0 + kSps) scan = y0 + kSps;
        while (scan + span < nY) {
            cf c(0, 0);
            float nrm = 0, q[4] = {0, 0, 0, 0};
            const int64_t base = scan - y0;
            for (size_t i = 0; i < tk.size(); i++) {
                const int64_t a = base + (int64_t)tk[i] * kSps;
                const cf p = y[(size_t)a] * std::conj(y[(size_t)(a - kSps)]);
                c += p * te[i];
                nrm += std::abs(p);
                q[std::min(3, tk[i] / 26)] += std::abs(p);
            }
            const float qmax = std::max(std::max(q[0], q[1]), std::max(q[2], q[3])), qmin = std::min(std::min(q[0], q[1]), std::min(q[2], q[3]));
            const double m = nrm > 0 && qmax < 2 * qmin ? std::abs(c) / nrm : 0;   // uneven quarters: the start or end of a signal, not the block
            if (m > (useEq ? 0.3 : 0.5) && m > candM) { candM = m; candT = scan; candC = c; }   // two equal paths halve the match
            if (candT >= 0 && scan > candT + 110 * kSps) {   // the highest within a block length: partly overlapping windows already pass the threshold
                const int64_t t = candT;
                const cf cc = candC;
                candM = 0; candT = -1;
                scan = t + kSps;
                lockScan = t + kSps;
                if (lockAt(t, std::arg(cc) * kBaud / (2 * kPi))) return true;
                continue;
            }
            scan++;
        }
        return false;
    }

    // coherent fit of the D-independent symbols of the 103 block starting at position t
    cf fitFixed(double t, double f, int lo, int hi) const {
        Impl* self = const_cast<Impl*>(this);
        const double t0s = t0, cs = cfo;
        self->t0 = t; self->cfo = f;
        int d0[3] = {0, 0, 0};
        const auto b = block103(d0);
        cf acc(0, 0);
        for (int k = lo; k < hi; k++) if (knownFixed(k)) acc += symAt(k) * std::conj(psk8(b[(size_t)k]));
        self->t0 = t0s; self->cfo = cs;
        return acc;
    }

    bool lockAt(int64_t t, double f0) {
        phRef = 0; phPos = 0; phHist.clear();
        // fine timing (tenth of a sample) and frequency (the phase slope between the two mini-probes)
        double bestT = (double)t, bestE = -1;
        for (double dt = -1.0; dt <= 1.0001; dt += 0.1) {
            const double e = std::abs(fitFixed((double)t + dt, f0, 0, kBlock));
            if (e > bestE) { bestE = e; bestT = (double)t + dt; }
        }
        double f = f0;
        if (useEq) {
            // with multipath the differential phase is biased (the probes are periodic): search the frequency on the channel itself
            f = searchFreq((double)t);
            bestE = -1;
            for (double dt = -1.0; dt <= 1.0001; dt += 0.1) {
                const double e = std::abs(fitFixed((double)t + dt, f, 0, kBlock));
                if (e > bestE) { bestE = e; bestT = (double)t + dt; }
            }
        } else {
            for (int pass = 0; pass < 2; pass++) {
                const cf a = fitFixed(bestT, f, 0, 32), b = fitFixed(bestT, f, 71, kBlock);
                f += std::arg(b * std::conj(a)) * kBaud / (2 * kPi * 71.0);
            }
        }
        t0 = bestT; cfo = f; sps = kSps;
        int dv[3];
        cf hh;
        double snr;
        if (!readD(dv, hh, snr) && (!useEq || snr < -8)) return false;   // with multipath the other paths count as noise in this fit
        if (useEq && !eqAcquire(dv, hh)) return false;
        int r = 0, fr = 0;
        if (!hdrFromD(dv, r, fr)) { char b[80]; snprintf(b, sizeof b, "preamble with an unknown rate code D = %d %d %d", dv[0], dv[1], dv[2]); event(b); return false; }
        // was this the start of a transmission (the 184 sync symbols just before) or a reinserted preamble?
        cf s(0, 0);
        float e = 0;
        for (int k = 0; k < 184; k++) { const cf v = symAt(k - 184); s += v * std::conj(psk8(kSync184[k])); e += std::abs(v); }
        const bool start = e > 0 && std::abs(s) / e > (useEq ? 0.35 : 0.5);
        bps = r; frames = fr; frame = 1; superN = 0; badProbes = 0; goodProbes = 0;
        if (useEq) eqStart(start, dv, hh);
        hPrev = hh; hPrevPos = posOf(kBlock - 16);
        soft.clear();
        curByte = 0; curBits = 0; eomWin = 0; pendingBits.clear();
        state = 2;
        st.state = 2; st.bps = bps; st.frames = frames; st.cfoHz = cfo; st.snrDb = snr; st.messages++;
        char m[96];
        snprintf(m, sizeof m, "%d bit/s, %s interleaver (%d frame%s)", bps, bps == 12800 ? "no" : hdrInterleaverName(frames), frames, frames == 1 ? "" : "s");
        st.mode = m;
        char b[200];
        snprintf(b, sizeof b, "%s: %s, frequency error %+.1f Hz, SNR %.0f dB", start ? "transmission start" : "joined at a reinserted preamble", m, cfo, snr);
        event(b);
        return true;
    }

    // D0 D1 D2 from the 103 block at t0 (and the channel there)
    bool readD(int dv[3], cf& hh, double& snr) {
        const cf a = fitFixed(t0, cfo, 0, kBlock);
        int d0[3] = {0, 0, 0};
        const auto b = block103(d0);
        int nk = 0;
        double er = 0, pw = 0;
        hh = a / 64.0f;
        if (std::abs(hh) < 1e-9f) return false;
        for (int k = 0; k < kBlock; k++) if (knownFixed(k)) { const cf v = symAt(k); er += std::norm(v - hh * psk8(b[(size_t)k])); pw += std::norm(v); nk++; }
        for (int i = 0; i < 3; i++) {
            cf z(0, 0);
            for (int k = 0; k < 13; k++) z += symAt(32 + 13 * i + k) * std::conj(psk8(kBarker[k]));
            z /= hh;
            int q = (int)std::lround(std::arg(z) / (kPi / 2));
            dv[i] = ((q % 4 + 4) % 4) * 2;
        }
        const double ns = er / std::max(1, nk), sp = std::norm(hh);
        noise = std::max(ns, 1e-9); sig = sp;
        snr = 10 * std::log10(sp / std::max(ns, 1e-12));
        (void)pw;
        return snr > 0;   // a side peak or noise does not fit the known symbols
    }

    // the channel at lags lo .. hi samples from n known symbols starting at symbol m0
    void cirAt(int64_t m0, const uint8_t* sym, int n, int lo, int hi, std::vector<cf>& c, int step = 1) const {
        c.assign((size_t)((hi - lo) / step + 1), cf(0, 0));
        for (int l = lo, j = 0; l <= hi; l += step, j++) {
            cf a(0, 0);
            for (int i = 0; i < n; i++) a += at(posOf(m0 + i) + l) * std::conj(psk8(sym[i]));
            c[(size_t)j] = a / (float)n;
        }
    }
    // the frequency error at a 103-symbol block at sample t: the two mini-probes' channels (lags -5 T .. +6 T) added coherently, over
    // -96 .. +96 Hz; then the phase change between them (72 symbols apart). With multipath the differential phase used to find the block
    // is biased (the probes are periodic), so the frequency is searched on the channel itself.
    double searchFreq(double t) {
        const double t0s = t0, cs = cfo, pr = phRef, pp0 = phPos;
        uint8_t pp[31], pm[31];
        for (int i = 0; i < 31; i++) { pp[i] = kProbePlus[i]; pm[i] = (uint8_t)((kProbePlus[i] + 4) & 7); }
        t0 = t; phRef = 0; phPos = t;
        std::vector<cf> c1, c2;
        auto eval = [&](double f, cf& cross) {
            cfo = f;
            cirAt(0, pp, 31, -50, 60, c1, 5);
            cirAt(72, pm, 31, -50, 60, c2, 5);
            double e = 0;
            cross = cf(0, 0);
            for (size_t l = 0; l < c1.size(); l++) { e += std::norm(c1[l] + c2[l]); cross += c2[l] * std::conj(c1[l]); }
            return e;
        };
        double best = -1, fb = 0;
        cf cr;
        for (double f = -96; f <= 96.001; f += 3) { const double e = eval(f, cr); if (e > best) { best = e; fb = f; } }
        for (int pass = 0; pass < 2; pass++) { eval(fb, cr); fb += std::arg(cr) * kBaud / (2 * kPi * 72.0); }
        t0 = t0s; cfo = cs; phRef = pr; phPos = pp0;
        return fb;
    }

    // ---- decision-feedback equaliser ------------------------------------------------------------------------------------------------
    int64_t dataStartOf(int64_t k) const { const int64_t sIdx = (k - 1) / kFramesPerSuper, f = (k - 1) % kFramesPerSuper + 1; return sIdx * kSuper + kBlock + (f - 1) * kFrame; }
    // the 8-PSK number of a known symbol (sync, the 103 block, a mini-probe), or -1 for data
    int symKnown(int64_t m) const {
        if (m < 0) return syncBefore && m >= -184 ? kSync184[m + 184] : -1;
        const int64_t sIdx = m / kSuper, r = m % kSuper;
        if (sIdx >= (int64_t)supers.size()) return -1;
        const SuperInfo& si = supers[(size_t)sIdx];
        if (r < kBlock) return si.blk[(size_t)r];
        const int64_t q = (r - kBlock) % kFrame, f = (r - kBlock) / kFrame + 1;
        if (q < kData) return -1;
        return (kProbePlus[q - kData] + (probeSign((int)f, si.bps, si.frames) < 0 ? 4 : 0)) & 7;
    }
    cf sampleAt(int64_t m) const { return at(posOf(m)) * inScale; }

    // the channel from the 31 known symbols starting at symbol p0: h(l) = 1/16 sum y(p0 + m) s*(m - l) over 16 consecutive m, for the
    // 16 lags l = L0 .. L0 + 15 (m = L0 + 15 .. L0 + 30, so that every s(m - l) is a probe symbol; the Frank sequence repeats every 16)
    ProbeEst estimate(int64_t p0) const {
        ProbeEst e;
        e.p0 = p0;
        const int L0 = cirL0;
        e.c = (double)p0 + (double)L0 + 22.5;
        int sy[31];
        for (int i = 0; i < 31; i++) sy[i] = symKnown(p0 + i);
        cf yv[16];
        for (int m = L0 + 15; m <= L0 + 30; m++) yv[m - L0 - 15] = sampleAt(p0 + m);
        for (int l = 0; l < kCirN; l++) {
            cf a(0, 0);
            for (int m = L0 + 15; m <= L0 + 30; m++) a += yv[m - L0 - 15] * std::conj(psk8(sy[m - (l + L0)]));
            e.h[l] = a / 16.0f;
            e.e += std::norm(e.h[l]);
        }
        return e;
    }

    // the channel at symbol m: Lagrange interpolation over up to two good probes on each side
    void interpH(double m, cf* out) const {
        int idx[4], n = 0;
        int after = -1;
        for (size_t i = 0; i < prs.size(); i++) if (prs[i].c > m) { after = (int)i; break; }
        const int start = after < 0 ? (int)prs.size() : after;
        // the nearest probes, also those in a fade (weak and noisy, but nearer than any other); none further than two intervals away
        auto usable = [&](int i) { return prs[(size_t)i].e > 1e-9 && std::fabs(prs[(size_t)i].c - m) < 2.5 * kFrame; };
        for (int i = start - 1, c = 0; i >= 0 && c < 2; i--) if (usable(i)) { idx[n++] = i; c++; }
        if (after >= 0) for (int i = after, c = 0; i < (int)prs.size() && c < 2; i++) if (usable(i)) { idx[n++] = i; c++; }
        for (int l = 0; l < kCirN; l++) out[l] = cf(0, 0);
        if (n == 0) return;
        if (n == 1) { for (int l = 0; l < kCirN; l++) out[l] = prs[(size_t)idx[0]].h[l]; return; }
        if (n == 2 && (prs[(size_t)idx[0]].c < m) == (prs[(size_t)idx[1]].c < m)) {   // both on one side: hold the nearer one
            const int closest = std::fabs(prs[(size_t)idx[0]].c - m) < std::fabs(prs[(size_t)idx[1]].c - m) ? idx[0] : idx[1];
            for (int l = 0; l < kCirN; l++) out[l] = prs[(size_t)closest].h[l];
            return;
        }
        // the probes in time order; the mean rotation between them is taken out before the interpolation (a frequency error, the
        // Doppler of the paths) and the channel is then read at m
        std::sort(idx, idx + n);
        double om = 0, ow = 0;
        for (int a = 0; a + 1 < n; a++) {
            const ProbeEst& p = prs[(size_t)idx[a]];
            const ProbeEst& q = prs[(size_t)idx[a + 1]];
            cf c(0, 0);
            for (int l = 0; l < kCirN; l++) c += q.h[l] * std::conj(p.h[l]);
            const double w = std::abs(c);
            om += w * std::arg(c) / (q.c - p.c);
            ow += w;
        }
        om = ow > 0 ? om / ow : 0;   // radians per symbol
        for (int a = 0; a < n; a++) {
            double wgt = 1;
            for (int b = 0; b < n; b++) if (b != a) wgt *= (m - prs[(size_t)idx[b]].c) / (prs[(size_t)idx[a]].c - prs[(size_t)idx[b]].c);
            const double ph = om * (m - prs[(size_t)idx[a]].c);
            const cf r = cf((float)std::cos(ph), (float)std::sin(ph)) * (float)wgt;
            for (int l = 0; l < kCirN; l++) out[l] += prs[(size_t)idx[a]].h[l] * r;
        }
    }

    // MMSE decision-feedback filters for the channel h: z = (w^H u - sum b_j s(m - j)) / g, u_k = y(m + k)
    // The reference is the first path (the first tap within 10 dB of the strongest): returned in l1, the filters' window starts there.
    void mmse(const cf* h, std::complex<double>* w, std::complex<double>* b, int& nfb, double& g, int& l1) const {
        double mx = 0;
        for (int l = 0; l < kCirN; l++) mx = std::max<double>(mx, std::norm(h[l]));
        const int L0 = cirL0;
        l1 = L0;
        for (int l = 0; l < kCirN; l++) if (std::norm(h[l]) >= 0.1 * mx) { l1 = l + L0; break; }
        // taps relative to the reference l1
        auto tap = [&](int l) -> std::complex<double> { l += l1; return l >= L0 && l < L0 + kCirN ? std::complex<double>(h[l - L0]) : 0.0; };
        const int lMin = L0 - l1, lMax = L0 + kCirN - 1 - l1;   // the support of tap()
        nfb = std::clamp(lMax - kFfLo, 0, kMaxFb);   // y(m + kFfLo) still holds s(m + kFfLo - lMax)
        static constexpr int N = kNff;
        std::complex<double> R[N][N];
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) R[i][j] = 0;
        // the current and later symbols (d >= 0) stay in u; the earlier ones are cancelled by the feedback
        for (int d = 0; d <= kFfHi - lMin; d++) {
            std::complex<double> c[N];
            for (int k = 0; k < N; k++) c[k] = tap(kFfLo + k - d);
            for (int i = 0; i < N; i++) if (c[i] != 0.0) for (int j = 0; j < N; j++) R[i][j] += c[i] * std::conj(c[j]);
        }
        for (int i = 0; i < N; i++) R[i][i] += std::max(eqNoise, 1e-4);
        std::complex<double> c0[N];
        for (int k = 0; k < N; k++) c0[k] = tap(kFfLo + k);
        // Cholesky R = L L^H, then w = R^-1 c0
        std::complex<double> L[N][N];
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) L[i][j] = 0;
        for (int j = 0; j < N; j++) {
            std::complex<double> sum = R[j][j];
            for (int k = 0; k < j; k++) sum -= L[j][k] * std::conj(L[j][k]);
            const double dj = std::sqrt(std::max(sum.real(), 1e-12));
            L[j][j] = dj;
            for (int i = j + 1; i < N; i++) {
                std::complex<double> v = R[i][j];
                for (int k = 0; k < j; k++) v -= L[i][k] * std::conj(L[j][k]);
                L[i][j] = v / dj;
            }
        }
        std::complex<double> yv[N];
        for (int i = 0; i < N; i++) { std::complex<double> v = c0[i]; for (int k = 0; k < i; k++) v -= L[i][k] * yv[k]; yv[i] = v / L[i][i]; }
        for (int i = N - 1; i >= 0; i--) { std::complex<double> v = yv[i]; for (int k = i + 1; k < N; k++) v -= std::conj(L[k][i]) * w[k]; w[i] = v / std::conj(L[i][i]); }
        std::complex<double> gg = 0;
        for (int k = 0; k < N; k++) gg += std::conj(w[k]) * c0[k];
        g = std::max(gg.real(), 1e-6);
        for (int j = 1; j <= nfb; j++) {
            std::complex<double> v = 0;
            for (int k = 0; k < N; k++) v += std::conj(w[k]) * tap(kFfLo + k + j);
            b[j - 1] = v;
        }
    }

    // equalise symbol m with the filters; prev(j) gives s(m - j) (decided or known)
    template <class Prev> cf eqSymbol(int64_t m, int l1, const std::complex<double>* w, const std::complex<double>* b, int nfb, double g, Prev prev) const {
        std::complex<double> z = 0;
        for (int k = 0; k < kNff; k++) z += std::conj(w[k]) * std::complex<double>(sampleAt(m + l1 + kFfLo + k));
        for (int j = 1; j <= nfb; j++) z -= b[j - 1] * std::complex<double>(prev(j));
        z /= g;
        return cf((float)z.real(), (float)z.imag());
    }

    // a new channel estimate: its quality (the correlation with the one before), the frequency error and the sample clock
    void addProbe(ProbeEst e) {
        double ea = 0;
        for (int l = 0; l < kCirN; l++) ea += std::norm(e.h[l]);
        e.ok = ea > 1e-6;
        if (!prs.empty() && e.ok) {
            const ProbeEst& p = prs.back();
            cf a(0, 0);
            double eb = 0;
            for (int l = 0; l < kCirN; l++) { a += e.h[l] * std::conj(p.h[l]); eb += std::norm(p.h[l]); }
            const double rho = eb > 0 ? std::abs(a) / std::sqrt(ea * eb) : 0;
            // a probe holds the signal when its taps carry clearly more than the noise (the shape may change fast in a fade);
            // after a gap, it must also look like the last good one
            e.ok = ea > 4 * std::max(eqNoise, 1e-4) && rho > 0.3;
            if (e.ok && p.ok) {
                const double dt = (e.c - p.c) * sps / kFs;
                if (dt > 0) setCfo(cfo + 0.15 * std::arg(a) / (2 * kPi * dt), posOf((int64_t)e.c));
            }
        }
        if (e.ok) {
            // where the window should start: just after the lag (modulo 16) with the least averaged power
            for (int l = 0; l < kCirN; l++) { const int c = (((l + cirL0) % 16) + 16) % 16; profC[c] = (goodEst == 0 ? 0.0 : 0.9 * profC[c]) + (goodEst == 0 ? 1.0 : 0.1) * std::norm(e.h[l]) / ea; }
            // the cut goes in the middle of the longest run of lags (modulo 16) with little power, so that no path is split
            double mxp = 0;
            for (double v : profC) mxp = std::max(mxp, v);
            int runStart = -1, runLen = 0;
            for (int st0 = 0; st0 < 16; st0++) {
                if (profC[(st0 + 15) % 16] < 0.03 * mxp || profC[st0] >= 0.03 * mxp) continue;   // a run starts here
                int len = 0;
                while (len < 16 && profC[(st0 + len) % 16] < 0.03 * mxp) len++;
                if (len > runLen) { runLen = len; runStart = st0; }
            }
            int bestL = cirL0;
            if (runStart >= 0 && runLen < 16) {
                const int cut = (runStart + (runLen - 1) / 2) % 16;   // the last lag before the window
                bestL = ((cut + 1) % 16) - 16;                      // as a lag in -16 .. -1
                if (bestL < -12) bestL += 16;                      // ... or 0 .. 3
            }
            l0Votes = bestL != cirL0 ? l0Votes + 1 : 0;
            if (l0Votes >= 2 || (goodEst == 0 && bestL != cirL0)) {
                // move the window and estimate the stored probes again (their samples are still here)
                cirL0 = bestL;
                l0Votes = 0;
                for (auto& q : prs) { const bool ok = q.ok; q = estimate(q.p0); q.ok = ok; }
                const bool ok = e.ok;
                e = estimate(e.p0);
                e.ok = ok;
                ea = 0;
                for (int l = 0; l < kCirN; l++) ea += std::norm(e.h[l]);
                goodEst = std::min<int64_t>(goodEst, 1);
            }
            goodEst++;
        }
        prs.push_back(e);
        if (prs.size() > 12) prs.erase(prs.begin());
        // the noise: the probe two back, against the cubic through its neighbours (that is also the interpolation error of the data)
        if (prs.size() >= 5) {
            const ProbeEst& q = prs[prs.size() - 3];
            bool all = q.ok;
            for (size_t i = prs.size() - 5; i < prs.size(); i++) all = all && prs[i].ok;
            if (all) {
                double num = 0, den = 0;
                for (int l = 0; l < kCirN; l++) {
                    cf pr(0, 0);
                    const size_t ids[4] = {prs.size() - 5, prs.size() - 4, prs.size() - 2, prs.size() - 1};
                    for (int a = 0; a < 4; a++) {
                        double wgt = 1;
                        for (int b2 = 0; b2 < 4; b2++) if (b2 != a) wgt *= (q.c - prs[ids[b2]].c) / (prs[ids[a]].c - prs[ids[b2]].c);
                        pr += prs[ids[a]].h[l] * (float)wgt;
                    }
                    num += std::norm(pr - q.h[l]);
                    den += std::norm(q.h[l]);
                }
                // a tap estimate carries 1/16 of the noise per sample, and the cubic of the neighbours about the same again
                const double nz = num * 16.0 / (kCirN * 2.0);
                const double al = noiseUpdates++ < 5 ? 0.5 : 0.2;
                cvNoise = (1 - al) * cvNoise + al * std::clamp(nz, 1e-5, 2.0);
                (void)den;
            }
        }
    }
    int64_t goodEst = 0;
    int noiseUpdates = 0;
    int blockBad = 0;        // frames of the current interleaver block whose probe held no signal
    double cvNoise = 0.05;   // the noise from the probes (with the channel's change over two probe intervals: high in fast fading)
    double ddNoise = -1;     // the error after the equaliser, on the decisions

    // the probes after frame k (a mini-probe, or after frame 72 the next 103 block and its D values)
    bool probesAfter(int64_t k) {
        const int64_t sIdx = (k - 1) / kFramesPerSuper, f = (k - 1) % kFramesPerSuper + 1;
        if (f < kFramesPerSuper) {
            const int64_t p0 = dataStartOf(k) + kData;
            if (posOf(p0 + 31 + 12) + 4 >= (double)nY) return false;
            addProbe(estimate(p0));
            return true;
        }
        const int64_t bs = (sIdx + 1) * kSuper;
        if (posOf(bs + kBlock + 12) + 4 >= (double)nY) return false;
        if ((int64_t)supers.size() <= sIdx + 1) {
            SuperInfo ni = supers[(size_t)sIdx];
            int d0[3] = {0, 0, 0};
            ni.blk = block103(d0);
            for (int k2 = 32; k2 < 71; k2++) ni.blk[(size_t)k2] = 0;
            supers.push_back(ni);
        }
        addProbe(estimate(bs));
        addProbe(estimate(bs + 72));
        SuperInfo& ni = supers[(size_t)sIdx + 1];
        int dv[3];
        readDEq(bs, ni.blk.data(), dv);
        int r = 0, fr = 0;
        if (hdrFromD(dv, r, fr)) { ni.bps = r; ni.frames = fr; }
        int dd[3];
        hdrDValues(ni.bps, ni.frames, dd);
        ni.blk = block103(dd);
        return true;
    }

    // D0 D1 D2 of the 103 block at symbol bs, equalised with the channel of its probes (in prs); decisions on the four even phases.
    // blk: the block's symbols (the D part is not used)
    void readDEq(int64_t bs, const uint8_t* blk, int dv[3]) const {
        std::complex<double> w[kNff], b[kMaxFb];
        double g = 1;
        int l1 = 0, nfb = 0;
        cf hh[kCirN];
        cf zd[39];
        int dec[39];
        for (int k2 = 32; k2 < 71; k2++) {
            if ((k2 - 32) % 13 == 0) { interpH((double)(bs + k2 + 6), hh); mmse(hh, w, b, nfb, g, l1); }
            const cf z = eqSymbol(bs + k2, l1, w, b, nfb, g, [&](int j) { const int r = k2 - j; return r >= 32 ? psk8(dec[r - 32]) : psk8(blk[r]); });
            const int q = (int)std::lround(std::arg(z) / (kPi / 2));
            dec[k2 - 32] = ((q % 4 + 4) % 4) * 2;
            zd[k2 - 32] = z;
        }
        for (int i = 0; i < 3; i++) {
            cf a(0, 0);
            for (int k2 = 0; k2 < 13; k2++) a += zd[13 * i + k2] * std::conj(psk8(kBarker[k2]));
            const int q = (int)std::lround(std::arg(a) / (kPi / 2));
            dv[i] = ((q % 4 + 4) % 4) * 2;
        }
    }

    // at an acquisition: the channel of the block's two probes (they must agree: else it was data that looked like the block), and
    // D0 D1 D2 through the equaliser
    bool eqAcquire(int dv[3], cf hh) {
        inScale = 1.0f / std::max(std::abs(hh), 1e-6f);
        prs.clear(); supers.clear();
        cirL0 = -6;
        syncBefore = false;
        SuperInfo si;
        int d0[3] = {0, 0, 0};
        si.blk = block103(d0);
        supers.push_back(si);
        ProbeEst a = estimate(0), b2 = estimate(72);
        double ea = 0, eb = 0;
        cf c(0, 0);
        for (int l = 0; l < kCirN; l++) { ea += std::norm(a.h[l]); eb += std::norm(b2.h[l]); c += b2.h[l] * std::conj(a.h[l]); }
        const double rho = ea > 0 && eb > 0 ? std::abs(c) / std::sqrt(ea * eb) : 0;
        if (rho < 0.6) return false;
        a.ok = b2.ok = true;
        prs.push_back(a);
        prs.push_back(b2);
        eqNoise = 0.02;
        readDEq(0, supers[0].blk.data(), dv);
        return true;
    }

    // at a lock: the rate, the known symbols of the lock block and its two channel estimates
    void eqStart(bool start, const int dv[3], cf hh) {
        inScale = 1.0f / std::max(std::abs(hh), 1e-6f);
        prs.clear(); supers.clear();
        goodEst = 0;
        cirL0 = -6; l0Votes = 0;
        for (double& v : profC) v = 0;
        syncBefore = start;
        SuperInfo si;
        si.bps = bps; si.frames = frames; si.blk = block103(dv);
        supers.push_back(si);
        frameK = 1; probedFrames = 0; blockBad = 0;
        eqNoise = 0.05;
        addProbe(estimate(0));
        addProbe(estimate(72));
        // the first noise figure: the two estimates of the lock block against each other (30 ms apart), the common turn taken out
        {
            const ProbeEst& a = prs[0];
            const ProbeEst& b2 = prs[1];
            cf c(0, 0);
            for (int l = 0; l < kCirN; l++) c += b2.h[l] * std::conj(a.h[l]);
            const cf r = std::abs(c) > 0 ? std::conj(c) / std::abs(c) : cf(1, 0);
            double d = 0;
            for (int l = 0; l < kCirN; l++) d += std::norm(b2.h[l] * r - a.h[l]);
            eqNoise = cvNoise = std::clamp(d * 16.0 / (kCirN * 2.0), 1e-5, 0.5);
            ddNoise = -1;
            noiseUpdates = 0;
        }
    }

    // one data frame through the equaliser (once the channel estimates of the next two probes are in)
    bool trackEq() {
        while (probedFrames < frameK) { if (!probesAfter(probedFrames + 1)) return false; probedFrames++; }
        const int64_t k = frameK, sIdx = (k - 1) / kFramesPerSuper, f = (k - 1) % kFramesPerSuper + 1;
        const int64_t d0 = dataStartOf(k);
        if (probedFrames < frameK + 1) {
            // the channel after the next probe as well, unless the signal ends here (the last frame of a message)
            const int64_t nx = dataStartOf(k + 1);
            if (posOf(nx + 48) + 4 >= (double)nY) return false;
            double e = 0, ef = 0;
            for (int i = 8; i < 40; i++) e += std::norm(sampleAt(nx + i));
            for (int i = 0; i < kData; i += 8) ef += std::norm(sampleAt(d0 + i));
            const bool goesOn = e / 32 > 0.1 * ef / (kData / 8);
            if (goesOn) { if (!probesAfter(probedFrames + 1)) return false; probedFrames++; }
        }
        const SuperInfo& si = supers[(size_t)sIdx];
        if (si.bps != bps || si.frames != frames) {   // a new rate from a reinserted preamble
            event("the rate changed at a reinserted preamble");
            bps = si.bps; frames = si.frames; soft.clear();
            char m[96];
            snprintf(m, sizeof m, "%d bit/s, %s interleaver (%d frame%s)", bps, bps == 12800 ? "no" : hdrInterleaverName(frames), frames, frames == 1 ? "" : "s");
            st.mode = m; st.bps = bps; st.frames = frames;
        }
        const int bpsym = hdrBitsPerSymbol(bps), it = scrIters(bps), M = 1 << bpsym;
        const ScrTable& sc = scrTable();
        uint8_t dec[kData];
        cf pts[kData];
        double ddSum = 0, segDd = 0;
        float segSoft[kEqSeg * 6];
        std::complex<double> w[kNff], b[kMaxFb];
        double g = 1, var = 1;
        int l1 = 0, nfb = 0;
        cf hh[kCirN];
        for (int i = 0; i < kData; i++) {
            if (i % kEqSeg == 0) {
                interpH((double)(d0 + i + kEqSeg / 2), hh);
                mmse(hh, w, b, nfb, g, l1);
                var = std::max((1 - std::min(g, 0.9999)) / std::min(g, 0.9999), 1e-4);
            }
            const cf z = eqSymbol(d0 + i, l1, w, b, nfb, g, [&](int j) { return i - j >= 0 ? pts[i - j] : psk8(symKnown(d0 + i - j)); });
            const int s = sc.v[it][(size_t)i];
            float best0[6], best1[6];
            for (int q = 0; q < bpsym; q++) best0[q] = best1[q] = 1e30f;
            int bestL = 0;
            float bestD = 1e30f;
            for (int L = 0; L < M; L++) {
                const float dd = std::norm(z - dataPoint(bps, L, s));
                if (dd < bestD) { bestD = dd; bestL = L; }
                for (int q = 0; q < bpsym; q++) { if ((L >> (bpsym - 1 - q)) & 1) best1[q] = std::min(best1[q], dd); else best0[q] = std::min(best0[q], dd); }
            }
            for (int q = 0; q < bpsym; q++) segSoft[(size_t)((i % kEqSeg) * bpsym + q)] = best1[q] - best0[q];
            dec[i] = (uint8_t)bestL;
            pts[i] = dataPoint(bps, bestL, s);
            ddSum += bestD;
            segDd += bestD;
            if (i % kEqSeg == kEqSeg - 1) {
                // the soft values of the segment: the noise the filters expect, or the error the decisions show if that is larger
                // (a fade the interpolated channel missed)
                const float v = (float)std::max(var, 1.5 * segDd / kEqSeg);
                for (int q = 0; q < kEqSeg * bpsym; q++) soft.push_back(std::clamp(segSoft[(size_t)q] / v, -50.f, 50.f));
                segDd = 0;
            }
        }
        (void)dec;
        // the probe after the frame: a good estimate, or noise (or the end of the signal)
        bool good = false;
        for (const auto& pe : prs) if (pe.p0 == d0 + kData) good = pe.ok;
        badProbes = good ? 0 : badProbes + 1;
        if (good) goodProbes++;
        blockBad += !good;
        // a fade of every path can last a second or more in ITU-R Poor conditions: a well established signal is held for 24 probes
        if (badProbes >= (goodProbes == 0 ? 3 : goodProbes < 4 ? 6 : 24)) {
            if (goodProbes == 0) { st.messages--; unlock("false start dropped", (double)lockScan); }
            else unlock("signal lost", posOf(d0));
            return true;
        }
        // the noise for the filters: the error after the equaliser (it holds the noise and what the channel estimate missed); the
        // probes' figure is the ceiling, as decisions in error make the decision error too small
        const double dd = ddSum / kData;
        ddNoise = ddNoise < 0 ? dd : 0.7 * ddNoise + 0.3 * dd;
        eqNoise = std::clamp(std::min(ddNoise, cvNoise), 1e-5, 0.5);
        noise = eqNoise; sig = 1;
        st.snrDb = 10 * std::log10(1.0 / std::max(eqNoise, 1e-6));
        hPrevPos = posOf(d0 + kData + 15);
        st.framesRx++;
        frame = (int)f;
        if (f % frames == 0 || (frames == 72 && f == kFramesPerSuper)) {
            if ((int)soft.size() == hdrInterleaverBits(bps, frames)) {
                const std::vector<uint8_t> bits = hdrDecodeBlock(soft, bps, frames);
                st.blocks++;
                soft.clear();
                const bool mostlyLost = 2 * blockBad > frames;   // the signal was gone for most of the block: nothing to deliver
                blockBad = 0;
                if (!mostlyLost && deliver(bits)) { unlock("end of message", posOf(d0 + kData)); return true; }
            } else if ((int)soft.size() > hdrInterleaverBits(bps, frames)) { soft.clear(); blockBad = 0; }
        }
        if (f == kFramesPerSuper) { superN++; st.superframes++; }
        frameK++;
        st.cfoHz = cfo;
        return true;
    }

    cf probeGain(int64_t m0, int sign, double& energy) const {
        cf acc(0, 0);
        energy = 0;
        for (int i = 0; i < kProbe; i++) { const cf v = symAt(m0 + i); acc += v * std::conj(psk8(kProbePlus[i] + (sign < 0 ? 4 : 0))); energy += std::norm(v); }
        return acc / (float)kProbe;
    }

    void unlock(const char* why, double resumeAt) {
        if (why) event(why);
        state = 0; st.state = 0;
        scan = std::max<int64_t>(y0 + kSps, (int64_t)resumeAt);
        // whatever is held back (the last 32 bits) belongs to the message
        for (uint8_t b : pendingBits) outBit(b);
        pendingBits.clear();
    }

    void outBit(uint8_t b) {
        parser.pushBit(b, [this](const D5066& d) { if (pduCb) pduCb(d); });
        curByte = (uint8_t)(curByte | (b << curBits));
        if (++curBits == 8) { if (byteCb) byteCb(curByte); st.bytesOut++; curByte = 0; curBits = 0; }
    }

    // returns true when it stopped at an end-of-message
    bool deliver(const std::vector<uint8_t>& bits) {
        for (uint8_t b : bits) {
            eomWin = (eomWin << 1) | b;
            pendingBits.push_back(b);
            if (pendingBits.size() >= 32 && eomWin == kHdrEom) {
                pendingBits.resize(pendingBits.size() - 32);
                for (uint8_t q : pendingBits) outBit(q);
                pendingBits.clear();
                st.eoms++;
                return true;
            }
            if (pendingBits.size() > 32) { outBit(pendingBits.front()); pendingBits.erase(pendingBits.begin()); }
        }
        return false;
    }

    // one data frame and the probe after it; at frame 72, the reinserted block
    bool track() {
        if (useEq) return trackEq();
        const int64_t mData = kBlock + (int64_t)(frame - 1) * kFrame;   // first data symbol of this frame
        const int64_t mProbe = mData + kData;
        const int64_t mEnd = frame < kFramesPerSuper ? mProbe + kProbe : mProbe + kBlock;
        if (posOf(mEnd) + 3 >= (double)nY) return false;
        // the probe after this frame: its sign is known; adjust timing with an early / late comparison
        const int sg = frame < kFramesPerSuper ? probeSign(frame, bps, frames) : 1;
        double e0, el, ee;
        cf hN = probeGain(mProbe, sg, e0);
        {
            const double t0s = t0;
            t0 = t0s + 2; const cf hl = probeGain(mProbe, sg, el);
            t0 = t0s - 2; const cf he = probeGain(mProbe, sg, ee);
            t0 = t0s;
            const double a = std::abs(hl), b = std::abs(he), c = std::abs(hN);
            if (c > 0) t0 += std::clamp(3.5 * (a - b) / c, -1.0, 1.0);   // (a - b) / c is about 0.13 per sample of timing error (raised cosine at 0.2 T)
            hN = probeGain(mProbe, sg, e0);
        }
        // quality: the probe fits, or it is noise (or a probe of the wrong sign: lost track)
        const double fit = std::norm(hN) * kProbe / std::max(e0, 1e-12);
        badProbes = fit < 0.25 ? badProbes + 1 : 0;
        if (fit >= 0.25) goodProbes++;
        if (badProbes >= 3) {
            // a lock that never saw a good probe was a false one: look again from just after it
            if (goodProbes == 0) { st.messages--; unlock("false start dropped", (double)lockScan); }
            else unlock("signal lost", posOf(mData));
            return true;
        }
        // frequency: the phase change since the last probe
        const double dist = (posOf(mProbe + 15) - hPrevPos) / kFs;
        if (dist > 0 && std::abs(hPrev) > 0) {
            const double dph = std::arg(hN * std::conj(hPrev));
            setCfo(cfo + 0.5 * dph / (2 * kPi * dist), posOf(mProbe + 15));
            // keep hN consistent with the new frequency
            hN = probeGain(mProbe, sg, e0);
        }
        // the previous probe again, with the same frequency, for the interpolation
        cf hP;
        double eP;
        if (frame == 1) hP = (fitFixed(t0, cfo, 71, kBlock) / 32.0f);
        else hP = probeGain(mData - kProbe, probeSign(frame - 1, bps, frames), eP);
        // noise from the probe residual
        {
            double er = 0;
            for (int i = 0; i < kProbe; i++) er += std::norm(symAt(mProbe + i) - hN * psk8(kProbePlus[i] + (sg < 0 ? 4 : 0)));
            noise = 0.9 * noise + 0.1 * std::max(er / kProbe, 1e-9);
            sig = 0.9 * sig + 0.1 * std::norm(hN);
            st.snrDb = 10 * std::log10(sig / noise);
        }
        // the data symbols: channel interpolated between the probes, soft bits by max-log
        const int bpsym = hdrBitsPerSymbol(bps), it = scrIters(bps), M = bps <= 4800 ? (1 << bpsym) : (1 << bpsym);
        const ScrTable& sc = scrTable();
        for (int i = 0; i < kData; i++) {
            const float u = (float)(i + 16) / (float)(kData + 31);
            const cf hc = hP * (1 - u) + hN * u;
            const float g2 = std::norm(hc);
            const cf z = g2 > 0 ? symAt(mData + i) / hc : cf(0, 0);
            float best0[6], best1[6];
            for (int k = 0; k < bpsym; k++) best0[k] = best1[k] = 1e30f;
            const int s = sc.v[it][(size_t)i];
            for (int L = 0; L < M; L++) {
                const float dd = std::norm(z - dataPoint(bps, L, s));
                for (int k = 0; k < bpsym; k++) {
                    if ((L >> (bpsym - 1 - k)) & 1) best1[k] = std::min(best1[k], dd); else best0[k] = std::min(best0[k], dd);
                }
            }
            const float scale = (float)(g2 / std::max(noise, 1e-9));
            for (int k = 0; k < bpsym; k++) soft.push_back(std::clamp((best1[k] - best0[k]) * scale, -50.f, 50.f));
        }
        st.framesRx++;
        hPrev = hN; hPrevPos = posOf(mProbe + 15);
        // a complete interleaver block
        if (frame % frames == 0) {
            const std::vector<uint8_t> bits = hdrDecodeBlock(soft, bps, frames);
            soft.clear();
            st.blocks++;
            if (deliver(bits)) { unlock("end of message", posOf(mProbe)); return true; }
        }
        if (frame == kFramesPerSuper) {
            // the reinserted preamble: the rate and interleaver again
            const double tNew = posOf(kSuper);
            t0 = tNew;
            int dv[3]; cf hh; double snr;
            int r = 0, fr = 0;
            if (readD(dv, hh, snr) && hdrFromD(dv, r, fr)) {
                if (r != bps || fr != frames) { event("the rate changed at a reinserted preamble"); bps = r; frames = fr; soft.clear(); }
            }
            hPrev = hh; hPrevPos = posOf(kBlock - 16);
            superN++; st.superframes++;
            frame = 1;
        } else {
            frame++;
        }
        st.cfoHz = cfo;
        return true;
    }
};

HighRateRx::HighRateRx() : p_(new Impl) {}
HighRateRx::~HighRateRx() = default;
void HighRateRx::reset() {
    auto b = std::move(p_->byteCb); auto e = std::move(p_->evCb); auto d = std::move(p_->pduCb);
    const bool eq = p_->useEq;
    p_.reset(new Impl);
    p_->byteCb = std::move(b); p_->evCb = std::move(e); p_->pduCb = std::move(d);
    p_->useEq = eq;
}
void HighRateRx::setEqualizer(bool on) { p_->useEq = on; }
void HighRateRx::push(const cf32* x, size_t n) { p_->push(x, n); }
void HighRateRx::setByteSink(std::function<void(uint8_t)> cb) { p_->byteCb = std::move(cb); }
void HighRateRx::setEventSink(std::function<void(const SerialEvent&)> cb) { p_->evCb = std::move(cb); }
void HighRateRx::setD5066Sink(std::function<void(const D5066&)> cb) { p_->pduCb = std::move(cb); }
HdrStatus HighRateRx::status() const { return p_->st; }

}  // namespace dect2::hfu
