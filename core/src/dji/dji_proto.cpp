// DJI DroneID: see dji_proto.h.
#include "dect2/dji/dji_proto.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/mathconst.h"

namespace dect2::dji {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int kD = kTurboK + 4;          // bits per turbo stream with the tail
constexpr int kR = 45;                   // rows of the sub-block interleaver: ceil(1412 / 32)
constexpr int kKpi = kR * 32;            // 1440
constexpr int kNd = kKpi - kD;           // 28 dummy bits
constexpr int kP[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30, 1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};
constexpr int kQppF1 = 43, kQppF2 = 88;  // 36.212 table 5.1.3-3, K = 1408

// the 8-state trellis of the LTE constituent code: feedback 1 + D^2 + D^3, parity 1 + D + D^3
struct Trellis {
    int next[8][2];
    int par[8][2];
    Trellis() {
        for (int s = 0; s < 8; s++)
            for (int u = 0; u < 2; u++) {
                const int s0 = s & 1, s1 = (s >> 1) & 1, s2 = (s >> 2) & 1;
                const int a = u ^ s1 ^ s2;
                par[s][u] = a ^ s0 ^ s2;
                next[s][u] = a | (s0 << 1) | (s1 << 2);
            }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

// (stream, index into the stream's 1412 bits) of every position of the circular buffer after sub-block interleaving; index -1: a dummy bit
struct RmMap {
    std::vector<std::array<int, 2>> e;   // the kBurstBits transmitted bits
    RmMap() {
        std::vector<std::array<int, 2>> w(3 * kKpi);
        auto y = [](int pos) { return pos < kNd ? -1 : pos - kNd; };
        for (int k = 0; k < kKpi; k++) {
            const int col = k / kR, row = k % kR;
            const int p01 = row * 32 + kP[col];
            const int p2 = (kP[col] + 32 * row + 1) % kKpi;
            w[k] = {0, y(p01)};
            w[kKpi + 2 * k] = {1, y(p01)};
            w[kKpi + 2 * k + 1] = {2, y(p2)};
        }
        const int k0 = kR * 2;
        for (int j = 0; (int)e.size() < kBurstBits; j++) {
            const auto& v = w[(size_t)((k0 + j) % (3 * kKpi))];
            if (v[1] >= 0) e.push_back(v);
        }
    }
};
const RmMap& rmMap() { static const RmMap m; return m; }

std::vector<int> qpp() {
    std::vector<int> p(kTurboK);
    for (int i = 0; i < kTurboK; i++) p[(size_t)i] = (int)(((long long)kQppF1 * i + (long long)kQppF2 * i * i) % kTurboK);
    return p;
}

// max-log-MAP over n steps from state 0 to state 0; ls: systematic + a priori, lp: parity; out: a posteriori LLR of the input (positive: 0)
void bcjr(int n, const float* ls, const float* lp, float* out, std::vector<float>& alpha) {
    const Trellis& T = trellis();
    constexpr float kNeg = -1e9f;
    alpha.assign((size_t)(n + 1) * 8, kNeg);
    alpha[0] = 0;
    for (int k = 0; k < n; k++) {
        const float* a = &alpha[(size_t)k * 8];
        float* b = &alpha[(size_t)(k + 1) * 8];
        for (int s = 0; s < 8; s++) {
            if (a[s] <= kNeg / 2) continue;
            for (int u = 0; u < 2; u++) {
                const float g = 0.5f * ((u ? -1.f : 1.f) * ls[k] + (T.par[s][u] ? -1.f : 1.f) * lp[k]);
                const int ns = T.next[s][u];
                b[ns] = std::max(b[ns], a[s] + g);
            }
        }
    }
    float beta[8], nb[8];
    for (int s = 0; s < 8; s++) beta[s] = s == 0 ? 0.f : kNeg;
    for (int k = n - 1; k >= 0; k--) {
        const float* a = &alpha[(size_t)k * 8];
        float m0 = kNeg, m1 = kNeg;
        for (int s = 0; s < 8; s++) nb[s] = kNeg;
        for (int s = 0; s < 8; s++)
            for (int u = 0; u < 2; u++) {
                const float g = 0.5f * ((u ? -1.f : 1.f) * ls[k] + (T.par[s][u] ? -1.f : 1.f) * lp[k]);
                const int ns = T.next[s][u];
                const float v = a[s] + g + beta[ns];
                if (u) m1 = std::max(m1, v); else m0 = std::max(m0, v);
                nb[s] = std::max(nb[s], g + beta[ns]);
            }
        out[k] = m0 - m1;
        for (int s = 0; s < 8; s++) beta[s] = nb[s];
    }
}

// the LTE encoder of one constituent code: input bits c (n of them), parity z (n), then the termination: three input bits and three parity bits
void rscEncode(const std::vector<uint8_t>& c, std::vector<uint8_t>& z, uint8_t xt[3], uint8_t zt[3]) {
    int s0 = 0, s1 = 0, s2 = 0;
    z.resize(c.size());
    for (size_t k = 0; k < c.size(); k++) {
        const int a = c[k] ^ s1 ^ s2;
        z[k] = (uint8_t)(a ^ s0 ^ s2);
        s2 = s1; s1 = s0; s0 = a;
    }
    for (int t = 0; t < 3; t++) {
        const int u = s1 ^ s2, a = 0;
        xt[t] = (uint8_t)u;
        zt[t] = (uint8_t)(a ^ s0 ^ s2);
        s2 = s1; s1 = s0; s0 = a;
    }
}

int popbit(uint32_t v, int i) { return (int)((v >> i) & 1u); }
} // namespace

std::vector<cf32> zcFreq(int root) {
    std::vector<cf32> z;
    z.reserve(kCarriers);
    for (int n = 0; n <= 600; n++) {
        if (n == 300) continue;
        const double ph = -dect2::math::kPi * root * (double)n * (double)(n + 1) / 601.0;
        z.emplace_back((float)std::cos(ph), (float)std::sin(ph));
    }
    return z;
}

std::vector<cf32> zcTime(int root) {
    const std::vector<cf32> z = zcFreq(root);
    std::vector<cf32> f(kFft, cf32(0, 0));
    for (int k = 0; k < kCarriers; k++) f[(size_t)carrierBin(k)] = z[(size_t)k];
    Fft fft(kFft);
    fft.inverse(f.data());
    return f;
}

std::vector<uint8_t> scramblerBits(int n, int variant) {
    uint32_t seed = 0x12345678u;
    if (variant == 1) { uint32_t r = 0; for (int i = 0; i < 31; i++) r |= (uint32_t)popbit(seed, i) << (30 - i); seed = r; }
    const int nc = 1600;
    std::vector<uint8_t> x1((size_t)(nc + n + 31)), x2((size_t)(nc + n + 31));
    x1[0] = 1;
    for (int i = 0; i < 31; i++) x2[(size_t)i] = (uint8_t)popbit(seed, i);
    for (int i = 0; i < nc + n; i++) {
        x1[(size_t)(i + 31)] = x1[(size_t)(i + 3)] ^ x1[(size_t)i];
        x2[(size_t)(i + 31)] = x2[(size_t)(i + 3)] ^ x2[(size_t)(i + 2)] ^ x2[(size_t)(i + 1)] ^ x2[(size_t)i];
    }
    std::vector<uint8_t> c((size_t)n);
    for (int i = 0; i < n; i++) c[(size_t)i] = x1[(size_t)(i + nc)] ^ x2[(size_t)(i + nc)];
    return c;
}

uint32_t crc24a(const uint8_t* d, size_t n) {
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << 16;
        for (int b = 0; b < 8; b++) crc = (crc & 0x800000u) ? ((crc << 1) ^ 0x864CFBu) : (crc << 1);
        crc &= 0xFFFFFFu;
    }
    return crc;
}

uint16_t crc16Duml(const uint8_t* d, size_t n) { return crc::crc16Kermit(d, n, 0x3692); }

std::vector<int> qppInterleaver() { return qpp(); }

std::vector<uint8_t> turboEncode(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> c(kTurboK);
    for (int i = 0; i < kTurboK; i++) c[(size_t)i] = (uint8_t)((bytes[(size_t)(i / 8)] >> (7 - i % 8)) & 1);
    const std::vector<int> pi = qpp();
    std::vector<uint8_t> ci(kTurboK);
    for (int i = 0; i < kTurboK; i++) ci[(size_t)i] = c[(size_t)pi[(size_t)i]];
    std::vector<uint8_t> z1, z2;
    uint8_t x1[3], t1[3], x2[3], t2[3];
    rscEncode(c, z1, x1, t1);
    rscEncode(ci, z2, x2, t2);
    std::array<std::vector<uint8_t>, 3> d;
    for (auto& v : d) v.assign(kD, 0);
    for (int k = 0; k < kTurboK; k++) { d[0][(size_t)k] = c[(size_t)k]; d[1][(size_t)k] = z1[(size_t)k]; d[2][(size_t)k] = z2[(size_t)k]; }
    const int K = kTurboK;
    d[0][(size_t)K] = x1[0];     d[0][(size_t)K + 1] = t1[1]; d[0][(size_t)K + 2] = x2[0]; d[0][(size_t)K + 3] = t2[1];
    d[1][(size_t)K] = t1[0];     d[1][(size_t)K + 1] = x1[2]; d[1][(size_t)K + 2] = t2[0]; d[1][(size_t)K + 3] = x2[2];
    d[2][(size_t)K] = x1[1];     d[2][(size_t)K + 1] = t1[2]; d[2][(size_t)K + 2] = x2[1]; d[2][(size_t)K + 3] = t2[2];
    std::vector<uint8_t> e;
    e.reserve(kBurstBits);
    for (const auto& m : rmMap().e) e.push_back(d[(size_t)m[0]][(size_t)m[1]]);
    return e;
}

bool turboDecode(const std::vector<float>& llr, std::vector<uint8_t>& out, int iterations) {
    if ((int)llr.size() < kBurstBits) return false;
    // rate de-matching: every transmitted copy of a bit adds its LLR
    std::array<std::vector<float>, 3> d;
    for (auto& v : d) v.assign(kD, 0.f);
    const auto& map = rmMap().e;
    for (int j = 0; j < kBurstBits; j++) d[(size_t)map[(size_t)j][0]][(size_t)map[(size_t)j][1]] += llr[(size_t)j];
    const int K = kTurboK, N = K + 3;
    const std::vector<int> pi = qpp();
    std::vector<float> sys1((size_t)N), par1((size_t)N), sys2((size_t)N), par2((size_t)N);
    for (int k = 0; k < K; k++) { sys1[(size_t)k] = d[0][(size_t)k]; par1[(size_t)k] = d[1][(size_t)k]; par2[(size_t)k] = d[2][(size_t)k]; }
    for (int k = 0; k < K; k++) sys2[(size_t)k] = sys1[(size_t)pi[(size_t)k]];
    sys1[(size_t)K] = d[0][(size_t)K];  sys1[(size_t)K + 1] = d[2][(size_t)K];  sys1[(size_t)K + 2] = d[1][(size_t)K + 1];
    par1[(size_t)K] = d[1][(size_t)K];  par1[(size_t)K + 1] = d[0][(size_t)K + 1];  par1[(size_t)K + 2] = d[2][(size_t)K + 1];
    sys2[(size_t)K] = d[0][(size_t)K + 2];  sys2[(size_t)K + 1] = d[2][(size_t)K + 2];  sys2[(size_t)K + 2] = d[1][(size_t)K + 3];
    par2[(size_t)K] = d[1][(size_t)K + 2];  par2[(size_t)K + 1] = d[0][(size_t)K + 3];  par2[(size_t)K + 2] = d[2][(size_t)K + 3];
    std::vector<float> la1((size_t)N, 0.f), la2((size_t)N, 0.f), ls((size_t)N), o1((size_t)N), o2((size_t)N), ext1((size_t)N, 0.f), alpha;
    out.assign(kFrameBytes, 0);
    std::vector<uint8_t> bits(kTurboK);
    for (int it = 0; it < iterations; it++) {
        for (int k = 0; k < N; k++) ls[(size_t)k] = sys1[(size_t)k] + la1[(size_t)k];
        bcjr(N, ls.data(), par1.data(), o1.data(), alpha);
        for (int k = 0; k < N; k++) ext1[(size_t)k] = 0.75f * (o1[(size_t)k] - ls[(size_t)k]);
        for (int k = 0; k < K; k++) la2[(size_t)k] = ext1[(size_t)pi[(size_t)k]];
        for (int k = K; k < N; k++) la2[(size_t)k] = 0.f;
        for (int k = 0; k < N; k++) ls[(size_t)k] = sys2[(size_t)k] + la2[(size_t)k];
        bcjr(N, ls.data(), par2.data(), o2.data(), alpha);
        std::fill(la1.begin(), la1.end(), 0.f);
        for (int k = 0; k < K; k++) {
            la1[(size_t)pi[(size_t)k]] = 0.75f * (o2[(size_t)k] - ls[(size_t)k]);
            bits[(size_t)pi[(size_t)k]] = o2[(size_t)k] < 0 ? 1 : 0;
        }
        std::fill(out.begin(), out.end(), 0);
        for (int k = 0; k < K; k++) out[(size_t)(k / 8)] |= (uint8_t)(bits[(size_t)k] << (7 - k % 8));
        if (crc24a(out.data(), out.size()) == 0) return true;
    }
    return false;
}

std::vector<uint8_t> packDroneId(const DroneId& d) {
    std::vector<uint8_t> f(kFrameBytes, 0);
    auto p16 = [&](int o, int v) { f[(size_t)o] = (uint8_t)(v & 0xFF); f[(size_t)o + 1] = (uint8_t)((v >> 8) & 0xFF); };
    auto p32 = [&](int o, int32_t v) { for (int i = 0; i < 4; i++) f[(size_t)o + (size_t)i] = (uint8_t)(((uint32_t)v >> (8 * i)) & 0xFF); };
    f[0] = (uint8_t)d.pktLen; f[1] = (uint8_t)d.zeroByte; f[2] = (uint8_t)d.version;
    p16(3, d.seq); p16(5, d.state);
    for (size_t i = 0; i < 16 && i < d.serial.size(); i++) f[7 + i] = (uint8_t)d.serial[i];
    p32(0x17, d.lonRaw); p32(0x1b, d.latRaw);
    p16(0x1f, d.altRaw); p16(0x21, d.heightRaw); p16(0x23, d.vNorth); p16(0x25, d.vEast); p16(0x27, d.vUp); p16(0x29, d.yaw);
    for (int i = 0; i < 8; i++) f[0x2b + (size_t)i] = (uint8_t)((d.gpsMs >> (8 * i)) & 0xFF);
    p32(0x33, d.appLatRaw); p32(0x37, d.appLonRaw); p32(0x3b, d.homeLonRaw); p32(0x3f, d.homeLatRaw);
    f[0x43] = (uint8_t)d.deviceType; f[0x44] = (uint8_t)d.uuidLen;
    for (size_t i = 0; i < 20 && i < d.uuid.size(); i++) f[0x45 + i] = (uint8_t)d.uuid[i];
    p16(0x59, crc16Duml(f.data(), 0x59));
    const uint32_t c = crc24a(f.data(), kFrameBytes - 3);
    f[kFrameBytes - 3] = (uint8_t)(c >> 16); f[kFrameBytes - 2] = (uint8_t)(c >> 8); f[kFrameBytes - 1] = (uint8_t)c;
    return f;
}

bool parseDroneId(const std::vector<uint8_t>& f, DroneId& d) {
    if ((int)f.size() < kInfoBytes) return false;
    auto u16 = [&](int o) { return (int)f[(size_t)o] | ((int)f[(size_t)o + 1] << 8); };
    auto s16 = [&](int o) { return (int)(int16_t)u16(o); };
    auto s32 = [&](int o) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)f[(size_t)o + (size_t)i] << (8 * i); return (int32_t)v; };
    d = DroneId();
    d.pktLen = f[0]; d.zeroByte = f[1]; d.version = f[2];
    if (d.pktLen < 60 || d.pktLen > 120) return false;
    d.seq = u16(3); d.state = u16(5);
    d.serial.clear();
    for (int i = 0; i < 16 && f[7 + (size_t)i]; i++) d.serial += (f[7 + (size_t)i] >= 32 && f[7 + (size_t)i] < 127) ? (char)f[7 + (size_t)i] : '?';
    d.lonRaw = s32(0x17); d.latRaw = s32(0x1b);
    d.altRaw = s16(0x1f); d.heightRaw = s16(0x21); d.vNorth = s16(0x23); d.vEast = s16(0x25); d.vUp = s16(0x27); d.yaw = s16(0x29);
    d.gpsMs = 0;
    for (int i = 0; i < 8; i++) d.gpsMs |= (uint64_t)f[0x2b + (size_t)i] << (8 * i);
    d.appLatRaw = s32(0x33); d.appLonRaw = s32(0x37); d.homeLonRaw = s32(0x3b); d.homeLatRaw = s32(0x3f);
    d.deviceType = f[0x43]; d.uuidLen = f[0x44];
    for (int i = 0; i < 20 && f[0x45 + (size_t)i]; i++) d.uuid += (f[0x45 + (size_t)i] >= 32 && f[0x45 + (size_t)i] < 127) ? (char)f[0x45 + (size_t)i] : '?';
    d.crc16Ok = (uint16_t)u16(0x59) == crc16Duml(f.data(), 0x59);
    return true;
}

std::vector<cf32> buildBurst(const std::vector<uint8_t>& bits, bool withFirst) {
    Fft fft(kFft);
    const float sc = 1.f / std::sqrt((float)kCarriers);
    std::vector<cf32> out;
    out.reserve(kBurstSamples);
    const std::vector<cf32> z1 = zcFreq(kZcRoot[0]), z2 = zcFreq(kZcRoot[1]);
    size_t bi = 0;
    uint32_t lf = 0xACE1u;   // the first symbol: a fixed pattern (its content is not published)
    for (int s = 0; s < kSyms; s++) {
        if (s == 0 && !withFirst) continue;
        std::vector<cf32> f(kFft, cf32(0, 0));
        for (int k = 0; k < kCarriers; k++) {
            cf32 v;
            if (s == kZcSym[0]) v = z1[(size_t)k];
            else if (s == kZcSym[1]) v = z2[(size_t)k];
            else if (s == 0) { lf = lf * 1664525u + 1013904223u; v = qpsk((int)((lf >> 20) & 1), (int)((lf >> 21) & 1)); }
            else { const int b0 = bi < bits.size() ? bits[bi] : 0, b1 = bi + 1 < bits.size() ? bits[bi + 1] : 0; bi += 2; v = qpsk(b0, b1); }
            f[(size_t)carrierBin(k)] = v;
        }
        fft.inverse(f.data());
        for (auto& v : f) v *= sc;
        const int cp = cpLen(s);
        out.insert(out.end(), f.end() - cp, f.end());
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

// How well a symbol carries a Zadoff-Chu sequence, whatever the timing error (a linear phase across the carriers): the phase steps of Y x conj(Z)
// between neighbouring carriers add up when they match. About 0.04 for noise, 1 for a clean symbol. shift: whole carriers of frequency offset.
static float zcCoherence(const std::vector<cf32>& Y, const std::vector<cf32>& Z, int shift) {
    cf32 acc(0, 0);
    double den = 0;
    cf32 prev = Y[(size_t)((carrierBin(0) + shift + kFft) % kFft)] * std::conj(Z[0]);
    for (int k = 1; k < kCarriers; k++) {
        const cf32 h = Y[(size_t)((carrierBin(k) + shift + kFft) % kFft)] * std::conj(Z[(size_t)k]);
        if (k != 300) { acc += h * std::conj(prev); den += (double)std::abs(h) * std::abs(prev); }
        prev = h;
    }
    return (float)(std::abs(acc) / (den + 1e-12));
}

// The same measure for a symbol already in carrier order (600 values, as the Zadoff-Chu sequence): index k is carrier k, not an FFT bin.
static float zcCoherenceCarriers(const std::vector<cf32>& Y, const std::vector<cf32>& Z) {
    cf32 acc(0, 0);
    double den = 0;
    cf32 prev = Y[0] * std::conj(Z[0]);
    for (int k = 1; k < kCarriers; k++) {
        const cf32 h = Y[(size_t)k] * std::conj(Z[(size_t)k]);
        if (k != 300) { acc += h * std::conj(prev); den += (double)std::abs(h) * std::abs(prev); }
        prev = h;
    }
    return (float)(std::abs(acc) / (den + 1e-12));
}

BurstResult receiveBurst(const std::vector<cf32>& x) {
    BurstResult r;
    const int N = (int)x.size();
    const int w = 72;
    // symbol 1 (0-based) .. 8: cyclic-prefix starts relative to the start of symbol 1's prefix
    int off[8];
    off[0] = 0;
    for (int j = 1; j < 8; j++) off[j] = off[j - 1] + cpLen(j) + kFft;
    const int span = off[7] + w + kFft;
    if (N < span + 8) return r;
    // 1. the cyclic prefixes: the product of a sample and the one 1024 later, summed over the eight symbols, peaks at the start; its angle is the offset within one carrier spacing
    std::vector<cf32> P((size_t)(N - kFft) + 1, cf32(0, 0));
    std::vector<float> Pe((size_t)(N - kFft) + 1, 0.f);
    for (int i = 0; i < N - kFft; i++) {
        P[(size_t)i + 1] = P[(size_t)i] + x[(size_t)i] * std::conj(x[(size_t)i + kFft]);
        Pe[(size_t)i + 1] = Pe[(size_t)i] + 0.5f * (std::norm(x[(size_t)i]) + std::norm(x[(size_t)i + kFft]));
    }
    int best = -1;
    float bestM = 0;
    cf32 bestS(0, 0);
    for (int t = 0; t + span <= N; t++) {
        cf32 s(0, 0);
        float e = 0;
        for (int j = 0; j < 8; j++) { s += P[(size_t)(t + off[j] + w)] - P[(size_t)(t + off[j])]; e += Pe[(size_t)(t + off[j] + w)] - Pe[(size_t)(t + off[j])]; }
        const float m = std::abs(s) / (e + 1e-12f);
        if (m > bestM) { bestM = m; best = t; bestS = s; }
    }
    if (best < 0) return r;
    // eight cyclic prefixes in a row look the same when shifted by a symbol: the Zadoff-Chu symbol decides among the neighbours
    std::vector<cf32> y((size_t)N);
    auto derot = [&](double f) {
        const double step = -2 * dect2::math::kPi * f / kFs;
        cf32 ph(1, 0);
        const cf32 inc((float)std::cos(step), (float)std::sin(step));
        for (int n = 0; n < N; n++) { y[(size_t)n] = x[(size_t)n] * ph; ph *= inc; if ((n & 255) == 255) ph /= std::abs(ph); }
    };
    constexpr int kEarly = 36;
    Fft fft(kFft);
    std::vector<cf32> sym(kFft);
    auto symbolAt = [&](int t0, int i, std::vector<cf32>& V) -> bool {
        const int st = t0 + bodyStart(i) - kEarly;
        if (st < 0 || st + kFft > N) return false;
        std::copy(y.begin() + st, y.begin() + st + kFft, sym.begin());
        fft.forward(sym.data());
        V.assign(sym.begin(), sym.end());
        return true;
    };
    const std::vector<cf32> Z1 = zcFreq(kZcRoot[0]), Z2 = zcFreq(kZcRoot[1]);
    std::vector<cf32> Y1;
    int t0 = 0, bestS1 = 0;
    double f1 = 0;
    float bestC = -1;
    const int cand[] = {0, -8, 8, 1096, -1096, 1104, -1104, 2192, -2192};
    for (int dt : cand) {
        const int tau = best + dt;
        if (tau < 0 || tau + span > N) continue;
        cf32 s(0, 0);
        for (int j = 0; j < 8; j++) s += P[(size_t)(tau + off[j] + w)] - P[(size_t)(tau + off[j])];
        const double fc = -std::arg(s) / (2 * kPi * kFft) * kFs;
        derot(fc);
        const int tt = tau - (80 + kFft);
        if (!symbolAt(tt, kZcSym[0], Y1)) continue;
        const float c = zcCoherence(Y1, Z1, 0);
        if (c > bestC) { bestC = c; t0 = tt; f1 = fc; }
    }
    r.timing = t0;
    if (bestC < 0) return r;
    // The whole carriers of offset: a Zadoff-Chu sequence shifted by s carriers is the same sequence delayed by 1024 root s / 601 samples, which a timing error
    // cannot tell from it, but the two symbols (roots 600 and 147) move by different amounts: the difference of their delays gives s.
    {
        derot(f1);
        std::vector<cf32> A, B;
        if (!symbolAt(t0, kZcSym[0], A) || !symbolAt(t0, kZcSym[1], B)) return r;
        auto delay = [&](const std::vector<cf32>& Y, const std::vector<cf32>& Z) {
            cf32 acc(0, 0);
            cf32 prev = Y[(size_t)carrierBin(0)] * std::conj(Z[0]);
            for (int k = 1; k < kCarriers; k++) {
                const cf32 h = Y[(size_t)carrierBin(k)] * std::conj(Z[(size_t)k]);
                if (k != 300) acc += h * std::conj(prev);
                prev = h;
            }
            return -std::arg(acc) * kFft / (2 * kPi);
        };
        auto wrap = [](double v) { while (v > 512) v -= 1024; while (v < -512) v += 1024; return v; };
        const double m1 = delay(A, Z1), m2 = delay(B, Z2);
        double bestD = 1e9;
        for (int sh = -12; sh <= 12; sh++) {
            const double p1 = -1024.0 * kZcRoot[0] * sh / 601.0, p2 = -1024.0 * kZcRoot[1] * sh / 601.0;
            const double e1 = wrap(m1 - p1), e2 = wrap(m2 - p2);
            const double cost = std::abs(wrap(e1 - e2)) + 0.05 * std::abs(e1 - kEarly);
            if (cost < bestD) { bestD = cost; bestS1 = sh; }
        }
    }
    r.cfoHz = f1 + bestS1 * kSpacingHz;
    r.zc1 = bestC;
    derot(r.cfoHz);   // y: the burst at DC
    std::vector<cf32> V[kSyms];
    bool ok = true;
    for (int s = 1; s < kSyms; s++) {
        std::vector<cf32> F;
        if (!symbolAt(t0, s, F)) { ok = false; break; }
        V[s].resize(kCarriers);
        for (int k = 0; k < kCarriers; k++) V[s][(size_t)k] = F[(size_t)carrierBin(k)];
    }
    if (!ok) return r;
    r.zc1 = zcCoherenceCarriers(V[kZcSym[0]], Z1);
    r.zc2 = zcCoherenceCarriers(V[kZcSym[1]], Z2);
    r.zc = r.zc1 > 0.3f && r.zc2 > 0.3f;
    if (!r.zc) return r;
    // 3. channel from the two Zadoff-Chu symbols; the common phase step between them carries any offset left
    std::vector<cf32> H1(kCarriers), H2(kCarriers);
    cf32 cross(0, 0);
    for (int k = 0; k < kCarriers; k++) {
        H1[(size_t)k] = V[kZcSym[0]][(size_t)k] * std::conj(Z1[(size_t)k]);
        H2[(size_t)k] = V[kZcSym[1]][(size_t)k] * std::conj(Z2[(size_t)k]);
        cross += H2[(size_t)k] * std::conj(H1[(size_t)k]);
    }
    const float phi = std::arg(cross);
    const cf32 rot = std::polar(1.f, -phi);
    std::vector<cf32> Hb(kCarriers);
    double sig = 0, noi = 0;
    for (int k = 0; k < kCarriers; k++) {
        Hb[(size_t)k] = 0.5f * (H1[(size_t)k] + H2[(size_t)k] * rot);
        sig += std::norm(Hb[(size_t)k]);
        noi += std::norm(H1[(size_t)k] - H2[(size_t)k] * rot);
    }
    sig /= kCarriers;
    const double sigma2 = std::max(noi / (2.0 * kCarriers), sig * 1e-4);
    r.snrDb = (float)(10 * std::log10(sig / sigma2));
    // 4. soft bits of the six data symbols
    std::vector<float> llr(kBurstBits);
    std::vector<cf32> eq((size_t)kBurstBits / 2);
    for (int si = 0; si < 6; si++) {
        const int s = kDataSym[si];
        const float ph = phi * (s - kZcSym[0]) / 2.f;
        const cf32 g = std::polar(1.f, ph);
        for (int k = 0; k < kCarriers; k++) {
            const cf32 H = Hb[(size_t)k] * g;
            eq[(size_t)(si * kCarriers + k)] = V[s][(size_t)k] * std::conj(H) * (float)(2.0 * std::sqrt(2.0) / sigma2);
        }
    }
    // 5. descramble and decode; the orientation of the constellation and the seed convention are tried until the CRC-24 holds
    std::vector<uint8_t> frame;
    for (int var = 0; var < 2 && !r.crcOk; var++) {
        const std::vector<uint8_t> scr = scramblerBits(kBurstBits, var);
        for (int rot4 = 0; rot4 < 4 && !r.crcOk; rot4++) {
            const cf32 q = std::polar(1.f, -kPi / 2 * rot4);
            for (int i = 0; i < kBurstBits / 2; i++) {
                const cf32 v = eq[(size_t)i] * q;
                llr[(size_t)(2 * i)] = scr[(size_t)(2 * i)] ? -v.real() : v.real();
                llr[(size_t)(2 * i + 1)] = scr[(size_t)(2 * i + 1)] ? -v.imag() : v.imag();
            }
            if (turboDecode(llr, frame, 8)) { r.crcOk = true; r.variant = var; r.rotation = rot4; }
        }
    }
    if (r.crcOk) parseDroneId(frame, r.id);
    return r;
}

} // namespace dect2::dji
