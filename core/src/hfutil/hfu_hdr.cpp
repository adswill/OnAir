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
    // decision-feedback equaliser (RLS), trained on the preamble, the mini-probes and the reinserted preambles
    static constexpr int kFfLo = -4, kFfHi = 12;                 // feed-forward taps T/2 apart, -2 T to +6 T around the first path
    static constexpr int kNff = kFfHi - kFfLo + 1, kNfb = 8, kNeq = kNff + kNfb;
    bool useEq = true;
    std::vector<std::complex<double>> w, P, pu;
    cf u[kNeq];
    cf fb[kNfb];                     // the last decisions, the newest first
    float inScale = 1;
    double eqLambda = 0.993, eqMse = 0.1, eqAcc = 0;
    int eqAccN = 0;
    std::vector<double> prof;        // the power delay profile of the probes (lags -20 .. +60 samples), averaged
    std::vector<cf> cirPrev;
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
        const double ph = phRef - 2 * kPi * cfo * (pos - phPos) / kFs;
        return v * cf((float)std::cos(ph), (float)std::sin(ph));
    }
    void setCfo(double f, double pos) {
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
            if (m > 0.5 && m > candM) { candM = m; candT = scan; candC = c; }
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
        phRef = 0; phPos = 0;
        // fine timing (tenth of a sample) and frequency (the phase slope between the two mini-probes)
        double bestT = (double)t, bestE = -1;
        for (double dt = -1.0; dt <= 1.0001; dt += 0.1) {
            const double e = std::abs(fitFixed((double)t + dt, f0, 0, kBlock));
            if (e > bestE) { bestE = e; bestT = (double)t + dt; }
        }
        double f = f0;
        for (int pass = 0; pass < 2; pass++) {
            const cf a = fitFixed(bestT, f, 0, 32), b = fitFixed(bestT, f, 71, kBlock);
            f += std::arg(b * std::conj(a)) * kBaud / (2 * kPi * 71.0);
        }
        t0 = bestT; cfo = f; sps = kSps;
        int dv[3];
        cf hh;
        double snr;
        if (!readD(dv, hh, snr)) return false;
        int r = 0, fr = 0;
        if (!hdrFromD(dv, r, fr)) { char b[80]; snprintf(b, sizeof b, "preamble with an unknown rate code D = %d %d %d", dv[0], dv[1], dv[2]); event(b); return false; }
        // was this the start of a transmission (the 184 sync symbols just before) or a reinserted preamble?
        cf s(0, 0);
        float e = 0;
        for (int k = 0; k < 184; k++) { const cf v = symAt(k - 184); s += v * std::conj(psk8(kSync184[k])); e += std::abs(v); }
        const bool start = e > 0 && std::abs(s) / e > 0.5;
        bps = r; frames = fr; frame = 1; superN = 0; badProbes = 0; goodProbes = 0;
        if (useEq) eqStart(start, dv, hh);
        hPrev = useEq ? fitFixed(t0, cfo, 71, kBlock) / 32.0f : hh; hPrevPos = posOf(kBlock - 16);
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

    // ---- decision-feedback equaliser ------------------------------------------------------------------------------------------------
    // the channel at lags lo .. hi samples from n known symbols starting at symbol m0
    void cirAt(int64_t m0, const uint8_t* sym, int n, int lo, int hi, std::vector<cf>& c) const {
        c.assign((size_t)(hi - lo + 1), cf(0, 0));
        for (int l = lo; l <= hi; l++) {
            cf a(0, 0);
            for (int i = 0; i < n; i++) a += at(posOf(m0 + i) + l) * std::conj(psk8(sym[i]));
            c[(size_t)(l - lo)] = a / (float)n;
        }
    }
    // the position (in samples, relative to lag lo) of the first path: the first lag within 5 dB of the strongest, then up to its peak
    static double firstPath(const std::vector<double>& pw, int lo) {
        double mx = 0;
        for (double v : pw) mx = std::max(mx, v);
        if (mx <= 0) return 0;
        size_t i = 0;
        while (i < pw.size() && pw[i] < 0.3 * mx) i++;
        while (i + 1 < pw.size() && pw[i + 1] > pw[i]) i++;
        double d = 0;
        if (i > 0 && i + 1 < pw.size()) {
            const double a = pw[i - 1], b = pw[i], c = pw[i + 1], den = a - 2 * b + c;
            if (den < 0) d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        }
        return (double)i + d + (double)lo;
    }
    void eqResetP() {
        P.assign((size_t)(kNeq * kNeq), 0.0);
        for (int i = 0; i < kNeq; i++) P[(size_t)(i * kNeq + i)] = 10.0;
    }
    void eqRegress(int64_t m) {
        const double p0 = posOf(m);
        for (int i = 0; i < kNff; i++) u[i] = at(p0 + (double)(kFfLo + i) * sps * 0.5) * inScale;
        for (int j = 0; j < kNfb; j++) u[kNff + j] = fb[j];
    }
    cf eqOut() const {
        std::complex<double> z = 0;
        for (int i = 0; i < kNeq; i++) z += std::conj(w[(size_t)i]) * std::complex<double>(u[i]);
        return cf((float)z.real(), (float)z.imag());
    }
    // RLS (output w^H u, a priori error e)
    void eqUpdate(cf e) {
        const int N = kNeq;
        double den = eqLambda;
        for (int r = 0; r < N; r++) {
            std::complex<double> a = 0;
            for (int c = 0; c < N; c++) a += P[(size_t)(r * N + c)] * std::complex<double>(u[c]);
            pu[(size_t)r] = a;
            den += (std::conj(std::complex<double>(u[r])) * a).real();
        }
        if (!(den > 1e-12) || !std::isfinite(den)) { eqResetP(); return; }
        const std::complex<double> ce = std::conj(std::complex<double>(e));
        double tr = 0;
        for (int r = 0; r < N; r++) {
            const std::complex<double> k = pu[(size_t)r] / den;
            w[(size_t)r] += k * ce;
            for (int c = 0; c < N; c++) P[(size_t)(r * N + c)] = (P[(size_t)(r * N + c)] - k * std::conj(pu[(size_t)c])) / eqLambda;
            tr += P[(size_t)(r * N + r)].real();
        }
        if (!(tr < 1e7)) eqResetP();
    }
    void eqPush(cf d) { for (int j = kNfb - 1; j > 0; j--) fb[j] = fb[j - 1]; fb[0] = d; }
    cf eqTrain(int64_t m, cf d) {
        eqRegress(m);
        const cf z = eqOut();
        eqUpdate(d - z);
        eqPush(d);
        eqAcc += std::norm(d - z); eqAccN++;
        return z;
    }

    // at a lock: the reference on the first path of the channel, then the equaliser trained on the known symbols
    void eqStart(bool start, const int dv[3], cf hh) {
        uint8_t pp[31], pm[31];
        for (int i = 0; i < 31; i++) { pp[i] = kProbePlus[i]; pm[i] = (uint8_t)((kProbePlus[i] + 4) & 7); }
        std::vector<cf> c1, c2, c3;
        cirAt(0, pp, 31, -60, 60, c1);
        cirAt(72, pm, 31, -60, 60, c2);
        if (start) cirAt(-184, kSync184, 184, -60, 60, c3);
        std::vector<double> pw(c1.size());
        for (size_t i = 0; i < pw.size(); i++) pw[i] = std::norm(c1[i]) + std::norm(c2[i]) + (c3.empty() ? 0.0 : 2 * std::norm(c3[i]));
        t0 += firstPath(pw, -60);
        inScale = 1.0f / std::max(std::abs(hh), 1e-6f);
        w.assign((size_t)kNeq, 0.0);
        pu.assign((size_t)kNeq, 0.0);
        eqResetP();
        for (auto& v : fb) v = cf(0, 0);
        if (start) for (int k = 0; k < 184; k++) eqTrain(k - 184, psk8(kSync184[k]));
        const auto b = block103(dv);
        for (int k = 0; k < kBlock; k++) { if (k == kBlock - 31) { eqAcc = 0; eqAccN = 0; } eqTrain(k, psk8(b[(size_t)k])); }
        eqMse = std::clamp(eqAcc / std::max(1, eqAccN), 1e-4, 1.0);
        prof.clear();
        cirPrev.clear();
    }

    // one data frame through the equaliser, then the probe after it (at frame 72 the reinserted preamble) to train it
    bool trackEq() {
        const int64_t mData = kBlock + (int64_t)(frame - 1) * kFrame;
        const int64_t mProbe = mData + kData;
        const bool last = frame == kFramesPerSuper;
        const int64_t mEnd = !last ? mProbe + kProbe : mProbe + kBlock;
        if (posOf(mEnd) + (double)kFfHi * sps * 0.5 + 64 >= (double)nY) return false;
        const int bpsym = hdrBitsPerSymbol(bps), it = scrIters(bps), M = 1 << bpsym;
        const ScrTable& sc = scrTable();
        cf zs[kData];
        for (int i = 0; i < kData; i++) {
            eqRegress(mData + i);
            const cf z = eqOut();
            const int s = sc.v[it][(size_t)i];
            int bestL = 0;
            float bestD = 1e30f;
            for (int L = 0; L < M; L++) { const float dd = std::norm(z - dataPoint(bps, L, s)); if (dd < bestD) { bestD = dd; bestL = L; } }
            const cf d = dataPoint(bps, bestL, s);
            eqUpdate(d - z);
            eqPush(d);
            zs[i] = z;
        }
        // the known symbols after the frame
        eqAcc = 0; eqAccN = 0;
        int dvNew[3] = {-1, -1, -1};
        uint8_t ps[31];
        const int sg = !last ? probeSign(frame, bps, frames) : 1;
        for (int i = 0; i < 31; i++) ps[i] = (uint8_t)((kProbePlus[i] + (sg < 0 ? 4 : 0)) & 7);
        if (!last) {
            for (int i = 0; i < kProbe; i++) eqTrain(mProbe + i, psk8(ps[i]));
        } else {
            int d0[3] = {0, 0, 0};
            const auto b = block103(d0);
            for (int k = 0; k < 32; k++) eqTrain(mProbe + k, psk8(b[(size_t)k]));
            cf zd[39];
            for (int k = 32; k < 71; k++) {   // D0 D1 D2: decisions on the four even phases
                eqRegress(mProbe + k);
                const cf z = eqOut();
                const int q = (int)std::lround(std::arg(z) / (kPi / 2));
                const cf d = psk8(((q % 4 + 4) % 4) * 2);
                eqUpdate(d - z);
                eqPush(d);
                zd[k - 32] = z;
            }
            for (int k = 71; k < kBlock; k++) eqTrain(mProbe + k, psk8(b[(size_t)k]));
            for (int i = 0; i < 3; i++) {
                cf a(0, 0);
                for (int k = 0; k < 13; k++) a += zd[13 * i + k] * std::conj(psk8(kBarker[k]));
                const int q = (int)std::lround(std::arg(a) / (kPi / 2));
                dvNew[i] = ((q % 4 + 4) % 4) * 2;
            }
        }
        const double mse = eqAccN ? eqAcc / eqAccN : 1.0;
        const bool good = mse < 0.5;
        badProbes = good ? 0 : badProbes + 1;
        if (good) goodProbes++;
        if (badProbes >= (goodProbes ? 6 : 3)) {
            if (goodProbes == 0) { st.messages--; unlock("false start dropped", (double)lockScan); }
            else unlock("signal lost", posOf(mData));
            return true;
        }
        // soft values with the noise of the probes before and after the frame
        const float scale = (float)(2.0 / std::max(eqMse + mse, 2e-4));
        eqMse = std::clamp(mse, 1e-4, 1.0);
        for (int i = 0; i < kData; i++) {
            float best0[6], best1[6];
            for (int k = 0; k < bpsym; k++) best0[k] = best1[k] = 1e30f;
            const int s = sc.v[it][(size_t)i];
            for (int L = 0; L < M; L++) {
                const float dd = std::norm(zs[i] - dataPoint(bps, L, s));
                for (int k = 0; k < bpsym; k++) { if ((L >> (bpsym - 1 - k)) & 1) best1[k] = std::min(best1[k], dd); else best0[k] = std::min(best0[k], dd); }
            }
            for (int k = 0; k < bpsym; k++) soft.push_back(std::clamp((best1[k] - best0[k]) * scale, -50.f, 50.f));
        }
        noise = 0.9 * noise + 0.1 * std::max(mse, 1e-9);
        sig = 1;
        st.snrDb = 10 * std::log10(1.0 / noise);
        // frequency (the phase change of the whole channel since the last probe) and the timing of the first path
        std::vector<cf> c;
        cirAt(mProbe, ps, 31, -20, 60, c);
        const double here = posOf(mProbe + 15);
        if (cirPrev.size() == c.size()) {
            cf a(0, 0);
            for (size_t l = 0; l < c.size(); l++) a += c[l] * std::conj(cirPrev[l]);
            const double dist = (here - hPrevPos) / kFs;
            if (dist > 0 && std::abs(a) > 0) setCfo(cfo + 0.3 * std::arg(a) / (2 * kPi * dist), here);
        }
        cirPrev = c;
        if (prof.size() != c.size()) prof.assign(c.size(), 0.0);
        for (size_t l = 0; l < c.size(); l++) prof[l] = goodProbes <= 1 ? std::norm(c[l]) : 0.9 * prof[l] + 0.1 * std::norm(c[l]);
        if (goodProbes >= 4) {
            const double lead = firstPath(prof, -20);
            if (std::fabs(lead) > 0.3) t0 += std::clamp(0.2 * lead, -0.25, 0.25);
        }
        hPrevPos = here;
        st.framesRx++;
        if (frame % frames == 0) {
            const std::vector<uint8_t> bits = hdrDecodeBlock(soft, bps, frames);
            soft.clear();
            st.blocks++;
            if (deliver(bits)) { unlock("end of message", posOf(mProbe)); return true; }
        }
        if (last) {
            t0 = posOf(kSuper);
            int r = 0, fr = 0;
            if (hdrFromD(dvNew, r, fr) && (r != bps || fr != frames)) { event("the rate changed at a reinserted preamble"); bps = r; frames = fr; soft.clear(); }
            superN++; st.superframes++;
            frame = 1;
        } else {
            frame++;
        }
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
