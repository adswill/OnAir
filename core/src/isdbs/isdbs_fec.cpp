// ISDB-S: the coding of a frame, see isdbs_fec.h.
#include "isdbs_fec.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dect2 {
namespace isdbs {

namespace {
constexpr float kInvSqrt2 = 0.70710678118f;
// puncturing (BO.1408 Table 2, the same patterns as DVB-S): per period of k steps, which of X and Y are sent; sent bits go out X first.
// The serial bits then fill the QPSK symbols I first (P1 = I, P0 = Q), or one BPSK symbol each.
struct Punct { int k; const char* x; const char* y; };
const Punct& punct(int mod) {
    static const Punct p12{1, "1", "1"}, p23{2, "10", "11"}, p34{3, "101", "110"}, p56{5, "10101", "11010"}, p78{7, "1000101", "1111010"};
    switch (mod) {
    case kQpsk23: return p23;
    case kQpsk34: return p34;
    case kQpsk56: return p56;
    case kQpsk78: return p78;
    default: return p12;
    }
}
inline int parity(unsigned v) { return __builtin_popcount(v) & 1; }
}  // namespace

unsigned convOut(unsigned state, int in) {
    const unsigned w = ((unsigned)in << 6) | (state & 63);   // bit 6 the input, bit 5 the previous input .. bit 0 six inputs back
    return (unsigned)((parity(w & 0171) << 1) | parity(w & 0133));
}

unsigned w1State() {
    unsigned s = 0;
    for (int i = 0; i < 6; i++) s = convNext(s, (kW1 >> (15 - i)) & 1);
    return s;
}

void Layout::build(const Tmcc& t) {
    groups.clear();
    group.fill(-1);
    ordinal.fill(-1);
    ok = false;
    int slot = 0, step = kHeaderBits;
    for (int g = 0; g < kMaxGroups; g++) {
        if (!isMod(t.mode[g]) || t.count[g] <= 0) continue;
        Group G;
        G.mod = t.mode[g]; G.firstSlot = slot; G.count = t.count[g];
        if (slot + G.count > kSlots || G.count % slotUnit(G.mod)) return;
        G.step0 = step; G.steps = groupSteps(G.mod, G.count); G.sym0 = slot * kSlotSyms;
        const int u = slotUnit(G.mod), a = slotActive(G.mod);
        for (int q = 0; q < G.count; q++) {
            group[(size_t)(slot + q)] = (int)groups.size();
            const int pos = q % u;
            ordinal[(size_t)(slot + q)] = pos < a ? (q / u) * a + pos : -1;
        }
        groups.push_back(G);
        slot += G.count; step += G.steps;
    }
    totalSteps = step;
    ok = true;
}

// ============================================================================ energy dispersal
void prbs15(uint8_t* bits, size_t n) {
    // X1..X15 loaded with 1 0 0 1 0 1 0 1 0 0 0 0 0 0 0; the output X14 + X15 is fed back into X1
    uint8_t x[16] = {0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        const uint8_t o = x[14] ^ x[15];
        for (int k = 15; k > 1; k--) x[k] = x[k - 1];
        x[1] = o;
        bits[i] = o;
    }
}

namespace {
const std::vector<uint8_t>& mainPrbs() {
    // from the second byte of slot 1 of frame 1 to the end of the super-frame; it is read here as running on through the sync bytes it is
    // not added to (as in DVB-S): BO.1408 does not say
    static const std::vector<uint8_t> t = [] {
        const size_t nb = (size_t)kFramesPerSuper * kSlots * 204;
        std::vector<uint8_t> bits(nb * 8), by(nb);
        prbs15(bits.data(), bits.size());
        for (size_t i = 0; i < nb; i++) { unsigned v = 0; for (int k = 0; k < 8; k++) v = (v << 1) | bits[i * 8 + (size_t)k]; by[i] = (uint8_t)v; }
        return by;
    }();
    return t;
}
}  // namespace

uint8_t prbsByte(int f, int s, int b) { return mainPrbs()[(size_t)((f * kSlots + s) * 204 + b - 1)]; }

const std::vector<uint8_t>& tmccPrbs() {
    // started at the first TMCC bit after W1 of the first frame (BO.1408 6.4) and, as read here, clocked only over TMCC bits
    static const std::vector<uint8_t> t = [] { std::vector<uint8_t> v(kTmccCoded); prbs15(v.data(), v.size()); return v; }();
    return t;
}

void headerBits(const uint8_t* tmcc512, int f, uint8_t* out) {
    const auto& pr = tmccPrbs();
    const uint16_t end = f == 0 ? kW2 : kW3;
    for (int i = 0; i < 16; i++) out[i] = (kW1 >> (15 - i)) & 1;
    for (int i = 0; i < 64; i++) out[16 + i] = (uint8_t)((tmcc512[f * 64 + i] ^ pr[(size_t)(f * 64 + i)]) & 1);
    for (int i = 0; i < 16; i++) out[80 + i] = (end >> (15 - i)) & 1;
}

// ============================================================================ encoder
void FrameEncoder::encode(const Layout& L, const uint8_t* header, const std::vector<std::vector<uint8_t>>& groupBytes, cf32* frame, const float* burstIn) {
    std::vector<cf32> data((size_t)kDataSyms + kHeaderSyms);
    size_t ns = 0;
    auto bpskBit = [&](int c) { data[ns++] = cf32(c ? -1.f : 1.f, 0.f); };
    for (int i = 0; i < kHeaderBits; i++) {
        const unsigned o = convOut(state_, header[i]);
        state_ = convNext(state_, header[i]);
        bpskBit((int)(o >> 1)); bpskBit((int)(o & 1));   // P1 (X) then P0 (Y)
    }
    for (size_t g = 0; g < L.groups.size(); g++) {
        const Layout::Group& G = L.groups[g];
        const std::vector<uint8_t>& by = groupBytes[g];
        const size_t nbits = by.size() * 8;
        auto bit = [&](size_t i) { return (by[i >> 3] >> (7 - (i & 7))) & 1; };
        ns = (size_t)kHeaderSyms + (size_t)G.sym0;
        const size_t symEnd = ns + (size_t)G.count * kSlotSyms;
        if (G.mod == kTc8psk23) {
            for (size_t i = 0; i + 1 < nbits && ns < symEnd; i += 2) {
                const int b1 = bit(i), b0 = bit(i + 1);
                const unsigned o = convOut(state_, b0);
                state_ = convNext(state_, b0);
                data[ns++] = psk8Point((b1 << 2) | (int)((o & 1) << 1) | (int)(o >> 1));   // label (W, Y, X)
            }
        } else {
            const Punct& P = punct(G.mod);
            std::vector<uint8_t> ser;
            ser.reserve(nbits * 2);
            for (size_t i = 0; i < nbits; i++) {
                const unsigned o = convOut(state_, bit(i));
                state_ = convNext(state_, bit(i));
                const int ph = (int)(i % (size_t)P.k);
                if (P.x[ph] == '1') ser.push_back((uint8_t)(o >> 1));
                if (P.y[ph] == '1') ser.push_back((uint8_t)(o & 1));
            }
            if (G.mod == kBpsk12) for (uint8_t c : ser) bpskBit(c);
            else for (size_t i = 0; i + 1 < ser.size(); i += 2) data[ns++] = cf32(ser[i] ? -kInvSqrt2 : kInvSqrt2, ser[i + 1] ? -kInvSqrt2 : kInvSqrt2);
        }
    }
    // the frame: header, then 203 data symbols and 4 burst symbols at a time
    const float* burst = burstIn ? burstIn : burstSequence().data();
    for (int i = 0; i < kHeaderSyms; i++) frame[i] = data[(size_t)i];
    for (int j = 0; j < kBlocks; j++) {
        cf32* b = frame + kHeaderSyms + j * kBlockSyms;
        memcpy(b, &data[(size_t)(kHeaderSyms + j * kBlockData)], kBlockData * sizeof(cf32));
        for (int i = 0; i < kBurstSyms; i++) b[kBlockData + i] = cf32(burst[(size_t)(j * kBurstSyms + i)], 0.f);
    }
}

// ============================================================================ decoder
float viterbi(const Branch* b, int n, int startState, int endState, const int8_t* forced, uint8_t* bits, uint8_t* states) {
    const float kNeg = -1e30f;
    float pm[64], npm[64];
    for (int s = 0; s < 64; s++) pm[s] = startState < 0 || s == startState ? 0.f : kNeg;
    std::vector<uint64_t> dec((size_t)n);
    double acc = 0;
    // the outputs of the two branches into each state: from s = ((ns & 31) << 1) | k with input ns >> 5
    static unsigned outTab[64][2];
    static bool init = false;
    if (!init) { for (int ns = 0; ns < 64; ns++) for (int k = 0; k < 2; k++) outTab[ns][k] = convOut((unsigned)(((ns & 31) << 1) | k), ns >> 5); init = true; }
    for (int t = 0; t < n; t++) {
        const float* m = b[t].m;
        uint64_t d = 0;
        const int f = forced ? forced[t] : -1;
        for (int ns = 0; ns < 64; ns++) {
            const int p0 = (ns & 31) << 1, p1 = p0 | 1;
            const float c0 = pm[p0] + m[outTab[ns][0]], c1 = pm[p1] + m[outTab[ns][1]];
            if (c1 > c0) { npm[ns] = c1; d |= 1ull << ns; } else npm[ns] = c0;
            if (f >= 0 && (ns >> 5) != f) npm[ns] = kNeg;
        }
        dec[(size_t)t] = d;
        float hi = kNeg;
        for (int s = 0; s < 64; s++) hi = std::max(hi, npm[s]);
        for (int s = 0; s < 64; s++) pm[s] = npm[s] < -1e29f ? kNeg : npm[s] - hi;
        acc += hi;
    }
    int s = endState;
    if (s < 0) { s = 0; for (int q = 1; q < 64; q++) if (pm[q] > pm[s]) s = q; }
    const float metric = (float)(acc + pm[s]);
    if (states) states[n] = (uint8_t)s;
    for (int t = n - 1; t >= 0; t--) {
        bits[t] = (uint8_t)(s >> 5);
        s = ((s & 31) << 1) | (int)((dec[(size_t)t] >> s) & 1);
        if (states) states[t] = (uint8_t)s;
    }
    return metric;
}

void headerBranches(const cf32* frame, Branch* out, int nbits) {
    for (int i = 0; i < nbits; i++) {
        const float x = 2.f * frame[2 * i].real(), y = 2.f * frame[2 * i + 1].real();
        Branch& B = out[i];
        B.m[0] = x + y; B.m[1] = x - y; B.m[2] = -x + y; B.m[3] = -x - y;
    }
}

void groupBranches(const Layout& L, int g, const cf32* frame, Branch* out) {
    const Layout::Group& G = L.groups[(size_t)g];
    const int nSym = G.count * kSlotSyms;
    if (G.mod == kTc8psk23) {
        cf32 p0[4];
        for (int xy = 0; xy < 4; xy++) p0[xy] = std::conj(psk8Point(((xy & 1) << 1) | (xy >> 1)));   // W = 0, Y = xy & 1, X = xy >> 1
        for (int i = 0; i < nSym; i++) {
            const cf32 r = frame[dataPos(G.sym0 + i)];
            Branch& B = out[i];
            for (int xy = 0; xy < 4; xy++) B.m[xy] = 2.f * std::fabs((r * p0[xy]).real());   // the better of the two points W = 0, 1 (opposite)
        }
        return;
    }
    // soft values of the serial coded bits (> 0 means bit 0), in the order they were sent
    std::vector<float> ser;
    ser.reserve((size_t)nSym * 2);
    if (G.mod == kBpsk12) for (int i = 0; i < nSym; i++) ser.push_back(2.f * frame[dataPos(G.sym0 + i)].real());
    else for (int i = 0; i < nSym; i++) { const cf32 r = frame[dataPos(G.sym0 + i)]; ser.push_back(2.f * kInvSqrt2 * r.real()); ser.push_back(2.f * kInvSqrt2 * r.imag()); }
    const Punct& P = punct(G.mod);
    size_t k = 0;
    for (int t = 0; t < G.steps; t++) {
        const int ph = t % P.k;
        float x = 0, y = 0;
        if (P.x[ph] == '1' && k < ser.size()) x = ser[k++];
        if (P.y[ph] == '1' && k < ser.size()) y = ser[k++];
        Branch& B = out[t];
        B.m[0] = x + y; B.m[1] = x - y; B.m[2] = -x + y; B.m[3] = -x - y;
    }
}

void tc8Uncoded(const Layout& L, int g, const cf32* frame, const uint8_t* states, const uint8_t* bits, int from, int to, uint8_t* w) {
    const Layout::Group& G = L.groups[(size_t)g];
    for (int i = from; i < to; i++) {
        const unsigned o = convOut(states[i - from], bits[i - from]);
        const cf32 p = psk8Point((int)(((o & 1) << 1) | (o >> 1)));
        w[i - from] = (frame[dataPos(G.sym0 + i)] * std::conj(p)).real() < 0 ? 1 : 0;
    }
}

}  // namespace isdbs
}  // namespace dect2
