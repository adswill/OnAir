#include "dect2/dvbt.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace dect2 {
namespace dvbt {

const char* guardName(int g) { static const char* n[] = {"1/32", "1/16", "1/8", "1/4"}; return g >= 0 && g < 4 ? n[g] : "?"; }
const char* modName(int m) { static const char* n[] = {"QPSK", "16-QAM", "64-QAM"}; return m >= 0 && m < 3 ? n[m] : "?"; }
const char* rateName(int r) { static const char* n[] = {"1/2", "2/3", "3/4", "5/6", "7/8"}; return r >= 0 && r < 5 ? n[r] : "?"; }

// ============================================================================ carrier tables (EN 300 744 4.5)
static const int kCp2k[45] = {0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636, 714, 759, 765, 780, 804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704};
static const int kCp8k[177] = {
    0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636, 714, 759, 765, 780, 804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206,
    1269, 1323, 1377, 1491, 1683, 1704, 1752, 1758, 1791, 1845, 1860, 1896, 1905, 1959, 1983, 1986, 2037, 2136, 2154, 2187, 2229, 2235, 2322, 2340, 2418, 2463, 2469, 2484, 2508, 2577, 2592, 2622, 2643,
    2646, 2673, 2688, 2754, 2805, 2811, 2814, 2841, 2844, 2850, 2910, 2973, 3027, 3081, 3195, 3387, 3408, 3456, 3462, 3495, 3549, 3564, 3600, 3609, 3663, 3687, 3690, 3741, 3840, 3858, 3891, 3933, 3939,
    4026, 4044, 4122, 4167, 4173, 4188, 4212, 4281, 4296, 4326, 4347, 4350, 4377, 4392, 4458, 4509, 4515, 4518, 4545, 4548, 4554, 4614, 4677, 4731, 4785, 4899, 5091, 5112, 5160, 5166, 5199, 5253, 5268,
    5304, 5313, 5367, 5391, 5394, 5445, 5544, 5562, 5595, 5637, 5643, 5730, 5748, 5826, 5871, 5877, 5892, 5916, 5985, 6000, 6030, 6051, 6054, 6081, 6096, 6162, 6213, 6219, 6222, 6249, 6252, 6258, 6318,
    6381, 6435, 6489, 6603, 6795, 6816};
static const int kTps2k[17] = {34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687};
static const int kTps8k[68] = {34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687, 1738, 1754, 1913, 2050, 2117, 2273, 2299, 2392, 2494, 2605, 2777, 2923, 2966, 2990, 3173, 3298, 3391, 3442, 3458, 3617, 3754, 3821, 3977, 4003, 4096, 4198, 4309, 4481, 4627, 4670, 4694, 4877, 5002, 5095, 5146, 5162, 5321, 5458, 5525, 5681, 5707, 5800, 5902, 6013, 6185, 6331, 6374, 6398, 6581, 6706, 6799};

const std::vector<int>& continualPilots(int mode) {
    static const std::vector<int> a(kCp2k, kCp2k + 45), b(kCp8k, kCp8k + 177);
    return mode == k8K ? b : a;
}
const std::vector<int>& tpsCarriers(int mode) {
    static const std::vector<int> a(kTps2k, kTps2k + 17), b(kTps8k, kTps8k + 68);
    return mode == k8K ? b : a;
}
const std::vector<uint8_t>& prbsW() {
    static std::vector<uint8_t> w;
    static std::once_flag once;
    std::call_once(once, [] {
        w.resize(6817);
        unsigned reg = 0x7FF;
        for (auto& v : w) { v = reg & 1; const unsigned nb = ((reg >> 2) ^ reg) & 1; reg = (reg >> 1) | (nb << 10); }
    });
    return w;
}

void carrierRoles(int mode, int symIdx, std::vector<uint8_t>& roles) {
    const int K = carriersK(mode);
    roles.assign(K, 0);
    for (int k = 3 * (symIdx % 4); k < K; k += 12) roles[k] = 1;
    for (int k : continualPilots(mode)) roles[k] = 2;
    for (int k : tpsCarriers(mode)) roles[k] = 3;
}

// ============================================================================ TPS (4.6)
static void putBits(std::array<uint8_t, 68>& s, int first, int last, unsigned v) { for (int i = last; i >= first; i--) { s[i] = v & 1; v >>= 1; } }

static void tpsBch(std::array<uint8_t, 68>& s) {
    unsigned reg = 0;
    uint8_t in[113];
    memset(in, 0, 60);
    memcpy(in + 60, &s[1], 53);
    for (int i = 0; i < 113; i++) {
        const unsigned fb = (in[i] ^ reg) & 1;
        reg >>= 1;
        reg |= fb << 13;
        reg ^= (fb << 12) ^ (fb << 11) ^ (fb << 9) ^ (fb << 8) ^ (fb << 7) ^ (fb << 5) ^ (fb << 4);
    }
    for (int i = 0; i < 14; i++) s[54 + i] = (reg >> i) & 1;
}

std::array<uint8_t, 68> tpsBits(const Params& p, int frameIdx) {
    std::array<uint8_t, 68> s{};
    s[0] = prbsW()[0];
    putBits(s, 1, 16, (frameIdx & 1) ? 0xCA11 : 0x35EE);
    putBits(s, 17, 22, p.cellIdLength ? 0x1F : 0x17);
    putBits(s, 23, 24, frameIdx & 3);
    putBits(s, 25, 26, p.mod);
    putBits(s, 27, 29, p.hier);
    putBits(s, 30, 32, p.crHp);
    putBits(s, 33, 35, p.crLp);
    putBits(s, 36, 37, p.guard);
    putBits(s, 38, 39, p.mode);
    putBits(s, 40, 47, (frameIdx & 1) ? (p.cellId & 0xFF) : ((p.cellId >> 8) & 0xFF));
    putBits(s, 48, 53, 0);
    tpsBch(s);
    return s;
}

bool tpsSync(const uint8_t* b, bool& odd) {
    unsigned v = 0;
    for (int i = 0; i < 16; i++) v = (v << 1) | (b[i] & 1);
    if (v == 0x35EE) { odd = false; return true; }
    if (v == 0xCA11) { odd = true; return true; }
    return false;
}

bool tpsDecode(const uint8_t bits[68], Params& p, int& frameIdx, bool& odd) {
    if (!tpsSync(bits + 1, odd)) return false;
    std::array<uint8_t, 68> s;
    for (int i = 0; i < 68; i++) s[i] = bits[i] & 1;
    std::array<uint8_t, 68> chk = s;
    tpsBch(chk);
    for (int i = 54; i < 68; i++) if (chk[i] != s[i]) return false;
    auto get = [&](int a, int b) { unsigned v = 0; for (int i = a; i <= b; i++) v = (v << 1) | s[i]; return (int)v; };
    const int len = get(17, 22);
    if (len != 0x17 && len != 0x1F) return false;
    p.cellIdLength = len == 0x1F;
    frameIdx = get(23, 24);
    if (odd != ((frameIdx & 1) != 0)) return false;
    p.mod = get(25, 26);
    p.hier = get(27, 29);
    p.crHp = get(30, 32);
    p.crLp = get(33, 35);
    p.guard = get(36, 37);
    p.mode = get(38, 39);
    if (p.mod > 2 || p.hier > 3 || p.crHp > 4 || p.crLp > 4 || p.mode > 1) return false;
    return true;
}

// ============================================================================ constellation (4.3.5)
void constellation(int mod, int hier, std::vector<cf32>& pts) {
    const int m = bitsPerCell(mod);
    const int size = 1 << m, alpha = alphaOf(hier);
    float norm;
    if (m == 2) norm = 1.f / std::sqrt(2.f);
    else if (m == 4) norm = alpha == 1 ? 1.f / std::sqrt(10.f) : alpha == 2 ? 1.f / std::sqrt(20.f) : 1.f / std::sqrt(52.f);
    else norm = alpha == 1 ? 1.f / std::sqrt(42.f) : alpha == 2 ? 1.f / std::sqrt(60.f) : 1.f / std::sqrt(108.f);
    const int bitsPerAxis = m / 2, stepsPerAxis = (int)std::lround(std::sqrt((double)size)) / 2 - 1, step = 2;
    pts.assign(size, cf32(0, 0));
    for (int i = 0; i < size; i++) {
        const int q = (i >> (2 * (bitsPerAxis - 1))) & 3;
        const int sign0 = (q >> 1) ? -1 : 1, sign1 = (q & 1) ? -1 : 1;
        int x = (i >> (bitsPerAxis - 1)) & ((1 << (bitsPerAxis - 1)) - 1);
        int y = i & ((1 << (bitsPerAxis - 1)) - 1);
        const int xval = alpha + (stepsPerAxis - x) * step, yval = alpha + (stepsPerAxis - y) * step;
        int val = ((x >> 1) ^ x) << (bitsPerAxis - 1) | ((y >> 1) ^ y);
        x = 0; y = 0;
        for (int j = 0; j < bitsPerAxis - 1; j++) { x += ((val >> (1 + 2 * j)) & 1) << j; y += ((val >> (2 * j)) & 1) << j; }
        val = (q << (2 * (bitsPerAxis - 1))) + (x << (bitsPerAxis - 1)) + y;
        pts[val] = norm * cf32((float)(sign0 * xval), (float)(sign1 * yval));
    }
}

// ============================================================================ transmit chain
void scramble(const uint8_t* ts, size_t packets, uint8_t* out) {
    unsigned reg = 0;
    auto clock8 = [&] {
        unsigned res = 0;
        for (int i = 0; i < 8; i++) {
            const unsigned fb = ((reg >> 13) ^ (reg >> 14)) & 1;
            reg = ((reg << 1) | fb) & 0x7FFF;
            res = (res << 1) | fb;
        }
        return res;
    };
    for (size_t p = 0; p < packets; p++) {
        if (p % 8 == 0) reg = 0xA9;
        const uint8_t* in = ts + p * 188;
        uint8_t* o = out + p * 188;
        o[0] = (p % 8 == 0) ? 0xB8 : 0x47;
        for (int k = 1; k < 188; k++) o[k] = in[k] ^ (uint8_t)clock8();
        clock8(); // the PRBS runs over the sync byte of the next packet
    }
}

namespace {
struct Gf {
    uint8_t exp[512], log[256];
    Gf() {
        unsigned x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};
const Gf& gf() { static Gf g; return g; }
// g(x) = prod_{i=0}^{15} (x - alpha^i): coefficients g[0..15] (x^16 implicit)
const std::array<uint8_t, 16>& rsGen() {
    static std::array<uint8_t, 16> g = [] {
        std::array<uint8_t, 17> p{};
        p[0] = 1;
        const Gf& f = gf();
        for (int i = 0; i < 16; i++) {          // multiply by (x + alpha^i)
            std::array<uint8_t, 17> n{};
            for (int j = 0; j <= i; j++) { n[j + 1] ^= p[j]; n[j] ^= f.mul(p[j], f.exp[i]); }
            p = n;
        }
        std::array<uint8_t, 16> r;
        for (int i = 0; i < 16; i++) r[i] = p[i];
        return r;
    }();
    return g;
}
} // namespace

void rsEncode(const uint8_t* in, uint8_t* out) {
    const Gf& f = gf();
    const auto& G = rsGen();
    uint8_t par[16] = {};
    for (int i = 0; i < 188; i++) {
        out[i] = in[i];
        const uint8_t fb = in[i] ^ par[0];
        for (int j = 0; j < 15; j++) par[j] = par[j + 1] ^ f.mul(fb, G[15 - j]);
        par[15] = f.mul(fb, G[0]);
    }
    memcpy(out + 188, par, 16);
}

ConvInterleaver::ConvInterleaver(bool inverse) : inverse_(inverse) {
    fifo_.resize(12);
    pos_.assign(12, 0);
    for (int j = 0; j < 12; j++) fifo_[j].assign((size_t)17 * (inverse ? 11 - j : j), 0);
}
void ConvInterleaver::process(const uint8_t* in, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const int j = (int)(i % 12);
        auto& f = fifo_[j];
        if (f.empty()) { out[i] = in[i]; continue; }
        out[i] = f[pos_[j]];
        f[pos_[j]] = in[i];
        if (++pos_[j] == f.size()) pos_[j] = 0;
    }
}

void InnerEncoder::encode(const std::vector<uint8_t>& bits, std::vector<uint8_t>& coded) {
    // pattern per input step: bit0 = X present, bit1 = Y present
    static const int kPat[5][7] = {{3, 0, 0, 0, 0, 0, 0}, {3, 2, 0, 0, 0, 0, 0}, {3, 2, 1, 0, 0, 0, 0}, {3, 2, 1, 2, 1, 0, 0}, {3, 2, 2, 2, 1, 2, 1}};
    static const int kLen[5] = {1, 2, 3, 5, 7};
    const int k = kLen[rate_];
    coded.clear();
    coded.reserve(bits.size() * 2);
    for (size_t i = 0; i < bits.size(); i++) {
        reg_ |= (unsigned)(bits[i] & 1) << 7;
        reg_ >>= 1;
        const unsigned x = __builtin_parity(reg_ & 0x79), y = __builtin_parity(reg_ & 0x5B);
        const int pat = kPat[rate_][step_ % k];
        step_++;
        if (pat & 1) coded.push_back((uint8_t)x);
        if (pat & 2) coded.push_back((uint8_t)y);
    }
}

namespace {
inline int hCol(int w, int e) { static const int off[6] = {0, 63, 105, 42, 21, 84}; return (w + off[e]) % 126; }
}

void bitInterleave(const std::vector<uint8_t>& coded, int mod, std::vector<uint8_t>& words) {
    const int v = bitsPerCell(mod);
    const size_t nw = coded.size() / v;
    std::vector<uint8_t> w0(nw);
    for (size_t i = 0; i < nw; i++) { unsigned c = 0; for (int j = 0; j < v; j++) c = (c << 1) | coded[i * v + j]; w0[i] = (uint8_t)c; }
    words.assign(nw, 0);
    for (size_t b = 0; b + 126 <= nw; b += 126) {
        uint8_t d[6][126];
        for (int i = 0; i < 126; i++) {
            const int c = w0[b + i];
            for (int k = 0; k < v; k++) d[(k / (v / 2)) + 2 * (k % (v / 2))][i] = (c >> (v - k - 1)) & 1;
        }
        for (int w = 0; w < 126; w++) {
            unsigned val = 0;
            for (int e = 0; e < v; e++) val = (val << 1) | d[e][hCol(w, e)];
            words[b + w] = (uint8_t)val;
        }
    }
}

const std::vector<int>& symbolPermutation(int mode) {
    static std::vector<int> h[2];
    static std::once_flag once[2];
    std::call_once(once[mode], [mode] {
        static const int perm2k[10] = {4, 3, 9, 6, 2, 8, 1, 5, 7, 0};
        static const int perm8k[12] = {7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5};
        const int Mmax = fftN(mode), Nmax = dataCarriers(mode);
        const int Nr = mode == k8K ? 13 : 11;
        const int* bp = mode == k8K ? perm8k : perm2k;
        std::vector<int>& H = h[mode];
        H.assign(Mmax, 0);
        unsigned reg = 1; // state for i >= 2 (state_2 = 1)
        const unsigned mask = (1u << Nr) - 1;
        int q = 0;
        for (int i = 0; i < Mmax; i++) {
            unsigned r = 0;
            if (i >= 2) {
                if (i > 2) {
                    const unsigned nb = mode == k8K ? ((reg ^ (reg >> 1) ^ (reg >> 4) ^ (reg >> 6)) & 1) : ((reg ^ (reg >> 3)) & 1);
                    reg = ((reg >> 1) | (nb << (Nr - 2))) & mask;
                }
                r = reg;
            }
            unsigned nr = 0;
            for (int k = 0; k < Nr - 1; k++) nr |= ((r >> k) & 1u) << bp[k];
            H[q] = (int)(((unsigned)(i % 2) << (Nr - 1)) + nr);
            if (H[q] < Nmax) q++;
        }
        H.resize(Nmax);
    });
    return h[mode];
}

void mapSymbol(const std::vector<uint8_t>& words, int mod, int hier, std::vector<cf32>& cells) {
    std::vector<cf32> pts;
    constellation(mod, hier, pts);
    cells.resize(words.size());
    for (size_t i = 0; i < words.size(); i++) cells[i] = pts[words[i]];
}

// ============================================================================ receive chain
namespace {
struct AxisTables {
    static constexpr int G = 1024;
    float R = 0, scale = 0;
    int nbits = 0;
    // for label bit j: which axis (0 I, 1 Q) and the grid of f(y) = min(bit=1) - min(bit=0) over axis levels
    int axis[6];
    std::vector<float> lut[6];
};
const AxisTables& tables(int mod, int hier) {
    static AxisTables t[3][4];
    static std::once_flag once[3][4];
    std::call_once(once[mod][hier], [mod, hier] {
        AxisTables& T = t[mod][hier];
        std::vector<cf32> pts;
        constellation(mod, hier, pts);
        const int m = bitsPerCell(mod);
        T.nbits = m;
        float mx = 0;
        for (auto& p : pts) mx = std::max(mx, std::max(std::fabs(p.real()), std::fabs(p.imag())));
        T.R = mx + 1.2f;
        T.scale = (float)AxisTables::G / (2 * T.R);
        for (int j = 0; j < m; j++) {
            const int sh = m - 1 - j;
            bool movesRe = false;
            for (int i = 0; i < (1 << m); i++) if (std::fabs(pts[i].real() - pts[i ^ (1 << sh)].real()) > 1e-6f) movesRe = true;
            T.axis[j] = movesRe ? 0 : 1;
            T.lut[j].resize(AxisTables::G + 2);
            for (int g = 0; g <= AxisTables::G + 1; g++) {
                const float y = -T.R + (float)g / T.scale;
                float m0 = 1e30f, m1 = 1e30f;
                for (int i = 0; i < (1 << m); i++) {
                    const float c = movesRe ? pts[i].real() : pts[i].imag();
                    const float e = (y - c) * (y - c);
                    if ((i >> sh) & 1) m1 = std::min(m1, e); else m0 = std::min(m0, e);
                }
                T.lut[j][g] = m1 - m0;
            }
        }
    });
    return t[mod][hier];
}
} // namespace

void demap(const cf32* cells, const float* n0, int count, int mod, int hier, float* llr) {
    const AxisTables& T = tables(mod, hier);
    const int m = T.nbits;
    for (int c = 0; c < count; c++) {
        const float w = 1.f / std::max(1e-9f, 2 * n0[c]);
        const float y[2] = {cells[c].real(), cells[c].imag()};
        for (int j = 0; j < m; j++) {
            float t = (y[T.axis[j]] + T.R) * T.scale;
            t = std::min(std::max(t, 0.f), (float)AxisTables::G - 0.001f);
            const int i = (int)t;
            const float fr = t - (float)i;
            llr[(size_t)c * m + j] = w * (T.lut[j][i] + fr * (T.lut[j][i + 1] - T.lut[j][i]));
        }
    }
}

void symbolDeinterleave(int mode, int symIdx, const cf32* in, const float* n0in, cf32* out, float* n0out) {
    const auto& H = symbolPermutation(mode);
    const int N = dataCarriers(mode);
    if (symIdx % 2) { for (int q = 0; q < N; q++) { out[H[q]] = in[q]; n0out[H[q]] = n0in[q]; } }
    else { for (int q = 0; q < N; q++) { out[q] = in[H[q]]; n0out[q] = n0in[H[q]]; } }
}

void bitDeinterleave(const float* in, int mod, int words, float* out) {
    const int v = bitsPerCell(mod);
    for (int b = 0; b + 126 <= words; b += 126) {
        float d[6][126];
        for (int w = 0; w < 126; w++) for (int e = 0; e < v; e++) d[e][hCol(w, e)] = in[(size_t)(b + w) * v + e];
        for (int i = 0; i < 126; i++) for (int k = 0; k < v; k++) out[(size_t)(b + i) * v + k] = d[(k / (v / 2)) + 2 * (k % (v / 2))][i];
    }
}

void Viterbi::depuncture(const float* llr, size_t n, int rate, int phase, std::vector<int8_t>& soft, float scale) {
    static const int kPat[5][7] = {{3, 0, 0, 0, 0, 0, 0}, {3, 2, 0, 0, 0, 0, 0}, {3, 2, 1, 0, 0, 0, 0}, {3, 2, 1, 2, 1, 0, 0}, {3, 2, 2, 2, 1, 2, 1}};
    static const int kLen[5] = {1, 2, 3, 5, 7};
    const int k = kLen[rate];
    soft.clear();
    soft.reserve(n * 2);
    size_t i = 0;
    int step = phase % k;
    auto q = [&](float v) { const float s = v * scale; return (int8_t)std::max(-127.f, std::min(127.f, std::round(s))); };
    while (i < n) {
        const int pat = kPat[rate][step];
        int8_t x = 0, y = 0;
        if (pat & 1) { if (i >= n) break; x = q(llr[i++]); }
        if (pat & 2) { if (i >= n) break; y = q(llr[i++]); }
        soft.push_back(x);
        soft.push_back(y);
        if (++step == k) step = 0;
    }
}

namespace {
struct Trellis {
    // for each new state ns and predecessor bit b: expected code bits (x,y) as +1 / -1 signs
    int8_t sx[64][2], sy[64][2];
    Trellis() {
        for (int ns = 0; ns < 64; ns++) {
            const int u = ns >> 5;
            for (int b = 0; b < 2; b++) {
                const int s = ((ns & 31) << 1) | b;           // predecessor state
                const unsigned reg = ((unsigned)u << 6) | (unsigned)s;
                sx[ns][b] = __builtin_parity(reg & 0x79) ? -1 : 1;
                sy[ns][b] = __builtin_parity(reg & 0x5B) ? -1 : 1;
            }
        }
    }
};
const Trellis& trellis() { static Trellis t; return t; }

#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#include <array>
#include <cstdint>
#include <vector>
// NEON path. States 2j and 2j+1 feed new states j and j+32; both branch outputs flip when either the new bit or the oldest
// state bit flips (both generator polynomials have those taps), so one metric M_j = +/-sx +/- sy per butterfly is enough:
//   new[j]    = max(old[2j] + M, old[2j+1] - M)      new[j+32] = max(old[2j] - M, old[2j+1] + M)
struct NeonTables {
    int16_t sigX[32], sigY[32];
    NeonTables() {
        for (int j = 0; j < 32; j++) {
            const unsigned reg = (unsigned)(2 * j);
            sigX[j] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sigY[j] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    }
};
const NeonTables& neonTables() { static NeonTables t; return t; }

inline uint8_t maskBits(uint16x8_t m) {
    static const uint16_t w[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    return (uint8_t)vaddvq_u16(vandq_u16(m, vld1q_u16(w)));
}

void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const NeonTables& T = neonTables();
    std::vector<uint64_t> dec(steps);
    long off = 0; // total subtracted by the renormalisation: the absolute path metric is off + pm
    alignas(16) int16_t pm[64] = {}, nm[64];
    int16x8_t sigX[4], sigY[4];
    for (int q = 0; q < 4; q++) { sigX[q] = vld1q_s16(T.sigX + 8 * q); sigY[q] = vld1q_s16(T.sigY + 8 * q); }
    for (size_t t = 0; t < steps; t++) {
        const int16_t a = soft[2 * t], b = soft[2 * t + 1];
        int16x8x2_t o[4];
        for (int q = 0; q < 4; q++) o[q] = vld2q_s16(pm + 16 * q);
        uint8_t lo[4], hi[4];
        for (int q = 0; q < 4; q++) {
            const int16x8_t M = vaddq_s16(vmulq_n_s16(sigX[q], a), vmulq_n_s16(sigY[q], b));
            const int16x8_t A = o[q].val[0], B = o[q].val[1];
            const int16x8_t p0 = vqaddq_s16(A, M), p1 = vqsubq_s16(B, M);
            const int16x8_t r0 = vqsubq_s16(A, M), r1 = vqaddq_s16(B, M);
            vst1q_s16(nm + 8 * q, vmaxq_s16(p0, p1));
            vst1q_s16(nm + 32 + 8 * q, vmaxq_s16(r0, r1));
            lo[q] = maskBits(vcgtq_s16(p1, p0));
            hi[q] = maskBits(vcgtq_s16(r1, r0));
        }
        uint64_t d = 0;
        for (int q = 0; q < 4; q++) { d |= (uint64_t)lo[q] << (8 * q); d |= (uint64_t)hi[q] << (32 + 8 * q); }
        dec[t] = d;
        // renormalise every 4 steps so int16 never saturates
        if ((t & 3) == 3) {
            int16x8_t mx = vld1q_s16(nm);
            for (int q = 1; q < 8; q++) mx = vmaxq_s16(mx, vld1q_s16(nm + 8 * q));
            off += vmaxvq_s16(mx);
            const int16x8_t m = vdupq_n_s16(vmaxvq_s16(mx));
            for (int q = 0; q < 8; q++) vst1q_s16(pm + 8 * q, vsubq_s16(vld1q_s16(nm + 8 * q), m));
        } else memcpy(pm, nm, sizeof pm);
    }
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#else
void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const Trellis& T = trellis();
    std::vector<uint64_t> dec(steps);
    long off = 0;
    int32_t pm[64] = {}, nm[64];
    for (size_t t = 0; t < steps; t++) {
        const int sx = soft[2 * t], sy = soft[2 * t + 1];
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int p0 = ((ns & 31) << 1), p1 = p0 | 1;
            const int m0 = pm[p0] + T.sx[ns][0] * sx + T.sy[ns][0] * sy;
            const int m1 = pm[p1] + T.sx[ns][1] * sx + T.sy[ns][1] * sy;
            if (m1 > m0) { nm[ns] = m1; d |= 1ull << ns; } else nm[ns] = m0;
        }
        int best = nm[0];
        for (int s = 1; s < 64; s++) best = std::max(best, nm[s]);
        off += best;
        for (int s = 0; s < 64; s++) pm[s] = nm[s] - best;
        dec[t] = d;
    }
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#endif
} // namespace

void Viterbi::decode(const std::vector<int8_t>& soft, std::vector<uint8_t>& out, int threads, long* metric) {
    const size_t steps = soft.size() / 2;
    out.assign(steps, 0);
    const size_t C = 16384, L = 192;
    const size_t chunks = (steps + C - 1) / C;
    std::vector<long> metrics(chunks, 0);
    auto work = [&](size_t ci) {
        const size_t a = ci * C, b = std::min(steps, a + C);
        const size_t lo = a > L ? a - L : 0, hi = std::min(steps, b + L);
        std::vector<uint8_t> bits;
        long mt = 0;
        viterbiRange(soft.data() + 2 * lo, hi - lo, bits, &mt);
        metrics[ci] = mt;
        for (size_t t = a; t < b; t++) out[t] = bits[t - lo];
    };
    if (threads <= 1 || chunks <= 1) { for (size_t c = 0; c < chunks; c++) work(c); }
    else {
        std::atomic<size_t> next{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < std::min<size_t>(threads, chunks); t++) ts.emplace_back([&] { for (size_t c; (c = next++) < chunks;) work(c); });
        for (auto& t : ts) t.join();
    }
    if (metric) { long s = 0; for (long m : metrics) s += m; *metric = s; }
}

int rsDecode(uint8_t* r) {
    const Gf& f = gf();
    // syndromes S_i = r(alpha^i), r as polynomial with byte 0 the highest degree (203)
    uint8_t S[16];
    bool any = false;
    for (int i = 0; i < 16; i++) {
        uint8_t s = 0;
        for (int j = 0; j < 204; j++) s = f.mul(s, f.exp[i]) ^ r[j];
        S[i] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    uint8_t C[17] = {1}, B[17] = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < 16; n++) {
        uint8_t d = S[n];
        for (int i = 1; i <= L; i++) d ^= f.mul(C[i], S[n - i]);
        if (d == 0) { m++; continue; }
        uint8_t T[17];
        memcpy(T, C, 17);
        const uint8_t coef = f.mul(d, f.inv(b));
        for (int i = 0; i + m < 17; i++) C[i + m] ^= f.mul(coef, B[i]);
        if (2 * L <= n) { L = n + 1 - L; memcpy(B, T, 17); b = d; m = 1; } else m++;
    }
    if (L > 8) return -1;
    // Chien search: error at degree p (position j = 203 - p) when C(alpha^{-p}) = 0
    int pos[8], cnt = 0;
    for (int p = 0; p < 204; p++) {
        const uint8_t x = f.exp[(255 - p) % 255];       // alpha^{-p}
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = f.mul(v, x) ^ C[i];
        if (v == 0) { if (cnt >= 8) return -1; pos[cnt++] = p; }
    }
    if (cnt != L) return -1;
    // error evaluator Omega(x) = S(x) C(x) mod x^16, Forney with fcr = 0: e = X * Omega(X^-1) / C'(X^-1)
    uint8_t om[16] = {};
    for (int i = 0; i < 16; i++) { uint8_t s = 0; for (int j = 0; j <= std::min(i, L); j++) s ^= f.mul(C[j], S[i - j]); om[i] = s; }
    for (int k = 0; k < cnt; k++) {
        const int p = pos[k];
        const uint8_t X = f.exp[p % 255], xi = f.exp[(255 - p) % 255];
        uint8_t num = 0;
        for (int i = 15; i >= 0; i--) num = f.mul(num, xi) ^ om[i];
        uint8_t den = 0;                                  // C'(x) = sum over odd i of C[i] x^{i-1}
        for (int i = 1; i <= L; i += 2) den ^= f.mul(C[i], f.exp[((255 - p) * (i - 1)) % 255]);
        if (den == 0) return -1;
        const uint8_t e = f.mul(f.mul(num, X), f.inv(den));
        r[203 - p] ^= e;
    }
    // verify
    for (int i = 0; i < 16; i++) { uint8_t s = 0; for (int j = 0; j < 204; j++) s = f.mul(s, f.exp[i]) ^ r[j]; if (s) return -1; }
    return cnt;
}

void descramble(uint8_t* packets, size_t count, int first) {
    unsigned reg = 0;
    auto clock8 = [&] {
        unsigned res = 0;
        for (int i = 0; i < 8; i++) {
            const unsigned fb = ((reg >> 13) ^ (reg >> 14)) & 1;
            reg = ((reg << 1) | fb) & 0x7FFF;
            res = (res << 1) | fb;
        }
        return res;
    };
    for (size_t p = 0; p < count; p++) {
        const int g = (int)((first + p) % 8);
        if (g == 0) reg = 0xA9;
        uint8_t* b = packets + p * 188;
        b[0] = 0x47;
        for (int k = 1; k < 188; k++) b[k] ^= (uint8_t)clock8();
        clock8();
    }
}

} // namespace dvbt
} // namespace dect2
