// LR-FHSS coding chain, hopping and time on air (see lrfhss.h).
// Ported from Semtech sx126x_driver v2.5.0, src/lr_fhss_mac.c and src/sx126x_lr_fhss.c:
//   The Clear BSD License. Copyright Semtech Corporation 2021. All rights reserved.
//   Redistribution and use in source and binary forms, with or without modification, are permitted (subject to the limitations in the
//   disclaimer below) provided that the following conditions are met: redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer; redistributions in binary form must reproduce the above copyright notice, this list
//   of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution; neither the name of
//   the Semtech corporation nor the names of its contributors may be used to endorse or promote products derived from this software without
//   specific prior written permission.
//   NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS
//   AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
//   OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//   (Full text: LICENSES/BSD-3-Clause-Clear.txt.)
// Copied: kHeaderInterleaver, the hop-sequence polynomials, the channel counts. The rest is OnAir's own code following those functions.
// Synthetic tests only; not checked against a real transmitter.
#include "dect2/lrfhss/lrfhss.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dect2 {
namespace lrfhss {

namespace {
// lr_fhss_channel_count (Table 9 of Semtech's specification v18), by bandwidth index
constexpr int kChannelCount[10] = {80, 176, 280, 376, 688, 792, 1480, 1584, 3120, 3224};
// lr_fhss_lfsr_poly1 / 2 / 3
constexpr uint16_t kPoly1[6] = {33, 45, 48, 51, 54, 57};
constexpr uint16_t kPoly2[4] = {65, 68, 71, 72};
constexpr uint16_t kPoly3[2] = {142, 149};
constexpr double kOcwHz[10] = {39063, 85938, 136719, 183594, 335938, 386719, 722656, 773438, 1523438, 1574219};

// convolutional codes: register = (previous bits << 1) | new bit, output bit = parity(register & generator), first generator first
constexpr int kK13 = 7;
constexpr unsigned kG13[3] = {0155, 0117, 0127};
constexpr int kK12 = 5;
constexpr unsigned kG12[2] = {035, 023};
// puncturing of the rate 1/3 code: a coded bit is kept where the pattern says 1 (lr_fhss_build_frame)
constexpr uint8_t kPuncture[15] = {1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0};
int punctureLen(int cr) { return cr == Cr5_6 ? 15 : cr == Cr2_3 ? 6 : cr == Cr1_2 ? 3 : 0; }

inline int parity(unsigned v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1u);
}
inline int getBit(const std::vector<uint8_t>& v, size_t i) { return (v[i >> 3] >> (7 - (i & 7))) & 1; }
inline void setBit(std::vector<uint8_t>& v, size_t i, int b) {
    const uint8_t m = (uint8_t)(1u << (7 - (i & 7)));
    v[i >> 3] = (uint8_t)(b ? (v[i >> 3] | m) : (v[i >> 3] & ~m));
}
std::vector<uint8_t> bytesToBits(const uint8_t* p, size_t nBits) {
    std::vector<uint8_t> b(nBits);
    for (size_t i = 0; i < nBits; i++) b[i] = (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
    return b;
}

// encoder: n outputs per input bit, the register starts at `state`
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& in, int K, const unsigned* gens, int n, unsigned state) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * (size_t)n);
    const unsigned mask = (1u << (K - 1)) - 1;
    for (uint8_t b : in) {
        const unsigned reg = (state << 1) | b;
        for (int g = 0; g < n; g++) out.push_back((uint8_t)parity(reg & gens[g]));
        state = reg & mask;
    }
    return out;
}

// Soft Viterbi over `steps` input bits (soft: n values per step, > 0 = 1, 0 = erased). startState / endState < 0: free. Returns the path metric.
double viterbi(const float* soft, size_t steps, int K, const unsigned* gens, int n, int startState, int endState, std::vector<uint8_t>& bits) {
    const int S = 1 << (K - 1);
    const unsigned mask = (unsigned)S - 1;
    const double neg = -std::numeric_limits<double>::infinity();
    std::vector<double> m(S, neg), nm(S);
    if (startState >= 0) m[(size_t)startState] = 0; else std::fill(m.begin(), m.end(), 0.0);
    std::vector<uint8_t> dec(steps * (size_t)S);
    // branch outputs (as +-1) for every register value
    std::vector<int8_t> outs((size_t)(2 * S) * (size_t)n);
    for (unsigned reg = 0; reg < (unsigned)(2 * S); reg++)
        for (int g = 0; g < n; g++) outs[reg * (unsigned)n + (unsigned)g] = (int8_t)(parity(reg & gens[g]) ? 1 : -1);
    for (size_t t = 0; t < steps; t++) {
        const float* s = soft + t * (size_t)n;
        std::fill(nm.begin(), nm.end(), neg);
        for (unsigned st = 0; st < (unsigned)S; st++) {
            if (m[st] == neg) continue;
            for (unsigned b = 0; b < 2; b++) {
                const unsigned reg = (st << 1) | b;
                double bm = 0;
                for (int g = 0; g < n; g++) bm += (double)s[g] * outs[reg * (unsigned)n + (unsigned)g];
                const unsigned ns = reg & mask;
                const double v = m[st] + bm;
                if (v > nm[ns]) { nm[ns] = v; dec[t * (size_t)S + ns] = (uint8_t)(st >> (K - 2)); }   // the bit that falls out of the register
            }
        }
        m.swap(nm);
    }
    unsigned st = 0;
    if (endState >= 0) st = (unsigned)endState;
    else for (unsigned i = 1; i < (unsigned)S; i++) if (m[i] > m[st]) st = i;
    const double metric = m[st];
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(st & 1u);
        const unsigned top = dec[t * (size_t)S + st];
        st = (st >> 1) | (top << (K - 2));
    }
    return metric;
}

uint16_t isqrtUp(uint16_t x) { uint16_t y = 0; while ((unsigned)y * y < x) y++; return y; }
} // namespace

const uint8_t kHeaderInterleaver[80] = {
    0,  18, 36, 54, 72, 4,  22, 40, 58, 76, 8,  26, 44, 62, 12, 30, 48, 66, 16, 34, 52, 70, 1,  19, 37, 55, 73, 5,  23, 41, 59, 77,
    9,  27, 45, 63, 13, 31, 49, 67, 17, 35, 53, 71, 2,  20, 38, 56, 74, 6,  24, 42, 60, 78, 10, 28, 46, 64, 14, 32, 50, 68, 3,  21,
    39, 57, 75, 7,  25, 43, 61, 79, 11, 29, 47, 65, 15, 33, 51, 69};

const char* crName(int cr) {
    switch (cr) { case Cr5_6: return "5/6"; case Cr2_3: return "2/3"; case Cr1_2: return "1/2"; case Cr1_3: return "1/3"; default: return "?"; }
}
double occupiedHz(int bw) { return bw >= 0 && bw < 10 ? kOcwHz[bw] : 0; }
double gridHz(int grid) { return grid == Grid3906 ? 8 * kStepHz : 52 * kStepHz; }
int channelCount(int bw) { return bw >= 0 && bw < 10 ? kChannelCount[bw] : 0; }
int hopSequenceCount(const Params& p) { return (p.grid == Grid25391 || (p.grid == Grid3906 && p.bw < 4)) ? 384 : 512; }

HopParams hopParams(const Params& p, int id) {
    HopParams h;
    if (p.bw < 0 || p.bw > 9 || id < 0 || id > 511) return h;
    h.nGrid = channelCount(p.bw) / (p.grid == Grid3906 ? 8 : 52);
    h.id = (uint16_t)id;
    switch (h.nGrid) {
    case 10: case 22: case 28: case 30: case 35: case 47:
        if (id >= 384) return h;
        h.initState = 6; h.polynomial = kPoly1[id >> 6]; h.xoringSeed = (uint16_t)(id & 0x3F); break;
    case 60: case 62:
        if (id >= 384) return h;
        h.initState = 56; h.polynomial = kPoly1[id >> 6]; h.xoringSeed = (uint16_t)(id & 0x3F); break;
    case 86: case 99:
        h.initState = 6; h.polynomial = kPoly2[id >> 7]; h.xoringSeed = (uint16_t)(id & 0x7F); break;
    case 185: case 198:
        h.initState = 6; h.polynomial = kPoly3[id >> 8]; h.xoringSeed = (uint16_t)(id & 0xFF); break;
    case 390: case 403:
        h.initState = 6; h.polynomial = 264; h.xoringSeed = (uint16_t)id; break;
    default: return h;
    }
    h.ok = true;
    return h;
}

namespace {
// lr_fhss_get_next_state
int nextState(uint16_t& lfsr, const HopParams& h) {
    uint16_t hop;
    do {
        const uint16_t lsb = lfsr & 1u;
        lfsr = (uint16_t)(lfsr >> 1);
        if (lsb) lfsr = (uint16_t)(lfsr ^ h.polynomial);
        hop = h.xoringSeed;
        if (hop != lfsr) hop = (uint16_t)(hop ^ lfsr);
    } while (hop > h.nGrid);
    return (int)hop - 1;
}
int toSigned(int ni, int nGrid) { return ni < (nGrid >> 1) ? ni : ni - nGrid; }
// position after `advances` steps of the LFSR from its start
int positionAt(const Params& p, const HopParams& h, int advances) {
    if (!p.hopping) return toSigned(h.id % h.nGrid, h.nGrid);
    uint16_t lfsr = h.initState;
    int ni = 0;
    for (int i = 0; i < advances; i++) ni = nextState(lfsr, h);
    return toSigned(ni, h.nGrid);
}
} // namespace

std::vector<int> gridSequence(const Params& p, int id, int hops) {
    std::vector<int> v;
    const HopParams h = hopParams(p, id);
    if (!h.ok) return v;
    uint16_t lfsr = h.initState;
    if (p.hopping) for (int i = 0; i < 4 - p.headers; i++) nextState(lfsr, h);
    for (int k = 0; k < hops; k++) v.push_back(p.hopping ? toSigned(nextState(lfsr, h), h.nGrid) : toSigned(h.id % h.nGrid, h.nGrid));
    return v;
}
int gridPositionHeader(const Params& p, int id, int s) {
    const HopParams h = hopParams(p, id);
    return h.ok ? positionAt(p, h, 4 - s) : 0;
}
int gridPositionPayload(const Params& p, int id, int j) {
    const HopParams h = hopParams(p, id);
    return h.ok ? positionAt(p, h, 5 + j) : 0;
}

double hopOffsetHz(const Params& p, int nGrid, int position, int deviceOffset, int syncIndex) {
    const int perCell = p.grid == Grid3906 ? 8 : 52;
    const int gridOffset = (1 + (nGrid % 2)) * (perCell / 2);
    double f = -(double)position * gridHz(p.grid) - (double)(deviceOffset + gridOffset) * kStepHz;
    if (p.hopping && syncIndex >= 0 && (syncIndex % 2) == 1) f += kStepHz / 2;
    return f;
}

Digest digest(const Params& p, size_t len) {
    Digest d;
    int lengthBits = (int)(len + 2) * 8 + 6;
    switch (p.cr) {
    case Cr5_6: lengthBits = (lengthBits * 6 + 4) / 5; break;
    case Cr2_3: lengthBits = lengthBits * 3 / 2; break;
    case Cr1_2: lengthBits = lengthBits * 2; break;
    default: lengthBits = lengthBits * 3; break;
    }
    d.codedBits = lengthBits;
    d.hops = (lengthBits + kFragBits - 1) / kFragBits + p.headers;
    int payloadBits = lengthBits / kFragBits * kBlockBits;
    const int last = lengthBits % kFragBits;
    if (last > 0) payloadBits += last + 2;
    d.bits = kHeaderBits * p.headers + payloadBits;
    d.bytes = (d.bits + 7) / 8;
    if (!p.hopping) d.hops = 1;
    return d;
}
double airSeconds(const Params& p, size_t len) { return digest(p, len).bits / kBitRate; }
uint32_t airMs(const Params& p, size_t len) { return (uint32_t)((((uint32_t)digest(p, len).bits << 8) + 124) / 125); }

uint8_t headerCrc8(const uint8_t* p, size_t n) {
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x2F : c << 1);
    }
    return c;
}
uint16_t payloadCrc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x755B : c << 1);
    }
    return c;
}
void whiten(const uint8_t* in, size_t n, uint8_t* out) {
    uint8_t lfsr = 0xFF;
    for (size_t i = 0; i < n; i++) {
        const uint8_t u = (uint8_t)(in[i] ^ lfsr);
        out[i] = (uint8_t)(((u & 0x0F) << 4) | ((u & 0xF0) >> 4));
        lfsr = (uint8_t)((lfsr << 1) | (((lfsr & 0x80) >> 7) ^ (((lfsr & 0x20) >> 5) ^ (((lfsr & 0x10) >> 4) ^ ((lfsr & 0x8) >> 3)))));
    }
}
void dewhiten(const uint8_t* in, size_t n, uint8_t* out) {
    uint8_t lfsr = 0xFF;
    for (size_t i = 0; i < n; i++) {
        const uint8_t u = (uint8_t)(((in[i] & 0x0F) << 4) | ((in[i] & 0xF0) >> 4));
        out[i] = (uint8_t)(u ^ lfsr);
        lfsr = (uint8_t)((lfsr << 1) | (((lfsr & 0x80) >> 7) ^ (((lfsr & 0x20) >> 5) ^ (((lfsr & 0x10) >> 4) ^ ((lfsr & 0x8) >> 3)))));
    }
}

std::array<uint8_t, 5> rawHeader(const Params& p, int id, size_t len, int s) {
    std::array<uint8_t, 5> h{};
    h[0] = (uint8_t)len;
    h[1] = (uint8_t)((0 << 5) + (p.cr << 3) + (p.grid << 2) + (p.hopping ? 2 : 0) + (p.bw >> 3));
    h[2] = (uint8_t)(((p.bw & 0x07) << 5) + (id >> 4));
    h[3] = (uint8_t)(((id & 0x0F) << 4) | ((s & 3) << 2));
    h[4] = headerCrc8(h.data(), 4);
    return h;
}

std::vector<uint16_t> interleaverOrder(int nbits) {
    std::vector<uint16_t> order;
    if (nbits <= 0) return order;
    order.reserve((size_t)nbits);
    uint16_t step = isqrtUp((uint16_t)nbits);
    const uint16_t stepV = (uint16_t)(step >> 1);
    step = (uint16_t)(step << 1);
    uint16_t pos = 0, stIdx = 0, stIdxInit = 0;
    for (int j = 0; j < nbits; j++) {
        order.push_back(pos);
        pos = (uint16_t)(pos + step);
        if (pos >= nbits) {
            stIdx = (uint16_t)(stIdx + stepV);
            if (stIdx >= step) { stIdxInit++; stIdx = stIdxInit; }
            pos = stIdx;
        }
    }
    return order;
}

std::vector<uint8_t> headerAirBits(const std::array<uint8_t, 5>& raw) {
    const std::vector<uint8_t> in = bytesToBits(raw.data(), kHalfHeaderBits);
    // tail biting: the register starts with the last K - 1 bits (Semtech encodes twice and keeps the second pass)
    unsigned st = 0;
    for (int i = kHalfHeaderBits - (kK12 - 1); i < kHalfHeaderBits; i++) st = (st << 1) | in[(size_t)i];
    const std::vector<uint8_t> coded = convEncode(in, kK12, kG12, 2, st);
    std::vector<uint8_t> air(80);
    for (int j = 0; j < 80; j++) air[(size_t)j] = coded[kHeaderInterleaver[j]];
    return air;
}

std::vector<uint8_t> frameBits(const Params& p, int id, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> bits;
    const Digest d = digest(p, n);
    // payload: whiten, CRC over the whitened bytes, 6 zero tail bits, rate 1/3, puncture, interleave in blocks of 48 with 2 guard bits each
    std::vector<uint8_t> w(n + 3, 0);
    whiten(payload, n, w.data());
    const uint16_t crc = payloadCrc16(w.data(), n);
    w[n] = (uint8_t)(crc >> 8); w[n + 1] = (uint8_t)crc;
    const std::vector<uint8_t> info = bytesToBits(w.data(), 8 * (n + 2) + 6);
    std::vector<uint8_t> coded = convEncode(info, kK13, kG13, 3, 0);
    if (p.cr != Cr1_3) {
        const int pl = punctureLen(p.cr);
        std::vector<uint8_t> kept;
        for (size_t i = 0; i < coded.size(); i++) if (kPuncture[i % (size_t)pl]) kept.push_back(coded[i]);
        coded.swap(kept);
    }
    const std::vector<uint16_t> order = interleaverOrder((int)coded.size());
    // headers
    for (int i = 0; i < p.headers; i++) {
        const std::vector<uint8_t> air = headerAirBits(rawHeader(p, id, n, p.headers - i - 1));
        bits.push_back(0); bits.push_back(0);
        bits.insert(bits.end(), air.begin(), air.begin() + kHalfHeaderBits);
        for (int j = 0; j < kSyncBits; j++) bits.push_back((uint8_t)((p.syncWord >> (31 - j)) & 1u));
        bits.insert(bits.end(), air.begin() + kHalfHeaderBits, air.end());
    }
    for (size_t j = 0; j < order.size(); j++) {
        if (j % kFragBits == 0) { bits.push_back(0); bits.push_back(0); }
        bits.push_back(coded[order[j]]);
    }
    (void)d;
    return bits;
}

std::vector<uint8_t> buildFrame(const Params& p, int id, const uint8_t* payload, size_t n) {
    const std::vector<uint8_t> bits = frameBits(p, id, payload, n);
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) setBit(out, i, bits[i]);
    return out;
}

Header parseHeader(const std::array<uint8_t, 5>& r) {
    Header h;
    h.raw = r;
    h.len = r[0];
    h.modulation = r[1] >> 5;
    h.cr = (r[1] >> 3) & 3;
    h.grid = (r[1] >> 2) & 1;
    h.hopping = (r[1] & 2) != 0;
    h.bw = ((r[1] & 1) << 3) | (r[2] >> 5);
    h.hopSeqId = ((r[2] & 0x1F) << 4) | (r[3] >> 4);
    h.syncIndex = (r[3] >> 2) & 3;
    Params p; p.grid = h.grid; p.bw = h.bw;
    h.ok = headerCrc8(r.data(), 4) == r[4] && h.modulation == 0 && (r[1] & 0x80) == 0 && (r[3] & 3) == 0 && h.bw <= 9 && h.len >= 1 &&
           (h.grid == Grid3906 || h.bw >= 6) && h.hopSeqId < hopSequenceCount(p);
    return h;
}

Header decodeHeader(const float* soft80) {
    // back to code order, then the tail-biting trellis: every start state, ending in the same state
    std::vector<float> code(80, 0.f);
    for (int j = 0; j < 80; j++) code[kHeaderInterleaver[j]] = soft80[j];
    double best = -std::numeric_limits<double>::infinity();
    std::vector<uint8_t> bestBits, bits;
    for (int s = 0; s < (1 << (kK12 - 1)); s++) {
        const double m = viterbi(code.data(), kHalfHeaderBits, kK12, kG12, 2, s, s, bits);
        if (m > best) { best = m; bestBits = bits; }
    }
    std::array<uint8_t, 5> raw{};
    for (int i = 0; i < kHalfHeaderBits; i++) if (bestBits[(size_t)i]) raw[(size_t)(i >> 3)] = (uint8_t)(raw[(size_t)(i >> 3)] | (1u << (7 - (i & 7))));
    return parseHeader(raw);
}

bool decodePayload(int cr, size_t len, const float* soft, std::vector<uint8_t>& payload, int* bitErrors) {
    Params p; p.cr = cr;
    const Digest d = digest(p, len);
    const int n = d.codedBits;
    const std::vector<uint16_t> order = interleaverOrder(n);
    std::vector<float> punct((size_t)n, 0.f);
    for (int j = 0; j < n; j++) punct[order[(size_t)j]] = soft[j];
    const size_t steps = 8 * (len + 2) + 6;
    std::vector<float> full(steps * 3, 0.f);
    if (cr == Cr1_3) {
        for (size_t i = 0; i < full.size() && i < punct.size(); i++) full[i] = punct[i];
    } else {
        const int pl = punctureLen(cr);
        size_t k = 0;
        for (size_t i = 0; i < full.size() && k < punct.size(); i++) if (kPuncture[i % (size_t)pl]) full[i] = punct[k++];
    }
    std::vector<uint8_t> bits;
    viterbi(full.data(), steps, kK13, kG13, 3, 0, 0, bits);
    if (bitErrors) {
        const std::vector<uint8_t> re = convEncode(bits, kK13, kG13, 3, 0);
        int e = 0;
        for (size_t i = 0; i < re.size(); i++) if (full[i] != 0.f && ((full[i] > 0) != (re[i] != 0))) e++;
        *bitErrors = e;
    }
    std::vector<uint8_t> w(len + 2, 0);
    for (size_t i = 0; i < 8 * (len + 2); i++) if (bits[i]) w[i >> 3] = (uint8_t)(w[i >> 3] | (1u << (7 - (i & 7))));
    const uint16_t crc = payloadCrc16(w.data(), len);
    payload.assign(len, 0);
    dewhiten(w.data(), len, payload.data());
    return crc == (uint16_t)((w[len] << 8) | w[len + 1]);
}

} // namespace lrfhss
} // namespace dect2
