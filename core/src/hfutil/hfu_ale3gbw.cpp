// 3G-ALE traffic bursts BW1 to BW4: receivers, TM / HDL / LDL PDU readers and test generators; see hfu_ale3gbw.h.
// Clean-room from the text of MIL-STD-188-141B Appendix C (C.4.12, C.5.1.2, C.5.1.4 to C.5.1.8, C.5.3.4, C.5.4.4, C.5.5.4).
#include "dect2/hfutil/hfu_ale3gbw.h"
#include "dect2/hfutil/hfu_ale3g.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <map>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cf = std::complex<float>;
constexpr double kFs = 24000.0, kBaud = 2400.0, kCarrier = 1800.0;
constexpr int kSps = 10;

// table C-XI (BW1 acquisition preamble), table C-XV (BW3 preamble), table C-XIX (BW4 PN sequence)
constexpr uint8_t kPre1[576] = {
    4,4,7,3,7,7,1,0,4,7,1,6,6,5,5,0,6,4,0,6,0,1,6,2,2,7,3,5,1,5,4,5,5,6,6,0,2,0,4,0,7,0,2,6,3,7,1,5,2,3,2,3,7,1,1,7,0,0,0,7,4,5,2,3,
    2,3,7,3,1,0,3,4,2,5,6,6,6,5,2,3,2,7,6,7,6,6,1,0,1,2,6,5,6,5,1,4,3,5,2,6,5,6,5,2,5,2,0,0,2,6,7,0,4,2,2,5,0,2,5,1,5,2,1,2,3,4,1,7,
    4,1,0,5,1,1,4,1,6,2,6,5,2,2,3,1,7,1,0,6,0,4,6,2,3,3,6,1,5,0,4,2,5,1,4,6,7,2,1,5,4,1,4,7,7,1,5,4,1,7,0,2,6,2,4,7,1,1,3,0,6,4,2,1,
    7,3,2,5,4,0,4,4,5,4,6,7,7,2,6,7,1,1,4,6,0,5,1,6,6,1,2,3,2,5,3,4,5,2,0,4,1,4,6,5,2,6,3,2,2,3,0,7,7,0,2,2,1,6,6,6,0,5,1,3,4,5,1,6,
    7,2,2,2,1,3,7,5,7,0,6,6,5,7,2,4,0,3,0,6,1,4,3,4,0,1,5,4,5,1,5,7,6,5,6,4,7,7,0,1,4,3,5,6,1,5,7,1,5,3,1,0,5,5,0,4,2,2,2,5,2,4,5,3,
    6,2,6,3,5,0,4,0,0,7,3,5,1,4,5,5,2,5,2,6,6,3,7,6,0,2,7,1,4,3,5,2,6,1,2,0,6,5,1,7,1,0,6,3,0,4,7,6,0,5,0,4,1,5,7,0,4,6,6,1,7,0,5,1,
    6,0,6,4,6,6,1,4,6,3,3,2,1,4,4,1,4,6,7,2,6,2,4,6,1,0,5,0,4,0,5,4,4,2,5,2,7,2,4,4,7,3,6,4,7,5,6,5,6,5,5,3,2,3,4,7,5,7,2,7,1,5,5,3,
    0,3,5,4,2,1,3,7,1,5,4,4,3,7,5,5,5,4,7,0,7,7,1,0,5,4,7,0,4,6,7,1,0,0,3,4,5,4,7,0,3,3,2,2,2,0,3,2,7,0,0,3,0,5,3,7,1,4,2,3,5,3,5,7,
    1,3,3,1,0,1,1,6,5,1,5,1,5,0,7,0,2,5,7,6,7,7,3,1,0,3,1,4,2,3,5,1,4,0,2,1,7,1,1,7,4,5,0,1,0,0,3,6,6,6,6,3,7,3,2,6,0,3,7,5,1,0,1,6};
constexpr uint8_t kPre3[640] = {
    7,2,3,5,7,5,0,6,7,5,3,5,6,3,5,4,7,1,4,5,7,7,3,5,0,5,1,6,0,3,1,0,2,7,6,2,4,4,2,0,0,7,2,0,2,3,7,4,1,1,3,0,1,3,6,3,0,1,3,1,5,4,5,6,
    3,2,5,2,6,0,6,6,0,4,6,3,7,1,7,0,6,2,1,5,5,2,2,5,3,3,3,2,1,4,7,0,0,2,0,2,5,7,5,7,7,5,3,6,2,2,1,6,4,4,7,1,5,4,7,2,7,5,6,1,1,5,0,0,
    1,4,0,5,3,4,7,3,6,2,2,5,4,0,2,7,6,2,7,1,6,5,5,3,2,3,2,5,7,7,3,7,3,2,2,2,4,0,0,7,5,4,5,3,5,0,3,3,3,0,6,4,6,5,6,4,2,7,6,2,6,6,1,0,
    5,1,0,7,1,4,2,7,6,0,1,6,7,5,6,1,1,7,5,1,0,0,1,0,3,1,7,4,5,4,4,5,4,3,2,0,4,1,6,6,2,7,6,4,4,6,2,2,3,0,3,5,2,1,1,6,2,7,6,2,2,5,7,1,
    2,5,5,6,4,0,7,1,7,2,3,2,5,2,0,2,2,2,0,3,6,6,6,2,5,5,5,6,0,0,2,3,6,7,6,5,7,2,2,4,5,5,2,5,7,3,2,7,0,3,0,1,4,1,6,2,5,7,0,1,6,0,1,6,
    5,1,3,6,5,4,2,0,4,4,2,1,2,6,1,1,0,1,1,3,5,7,5,0,4,3,1,5,3,0,0,4,5,6,7,5,7,6,1,5,5,1,2,7,5,0,3,6,3,5,2,7,0,6,6,0,6,5,4,2,7,5,6,0,
    4,1,7,0,4,7,4,7,3,1,2,3,7,2,2,6,7,5,1,6,6,7,2,5,6,4,0,3,0,4,7,1,6,2,5,4,3,6,0,6,6,5,3,3,4,4,5,1,2,6,7,3,1,3,0,7,7,4,6,2,5,2,0,7,
    3,6,7,6,3,6,3,1,4,4,6,3,7,7,6,4,4,5,2,2,5,4,7,4,5,6,2,6,0,2,4,6,3,3,4,3,5,5,0,7,6,3,1,6,0,2,2,0,4,2,6,7,7,2,0,5,1,3,7,6,3,7,2,0,
    1,6,3,5,1,0,3,7,5,4,6,7,2,4,0,0,2,2,7,1,2,6,3,3,7,1,7,7,4,1,2,2,5,4,0,3,3,5,6,1,0,4,5,6,7,1,2,0,3,1,6,2,4,6,1,5,6,7,7,2,6,3,7,6,
    7,2,3,4,4,4,6,0,4,3,7,7,1,5,7,1,3,4,5,6,6,3,2,3,4,4,0,1,4,0,3,6,7,3,5,0,3,0,7,1,0,5,4,5,4,4,3,7,6,1,1,5,0,1,1,1,4,6,0,7,2,5,4,3};
constexpr uint8_t kPn4[1280] = {
    5,2,0,7,1,3,2,5,0,4,5,3,3,0,7,3,5,3,2,3,7,4,7,5,0,4,1,0,5,7,6,5,2,6,4,1,7,1,6,6,0,1,2,2,1,6,7,6,3,7,1,3,5,0,6,4,6,6,5,2,2,3,5,2,
    3,5,7,2,5,4,6,0,6,1,0,7,5,2,0,2,6,6,2,4,6,0,2,0,7,3,0,1,5,0,2,1,6,2,2,7,4,3,1,1,4,6,3,5,1,3,6,1,6,3,3,0,5,5,7,1,7,4,7,6,1,7,1,7,
    1,5,3,7,3,7,5,3,3,6,6,3,3,4,6,6,4,1,0,5,5,3,3,1,2,6,2,1,1,5,3,2,4,7,7,1,5,5,7,3,7,2,0,4,0,7,0,5,6,4,1,4,3,0,7,6,3,0,0,5,1,3,4,4,
    2,1,5,1,3,0,3,0,2,3,6,6,7,6,0,0,2,6,2,7,7,1,0,3,5,1,0,0,0,1,2,0,2,1,7,0,3,6,3,5,7,5,2,6,5,2,3,4,3,1,5,5,7,1,7,2,1,6,4,0,6,2,4,7,
    4,2,0,7,4,4,5,3,5,4,2,4,0,4,5,2,2,6,6,4,3,6,2,0,3,2,7,0,4,2,6,0,7,1,4,7,2,6,0,3,5,5,4,2,1,1,5,7,0,2,3,2,2,0,5,2,2,0,7,1,4,3,0,4,
    0,1,7,0,5,5,1,2,3,1,5,1,6,6,6,1,0,5,6,1,2,6,2,0,4,4,4,4,4,4,3,7,6,4,5,3,2,5,5,1,6,4,0,4,2,3,5,2,2,1,0,7,4,5,1,2,5,2,6,5,6,0,2,0,
    2,7,1,7,2,2,1,6,6,5,3,6,1,4,4,4,4,3,3,6,3,5,4,1,5,0,4,3,7,4,2,7,5,3,4,6,0,4,5,1,3,1,3,0,1,0,2,2,3,2,3,6,3,1,5,3,3,6,0,3,7,1,1,4,
    3,6,3,1,2,5,2,3,6,3,0,2,6,2,3,4,6,5,7,5,5,0,4,0,2,7,0,4,2,7,2,2,5,5,4,7,7,4,4,3,6,7,3,4,1,6,1,1,5,3,1,7,6,4,5,4,7,2,6,4,1,1,3,0,
    5,0,5,7,7,4,2,5,1,5,3,5,3,3,7,5,7,3,0,0,5,5,7,0,5,3,0,2,3,3,6,6,3,2,2,5,4,5,7,5,6,3,7,1,1,5,2,0,0,0,5,3,2,7,2,4,1,7,1,5,4,7,2,3,
    6,6,1,0,5,5,2,4,4,6,5,2,4,4,6,5,5,2,1,6,1,7,3,1,4,6,5,2,6,5,7,0,7,2,4,6,1,1,2,4,4,2,7,4,4,5,3,5,0,7,1,5,5,5,5,6,2,5,3,3,7,0,5,7,
    0,3,1,0,6,3,1,0,7,2,5,3,6,6,5,2,6,4,2,0,5,5,3,3,7,3,4,3,0,3,3,5,3,6,7,1,0,2,0,3,5,3,5,2,7,6,1,6,1,5,6,2,1,6,2,4,2,4,1,7,4,2,2,2,
    7,0,7,7,5,6,2,0,5,3,5,1,4,0,7,1,2,6,2,7,5,5,5,4,1,4,0,7,2,4,6,6,7,5,5,4,3,0,6,2,3,1,3,1,2,0,3,7,6,6,2,1,2,4,3,1,1,4,6,2,0,4,2,2,
    3,2,2,3,1,3,6,0,2,7,3,4,7,5,3,5,2,2,3,0,3,3,6,7,1,3,0,0,0,6,0,3,2,2,6,1,3,6,0,2,7,5,5,6,3,3,7,0,2,3,3,5,2,2,7,2,3,1,3,2,0,4,4,0,
    1,3,6,0,5,0,5,2,0,0,1,0,2,3,6,5,5,6,6,3,1,7,7,6,0,5,2,5,2,7,7,2,3,3,6,3,7,6,0,7,1,1,1,3,7,1,1,2,5,0,7,3,4,1,3,4,7,0,5,5,3,2,6,1,
    5,6,4,1,6,4,2,7,4,3,5,2,6,4,1,1,3,4,0,1,2,7,0,1,4,4,6,6,1,7,7,0,2,5,0,1,6,4,3,6,3,0,5,0,3,3,2,3,2,4,7,7,0,4,7,3,2,6,7,5,5,3,1,0,
    1,5,3,6,4,4,6,5,4,5,0,1,2,1,3,7,3,4,5,5,4,2,5,0,5,4,7,4,7,5,0,0,1,7,2,1,7,0,5,0,2,4,2,5,3,1,6,7,7,6,4,3,6,6,4,2,7,2,2,5,0,1,4,0,
    3,7,5,1,5,1,7,3,5,2,2,0,6,3,2,1,3,6,4,5,1,5,7,6,7,0,6,6,0,1,7,3,7,5,1,0,0,7,0,7,7,0,7,6,1,5,2,1,5,5,1,2,6,2,5,6,2,0,1,2,1,1,4,0,
    3,6,5,4,6,0,0,3,3,7,3,3,2,5,6,2,6,4,2,1,7,2,6,0,0,5,0,0,5,1,0,0,6,6,5,7,5,4,1,0,4,1,0,0,0,1,5,1,2,6,1,1,6,4,3,7,6,0,2,5,0,4,1,0,
    4,6,1,5,3,2,2,7,1,2,7,0,2,7,6,1,2,7,2,4,4,6,5,5,4,7,1,5,3,6,2,4,4,0,0,2,3,2,4,1,2,7,0,2,1,7,5,0,4,4,2,7,5,6,5,2,3,0,1,0,0,2,3,3,
    5,4,4,5,1,6,4,0,2,0,2,1,4,4,7,4,2,0,5,1,6,2,5,6,6,3,1,7,6,1,3,4,0,2,2,4,4,5,0,4,2,1,3,6,6,2,4,1,7,4,2,6,7,5,0,0,5,6,7,6,1,4,5,5};
// table C-XVII (BW3 PN sequence)
constexpr uint8_t kPn3[32] = {0,0,0,0,0,2,4,6,0,4,0,4,0,6,4,2,0,0,0,0,1,3,5,7,2,6,2,6,3,1,7,5};
// table C-XIV (BW2 gray coding)
constexpr uint8_t kGray[8] = {0, 1, 3, 2, 7, 6, 4, 5};
constexpr int kNTlc = 256, kNPre1 = 576, kNData1 = 2304, kNTlc2 = 240, kNPre2 = 64, kNPre3 = 640, kNData4 = 1280;
const int kBw3N[4] = {64, 128, 256, 512};

cf psk8(int n) { const double a = (n & 7) * kPi / 4; return cf((float)std::cos(a), (float)std::sin(a)); }
inline int par(uint32_t v) { return __builtin_parity(v); }
int walshChip(int n, int j) { return par((uint32_t)(n & j)) ? 4 : 0; }   // table C-VIII (16-ary) and table C-XVIII (4-ary, j < 4)

// C.5.1.2: for each fetch position, the stuffed bit index
std::vector<int> ilvOrder(int R, int C, int irs, int ics, int dirs, int dics, int irf, int icf, int dirf, int dicf) {
    auto md = [](int a, int m) { return ((a % m) + m) % m; };
    std::vector<int> m((size_t)(R * C), -1), out;
    int rs = 0, cs = 0;
    for (int s = 0; s < R * C;) {
        m[(size_t)(rs * C + cs)] = s;
        s++;
        cs = md(s % R == 0 ? cs + ics + dics : cs + ics, C);
        rs = md(s % C == 0 ? rs + irs + dirs : rs + irs, R);
    }
    int rf = 0, cfp = 0;
    for (int f = 0; f < R * C;) {
        out.push_back(m[(size_t)(rf * C + cfp)]);
        f++;
        cfp = md(f % R == 0 ? cfp + icf + dicf : cfp + icf, C);
        rf = md(f % C == 0 ? rf + irf + dirf : rf + irf, R);
    }
    return out;
}
const std::vector<int>& bw1Order() { static const std::vector<int> o = ilvOrder(16, 9, 0, 1, 1, 0, 1, 0, 0, 1); return o; }   // table C-XII
int bw3Idx(int n) { for (int i = 0; i < 4; i++) if (kBw3N[i] == n) return i; return -1; }
const std::vector<int>& bw3Order(int n) {   // table C-XVI
    static const std::vector<int> o[4] = {ilvOrder(24, 24, 7, 0, 0, 1, 1, -7, 0, -7), ilvOrder(32, 34, 7, 0, 0, 1, 1, -7, 0, -7),
                                          ilvOrder(44, 48, 7, 0, 0, 1, 1, -7, 0, -7), ilvOrder(64, 65, 7, 0, 0, 1, 1, -7, 0, -7)};
    return o[std::max(0, bw3Idx(n))];
}
// the fetch of table C-XVI reads every cell once for n = 64 and 128; for 256 and 512 the printed parameters read some cells twice
bool bw3Decodable(int n) {
    const auto& o = bw3Order(n);
    std::vector<uint8_t> seen(o.size(), 0);
    for (int v : o) { if (v < 0 || seen[(size_t)v]) return false; seen[(size_t)v] = 1; }
    return true;
}

// convolutional codes: the register holds x^0 (the oldest) .. x^(K-1) (the newest input) in bits 0 .. K-1
struct Code { int K; int n; uint32_t g[4]; };
const Code kBw1Code{9, 3, {0x1C9u, 0x1B3u, 0x16Fu, 0}};   // figure C-8
const Code kBw2Code{8, 4, {0x9Du, 0xBDu, 0xCBu, 0xEFu}};  // figure C-11
const Code kBw3Code{7, 2, {0x5Bu, 0x79u, 0, 0}};          // figure C-14

std::vector<uint8_t> convEncode(const Code& c, const std::vector<uint8_t>& in, bool tailBiting) {
    uint32_t r = 0;
    if (tailBiting && in.size() >= (size_t)(c.K - 1)) for (size_t i = in.size() - (size_t)(c.K - 1); i < in.size(); i++) r = (r >> 1) | ((uint32_t)in[i] << (c.K - 1));
    std::vector<uint8_t> out;
    out.reserve(in.size() * (size_t)c.n);
    for (uint8_t u : in) {
        r = (r >> 1) | ((uint32_t)(u & 1) << (c.K - 1));
        for (int j = 0; j < c.n; j++) out.push_back((uint8_t)par(r & c.g[j]));
    }
    return out;
}

// soft: n values per step, positive favours 0, 0 erased. Terminated: from and to state 0. Tail-biting: three rounds, the middle kept.
std::vector<uint8_t> viterbi(const Code& c, const std::vector<float>& soft, bool tailBiting) {
    const int K = c.K, n = c.n, S = 1 << (K - 1);
    const size_t N = soft.size() / (size_t)n;
    if (N == 0) return {};
    const size_t steps = tailBiting ? 3 * N : N;
    std::vector<uint8_t> ob((size_t)(2 * S));
    for (int ps = 0; ps < S; ps++) for (int u = 0; u < 2; u++) {
        const uint32_t r = (uint32_t)ps | ((uint32_t)u << (K - 1));
        int m = 0;
        for (int j = 0; j < n; j++) m |= par(r & c.g[j]) << j;
        ob[(size_t)((ps << 1) | u)] = (uint8_t)m;
    }
    std::vector<float> m((size_t)S, tailBiting ? 0.f : -1e30f), nm((size_t)S);
    if (!tailBiting) m[0] = 0;
    std::vector<uint8_t> dec(steps * (size_t)S);
    float bmTab[16];
    for (size_t t = 0; t < steps; t++) {
        const float* s = &soft[(t % N) * (size_t)n];
        for (int o = 0; o < (1 << n); o++) { float v = 0; for (int j = 0; j < n; j++) v += ((o >> j) & 1) ? -s[j] : s[j]; bmTab[o] = v; }
        float best = -1e30f;
        for (int ns = 0; ns < S; ns++) {
            const int u = ns >> (K - 2);
            const int p0 = (ns << 1) & (S - 1), p1 = p0 | 1;
            const float a = m[(size_t)p0] + bmTab[ob[(size_t)((p0 << 1) | u)]], b = m[(size_t)p1] + bmTab[ob[(size_t)((p1 << 1) | u)]];
            if (b > a) { nm[(size_t)ns] = b; dec[t * (size_t)S + (size_t)ns] = 1; } else { nm[(size_t)ns] = a; dec[t * (size_t)S + (size_t)ns] = 0; }
            best = std::max(best, nm[(size_t)ns]);
        }
        for (int i = 0; i < S; i++) m[(size_t)i] = nm[(size_t)i] - best;
    }
    int ns = 0;
    if (tailBiting) for (int i = 1; i < S; i++) if (m[(size_t)i] > m[(size_t)ns]) ns = i;
    std::vector<uint8_t> bits(steps);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(ns >> (K - 2));
        ns = ((ns << 1) & (S - 1)) | dec[t * (size_t)S + (size_t)ns];
    }
    if (tailBiting) return std::vector<uint8_t>(bits.begin() + (long)N, bits.begin() + (long)(2 * N));
    return bits;
}

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

// C.5.1.8: 8-PSK on 1800 Hz at 2400 Bd
std::vector<cf32> modulate(const std::vector<uint8_t>& sy, double leadSec, double tailSec) {
    const std::vector<float> h = rrc();
    const size_t lead = (size_t)(leadSec * kFs), tail = (size_t)(tailSec * kFs);
    std::vector<cf32> out(lead + sy.size() * kSps + h.size() + tail, cf32(0, 0));
    for (size_t s = 0; s < sy.size(); s++) {
        const cf v = psk8(sy[s]);
        for (size_t k = 0; k < h.size(); k++) out[lead + s * kSps + k] += v * h[k];
    }
    double ph = 0;
    const double w = 2 * kPi * kCarrier / kFs;
    for (auto& v : out) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)) * 3.16f; ph += w; if (ph > kPi) ph -= 2 * kPi; }
    return out;
}

void putBits(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1u)); }
uint64_t getBits(const std::vector<uint8_t>& b, size_t at, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[at + (size_t)i]; return v; }
std::vector<uint8_t> bitsOf(uint64_t v, int n) { std::vector<uint8_t> b; putBits(b, v, n); return b; }
std::vector<uint8_t> withCrc32(std::vector<uint8_t> b) { const uint32_t c = ale3gCrcBits(b, kAle3gCrc32, 32); putBits(b, c, 32); return b; }

const char* kPriority[4] = {"highest", "high", "routine", "low"};
const char* trafficType(int a) {
    static const char* t[17] = {"HDL 24 packets", "HDL 12 packets", "HDL 6 packets", "HDL 3 packets", "LDL 512 bytes", "LDL 256 bytes", "LDL 128 bytes", "LDL 64 bytes",
                                "ANDVT voice", "digital voice", "analog voice", "MIL-STD-188-110 serial", "high quality 9600", "high quality 6400", "high quality 4800",
                                "high quality 3200", "STANAG 4285"};
    if (a >= 0 && a < 17) return t[a];
    if (a == 61) return "packet";
    if (a == 62) return "circuit";
    if (a == 63) return "no traffic";
    return "reserved";
}
const char* termReason(int a) {
    static const char* t[7] = {"abort", "relink", "sign off", "unlink", "unlink acknowledged", "unlink, not all received", "suspend"};
    return a >= 0 && a < 7 ? t[a] : "reserved";
}
std::string addrText(int a) { char b[48]; snprintf(b, sizeof b, "%d (member %d, group %d)", a, a >> 5, a & 31); return b; }

std::string bytesText(const std::vector<uint8_t>& d) {
    size_t printable = 0;
    for (uint8_t c : d) printable += (c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t';
    std::string s;
    if (!d.empty() && printable * 10 >= d.size() * 9) { for (uint8_t c : d) s.push_back(c >= 32 && c < 127 ? (char)c : c == '\n' ? '\n' : ' '); return "\"" + s + "\""; }
    char b[4];
    for (size_t i = 0; i < d.size() && i < 64; i++) { snprintf(b, sizeof b, "%02X", d[i]); s += b; if (i + 1 < d.size()) s += ' '; }
    if (d.size() > 64) s += "...";
    return s;
}
}  // namespace

uint32_t ale3gCrcBits(const std::vector<uint8_t>& bits, uint32_t poly, int k) {
    const uint32_t top = 1u << (k - 1), mask = k >= 32 ? 0xFFFFFFFFu : (1u << k) - 1;
    uint32_t r = 0;
    for (uint8_t b : bits) {
        const uint32_t fb = (uint32_t)(b & 1) ^ ((r & top) ? 1u : 0u);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return r & mask;
}

uint64_t tmPdu(int type, int priority, int dest, int src, int arg) {
    std::vector<uint8_t> b;
    putBits(b, 1, 3);
    putBits(b, (uint64_t)(priority & 3), 2);
    putBits(b, (uint64_t)(dest & 0x7FF), 11);
    putBits(b, (uint64_t)(src & 0x7FF), 11);
    putBits(b, (uint64_t)(type & 7), 3);
    putBits(b, (uint64_t)(arg & 63), 6);
    putBits(b, ale3gCrcBits(b, kAle3gCrc12, 12), 12);
    return getBits(b, 0, 48);
}
uint64_t hdlAckPdu(uint32_t mask) {
    std::vector<uint8_t> tm;   // the CRC covers the Type and Ack Bit-Mask fields
    putBits(tm, 0, 1);
    putBits(tm, mask & 0xFFFFFFu, 24);
    std::vector<uint8_t> b;
    putBits(b, 0, 3);
    b.insert(b.end(), tm.begin(), tm.end());
    putBits(b, 0, 4);
    putBits(b, ale3gCrcBits(tm, kAle3gCrc16, 16), 16);
    return getBits(b, 0, 48);
}
uint64_t hdlEomPdu() { return (1ull << 44) | 0xA5A5A5A5A5Aull; }

Bw1Pdu bw1Parse(uint64_t v) {
    Bw1Pdu p;
    p.bits = v & 0xFFFFFFFFFFFFull;
    const std::vector<uint8_t> b = bitsOf(p.bits, 48);
    char t[300];
    const int proto = (int)getBits(b, 0, 3);
    if (proto == 1) {
        p.crcOk = ale3gCrcBits(std::vector<uint8_t>(b.begin(), b.begin() + 36), kAle3gCrc12, 12) == (uint32_t)getBits(b, 36, 12);
        const int pri = (int)getBits(b, 3, 2), dest = (int)getBits(b, 5, 11), src = (int)getBits(b, 16, 11), type = (int)getBits(b, 27, 3), arg = (int)getBits(b, 30, 6);
        const std::string d = dest == 0x7FF ? std::string("all (broadcast)") : addrText(dest), s = addrText(src);
        if (type == 0) { p.type = "TM_REQUEST"; snprintf(t, sizeof t, "%s asks %s for a traffic link: %s, priority %s", s.c_str(), d.c_str(), trafficType(arg), kPriority[pri]); }
        else if (type == 1) { p.type = "TM_CONFIRM"; snprintf(t, sizeof t, "%s confirms to %s: %s", s.c_str(), d.c_str(), trafficType(arg)); }
        else if (type == 2) { p.type = "TM_TERM"; snprintf(t, sizeof t, "%s to %s: link terminated, %s", s.c_str(), d.c_str(), termReason(arg)); }
        else { p.type = "TM PDU"; snprintf(t, sizeof t, "%s to %s: reserved type %d, argument %d", s.c_str(), d.c_str(), type, arg); }
    } else if (proto == 0 && b[3] == 0) {
        std::vector<uint8_t> tm(b.begin() + 3, b.begin() + 28);
        p.crcOk = ale3gCrcBits(tm, kAle3gCrc16, 16) == (uint32_t)getBits(b, 32, 16);
        p.type = "HDL_ACK";
        int acked = 0;
        std::string list;
        for (int i = 0; i < 24; i++) if (b[(size_t)(4 + i)]) { acked++; if (!list.empty()) list += ' '; list += std::to_string(i + 1); }
        snprintf(t, sizeof t, "%d of 24 packets received%s%s", acked, acked ? ": " : "", list.c_str());
    } else if (proto == 0) {
        p.crcOk = (p.bits & 0xFFFFFFFFFFFull) == 0xA5A5A5A5A5Aull;
        p.type = "HDL_EOM";
        snprintf(t, sizeof t, "end of the datagram (no more HDL_DATA)%s", p.crcOk ? "" : ", check pattern wrong");
    } else {
        p.type = "BW1 PDU";
        snprintf(t, sizeof t, "protocol %d, bits %012llX", proto, (unsigned long long)p.bits);
    }
    p.text = t;
    return p;
}

std::vector<uint8_t> bw1Encode(uint64_t bits48) {
    const std::vector<uint8_t> coded = convEncode(kBw1Code, bitsOf(bits48 & 0xFFFFFFFFFFFFull, 48), true);
    std::vector<uint8_t> out(144);
    const auto& o = bw1Order();
    for (int f = 0; f < 144; f++) out[(size_t)f] = coded[(size_t)o[(size_t)f]];
    return out;
}
uint64_t bw1Decode(const std::vector<float>& soft) {
    if (soft.size() != 144) return 0;
    std::vector<float> c(144);
    const auto& o = bw1Order();
    for (int f = 0; f < 144; f++) c[(size_t)o[(size_t)f]] = soft[(size_t)f];
    const auto d = viterbi(kBw1Code, c, true);
    return d.size() == 48 ? getBits(d, 0, 48) : 0;
}

std::vector<uint8_t> hdlPacket(const std::vector<uint8_t>& data, uint32_t seq) {
    std::vector<uint8_t> b;
    for (int i = 0; i < kHdlPayloadBytes; i++) putBits(b, i < (int)data.size() ? data[(size_t)i] : 0, 8);
    putBits(b, seq & 0x1FFFFu, 17);
    return withCrc32(b);
}
std::vector<uint8_t> ldlPacket(const std::vector<uint8_t>& data, int n, uint32_t seq) {
    std::vector<uint8_t> b;
    for (int i = 0; i < n; i++) putBits(b, i < (int)data.size() ? data[(size_t)i] : 0, 8);
    putBits(b, seq & 0x1FFFFu, 17);
    putBits(b, 0, 8);
    return b;
}
uint32_t seqOnly(int bytes) { return (1u << 16) | (1u << 15) | (uint32_t)(bytes & 0x7FFF); }
uint32_t seqFirst(int packets) { return (1u << 15) | (uint32_t)((packets - 1) & 0x7FFF); }
uint32_t seqInterior(int following) { return (uint32_t)(following & 0x7FFF); }
uint32_t seqLast(int bytes) { return (1u << 16) | (uint32_t)(bytes & 0x7FFF); }
std::string seqText(uint32_t s) {
    const bool eom = (s >> 16) & 1, som = (s >> 15) & 1;
    const int v = (int)(s & 0x7FFF);
    char t[96];
    if (eom && som) snprintf(t, sizeof t, "only packet of the datagram, %d bytes", v);
    else if (eom) snprintf(t, sizeof t, "last packet of the datagram, %d bytes", v);
    else if (som) snprintf(t, sizeof t, "first packet of a datagram of %d packets", v + 1);
    else snprintf(t, sizeof t, "packet with %d more to follow", v);
    return t;
}

std::vector<uint8_t> bw2Pn(int ft, size_t n) {
    uint32_t s = (uint32_t)(0xAB91 + ft) & 0xFFFFu;
    std::vector<uint8_t> o(n);
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) { const uint32_t nb = ((s >> 15) ^ (s >> 14) ^ (s >> 5) ^ s) & 1u; s = (s >> 1) | (nb << 15); }   // figure C-12 as drawn
        o[i] = (uint8_t)(s & 7u);
    }
    return o;
}

namespace {
// BW2 data symbols of one packet (640 gray-coded modulation symbols) from its encoded block
std::vector<uint8_t> bw2Symbols(const std::vector<uint8_t>& packet1913, int ft) {
    std::vector<uint8_t> e = packet1913;
    e.resize(1920, 0);   // seven flush bits
    const std::vector<uint8_t> c = convEncode(kBw2Code, e, false);
    const int blk = ft % 4, rot = ft % 3;
    std::vector<uint8_t> sy;
    for (int i = 0; i < 640; i++) {
        int m = (c[(size_t)(4 * (3 * i) + blk)] << 2) | (c[(size_t)(4 * (3 * i + 1) + blk)] << 1) | c[(size_t)(4 * (3 * i + 2) + blk)];
        for (int r = 0; r < rot; r++) m = ((m & 1) << 2) | (m >> 1);
        sy.push_back(kGray[m]);
    }
    return sy;
}
// the 8-PSK point index sent for modulation symbol M (bits as fetched, the first the most significant) at an FTcount
int bw2Label(int M, int ft) { for (int r = 0; r < ft % 3; r++) M = ((M & 1) << 2) | (M >> 1); return kGray[M]; }
}  // namespace

std::vector<cf32> genBw1(uint64_t bits48, double leadSec, double tailSec) {
    const uint8_t* tlc = ale3gTlcTable();
    const uint8_t* pn = ale3gPnTable();
    std::vector<uint8_t> sy(tlc, tlc + kNTlc);
    sy.insert(sy.end(), kPre1, kPre1 + kNPre1);
    const auto c = bw1Encode(bits48);
    for (int g = 0; g < 36; g++) {
        const int n = (c[(size_t)(4 * g)] << 3) | (c[(size_t)(4 * g + 1)] << 2) | (c[(size_t)(4 * g + 2)] << 1) | c[(size_t)(4 * g + 3)];
        for (int i = 0; i < 64; i++) sy.push_back((uint8_t)((walshChip(n, i & 15) + pn[(64 * g + i) & 255]) & 7));
    }
    return modulate(sy, leadSec, tailSec);
}

std::vector<cf32> genBw2(const std::vector<std::vector<uint8_t>>& packets, int ft, double leadSec, double tailSec) {
    const size_t np = packets.size();
    if (np != 3 && np != 6 && np != 12 && np != 24) return {};
    const uint8_t* tlc = ale3gTlcTable();
    std::vector<uint8_t> sy(tlc, tlc + kNTlc2);
    const auto pn = bw2Pn(ft, (size_t)kNPre2 + 960 * np);
    for (int i = 0; i < kNPre2; i++) sy.push_back(pn[(size_t)i]);
    size_t k = kNPre2;
    for (const auto& p : packets) {
        if (p.size() != 1913) return {};
        const auto ms = bw2Symbols(p, ft);
        for (int f = 0; f < 20; f++) {
            for (int i = 0; i < 32; i++) sy.push_back((uint8_t)((ms[(size_t)(32 * f + i)] + pn[k++]) & 7));
            for (int i = 0; i < 16; i++) sy.push_back(pn[k++]);
        }
    }
    return modulate(sy, leadSec, tailSec);
}

std::vector<cf32> genBw3(const std::vector<uint8_t>& packet, int n, int ft, double leadSec, double tailSec) {
    if (bw3Idx(n) < 0 || !bw3Decodable(n) || packet.size() != (size_t)(8 * n + 25)) return {};   // 256 and 512: see bw3Decodable
    std::vector<uint8_t> e = withCrc32(packet);
    e.resize((size_t)(8 * n + 64), 0);   // seven flush bits
    const auto c = convEncode(kBw3Code, e, false);
    const int blk = ft % 2;
    std::vector<uint8_t> eb;
    for (size_t i = 0; i < e.size(); i++) eb.push_back(c[2 * i + (size_t)blk]);
    const auto& o = bw3Order(n);
    std::vector<uint8_t> fetched(eb.size());
    for (size_t f = 0; f < eb.size(); f++) fetched[f] = eb[(size_t)std::max(0, o[f])];
    std::vector<uint8_t> sy(kPre3, kPre3 + kNPre3);
    for (int g = 0; g < 2 * n + 16; g++) {
        const int w = (fetched[(size_t)(4 * g)] << 3) | (fetched[(size_t)(4 * g + 1)] << 2) | (fetched[(size_t)(4 * g + 2)] << 1) | fetched[(size_t)(4 * g + 3)];
        for (int i = 0; i < 16; i++) sy.push_back((uint8_t)((walshChip(w, i) + kPn3[(16 * g + i) & 31]) & 7));
    }
    return modulate(sy, leadSec, tailSec);
}

std::vector<cf32> genBw4(int two, double leadSec, double tailSec) {
    const uint8_t* tlc = ale3gTlcTable();
    std::vector<uint8_t> sy(tlc, tlc + kNTlc);
    for (int i = 0; i < kNData4; i++) sy.push_back((uint8_t)((walshChip(two & 3, i & 3) + kPn4[i]) & 7));
    return modulate(sy, leadSec, tailSec);
}

// ---- receiver ------------------------------------------------------------------------------------------------------------------------

struct Ale3gTrafficRx::Impl {
    std::vector<float> h = rrc();
    std::vector<cf> hist;
    size_t hpos = 0;
    double nco = 0;
    std::vector<cf> y;
    int64_t y0 = 0, nY = 0, scan = 0;
    static constexpr int kHyp = 5, kSeg = 48, kStride = 5, kDetLen = 288;
    struct Det {
        int id = 0;                // 0 the TLC/AGC sequence (BW2, BW4), 1 the BW1 preamble, 3 the BW3 preamble
        const uint8_t* seq = nullptr;
        int len = 0, full = 0;     // symbols correlated to detect, symbols fitted for timing and frequency
        std::vector<cf> rh[kHyp];
        double candM = 0; int64_t candT = -1; cf candC; int candH = 2;
    };
    Det det[3];
    // the burst being read
    struct Pending {
        int id = -1;
        double t = 0, f = 0, noise = 1, snr = 0;
        cf hh;
        int stage = 0, ft = 0, packets = 0, good = 0;
        std::vector<float> soft;   // BW3: Walsh soft bits so far
        std::vector<float> failSoft[4];
        cf hTrack, hKnownPrev;     // BW2: the last known-symbol estimate
        // the paths of a multipath channel from the known sequence (RAKE fingers): offsets in samples and gains
        int nf = 1;
        double fOff[3] = {0, 0, 0};
        cf fH[3];
    } pend;
    double cfo = 0, phRef = 0, phPos = 0;
    uint64_t nBursts = 0, nGood = 0;
    std::vector<float> bw3Failed[4];   // the last BW3 encoded block of each size that failed its CRC (the other half may follow)
    // datagram reassembly (HDL and LDL)
    struct Reasm { int total = 0, lastBytes = -1; std::map<int, std::vector<uint8_t>> parts; };
    Reasm reasm[2];
    std::function<void(const Ale3gTraffic&)> cb;

    Impl() {
        hist.assign(h.size(), cf(0, 0));
        det[0].id = 0; det[0].seq = ale3gTlcTable(); det[0].len = kNTlc2; det[0].full = kNTlc2;
        det[1].id = 1; det[1].seq = kPre1; det[1].len = kDetLen; det[1].full = kNPre1;
        det[2].id = 3; det[2].seq = kPre3; det[2].len = kDetLen; det[2].full = kNPre3;
        for (auto& d : det) for (int hI = 0; hI < kHyp; hI++) {
            const double f = -80.0 + 40.0 * hI;
            for (int k = 0; k < d.len; k++) { const double a = -2 * kPi * f * k / kBaud; d.rh[hI].push_back(std::conj(psk8(d.seq[k])) * cf((float)std::cos(a), (float)std::sin(a))); }
        }
    }

    cf at(double pos) const {
        const int64_t i = (int64_t)std::floor(pos);
        const float fr = (float)(pos - (double)i);
        const int64_t a = i - y0;
        if (a < 0 || a + 1 >= (int64_t)y.size()) return cf(0, 0);
        const cf v = y[(size_t)a] * (1 - fr) + y[(size_t)a + 1] * fr;
        const double ph = phRef - 2 * kPi * cfo * (pos - phPos) / kFs;
        return v * cf((float)std::cos(ph), (float)std::sin(ph));
    }
    void setCfo(double f, double pos) { phRef = std::remainder(phRef - 2 * kPi * cfo * (pos - phPos) / kFs, 2 * kPi); phPos = pos; cfo = f; }
    cf fit(double t, const uint8_t* seq, int off, int lo, int hi) const {
        cf a(0, 0);
        for (int k = lo; k < hi; k++) a += at(t + (double)(off + k) * kSps) * std::conj(psk8(seq[k]));
        return a;
    }

    void emit(int wf, const std::string& type, const std::string& text, bool ok, std::vector<uint8_t> data = {}) {
        Ale3gTraffic t;
        t.waveform = wf; t.type = type; t.text = text; t.crcOk = ok; t.data = std::move(data); t.snrDb = pend.snr; t.cfoHz = cfo;
        if (ok) nGood++;
        if (cb) cb(t);
    }

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
        int64_t keepFrom = std::min<int64_t>(nY - 40000, scan - 2000);
        if (pend.id >= 0) keepFrom = std::min<int64_t>(keepFrom, (int64_t)pend.t - 4000);
        if (keepFrom > y0 + 40000) { y.erase(y.begin(), y.begin() + (long)(keepFrom - y0)); y0 = keepFrom; }
    }

    void work() {
        const int64_t span = (int64_t)kDetLen * kSps + 2;
        if (scan < y0 + kSps) scan = y0 + kSps;
        for (;;) {
            if (pend.id >= 0) {
                if (!progress()) return;   // waiting for more of the burst
                continue;
            }
            if (scan + span >= nY) return;
            const int64_t base = scan - y0;
            float en = 0, q[4] = {0, 0, 0, 0};
            for (int k = 0; k < kDetLen; k++) { const float e = std::norm(y[(size_t)(base + (int64_t)k * kSps)]); if (k < kNTlc2) en += e; q[std::min(3, k / 72)] += e; }
            const float qmax = std::max(std::max(q[0], q[1]), std::max(q[2], q[3])), qmin = std::min(std::min(q[0], q[1]), std::min(q[2], q[3]));
            const bool even = en > 0 && qmax < 2 * qmin;
            for (auto& d : det) {
                double m = 0;
                cf slope(0, 0);
                int hBest = 2;
                if (even) {
                    float ed = 0;
                    for (int k = 0; k < d.len; k++) ed += std::norm(y[(size_t)(base + (int64_t)k * kSps)]);
                    for (int hI = 0; hI < kHyp; hI++) {
                        double e = 0;
                        cf prev(0, 0), sl(0, 0);
                        for (int j = 0; j < d.len / kSeg; j++) {
                            cf sj(0, 0);
                            for (int k = j * kSeg; k < (j + 1) * kSeg; k++) sj += y[(size_t)(base + (int64_t)k * kSps)] * d.rh[hI][(size_t)k];
                            e += std::norm(sj);
                            if (j) sl += sj * std::conj(prev);
                            prev = sj;
                        }
                        const double mh = e / ((double)kSeg * ed);
                        if (mh > m) { m = mh; slope = sl; hBest = hI; }
                    }
                }
                if (m > 0.12 && m > d.candM) { d.candM = m; d.candT = scan; d.candC = slope; d.candH = hBest; }
            }
            bool started = false;
            for (auto& d : det) {
                if (d.candT >= 0 && scan > d.candT + (int64_t)d.len * kSps) {
                    if (!have((double)d.candT + (double)d.full * kSps + 40)) return;   // all of the known sequence first
                    const int64_t t = d.candT;
                    const double f0 = -80.0 + 40.0 * d.candH + std::arg(d.candC) * kBaud / (2 * kPi * kSeg);
                    d.candM = 0; d.candT = -1;
                    if (startBurst(d, t, f0)) { started = true; break; }
                }
            }
            if (!started) scan += kStride;
        }
    }

    // fine timing and frequency on the known sequence; the burst becomes pending
    bool startBurst(const Det& d, int64_t t, double f0) {
        phRef = 0; phPos = (double)t; cfo = f0;
        double bestT = (double)t, bestE = -1;
        for (double dt = -3.0; dt <= 3.0001; dt += 0.25) {
            const double e = std::abs(fit((double)t + dt, d.seq, 0, 0, d.full));
            if (e > bestE) { bestE = e; bestT = (double)t + dt; }
        }
        double f = f0;
        const int q = d.full / 4;
        for (int span : {q, 2 * q}) {
            phRef = 0; phPos = bestT; cfo = f;
            const cf a = fit(bestT, d.seq, 0, 0, span), b = fit(bestT, d.seq, 0, span, 2 * span);
            f += std::arg(b * std::conj(a)) * kBaud / (2 * kPi * (double)span);
        }
        phRef = 0; phPos = bestT; cfo = f;
        const cf hh = fit(bestT, d.seq, 0, 0, d.full) / (float)d.full;
        double er = 0;
        for (int k = 0; k < d.full; k++) er += std::norm(at(bestT + (double)k * kSps) - hh * psk8(d.seq[k]));
        pend = Pending();
        pend.id = d.id; pend.t = bestT; pend.f = f; pend.hh = hh;
        pend.noise = std::max(er / d.full, 1e-12);
        pend.snr = 10 * std::log10(std::norm(hh) / pend.noise);
        // the channel at half-symbol lags over +-6 symbols: up to three paths at least 7 dB below the strongest, a symbol apart
        {
            double pw[25];
            cf cv[25];
            for (int j = 0; j < 25; j++) { cv[j] = fit(bestT + (double)(j - 12) * 5.0, d.seq, 0, 0, d.full) / (float)d.full; pw[j] = std::norm(cv[j]); }
            pend.nf = 1; pend.fOff[0] = 0; pend.fH[0] = cv[12];
            bool used[25] = {};
            for (int j = 10; j <= 14; j++) used[j] = true;
            for (int f = 1; f < 3; f++) {
                int bj = -1;
                for (int j = 0; j < 25; j++) if (!used[j] && (j == 0 || pw[j] >= pw[j - 1]) && (j == 24 || pw[j] >= pw[j + 1]) && (bj < 0 || pw[j] > pw[bj])) bj = j;
                if (bj < 0 || pw[bj] < 0.2 * pw[12]) break;
                pend.fOff[f] = (double)(bj - 12) * 5.0; pend.fH[f] = cv[bj]; pend.nf++;
                for (int j = std::max(0, bj - 2); j <= std::min(24, bj + 2); j++) used[j] = true;
            }
        }
        if (pend.snr < -10) { pend.id = -1; return false; }
        return true;
    }

    void finish(double endPos, bool burst) {
        if (burst) { nBursts++; scan = std::max<int64_t>(scan, (int64_t)endPos); for (auto& d : det) { d.candM = 0; d.candT = -1; } }
        else scan += kStride;
        pend.id = -1;
    }

    // coherent 16-ary Walsh symbols (16 chips, repeated rep times), the channel followed from symbol to symbol; soft bits b3 first
    // With several paths (pend.nf fingers) the correlations of each path are added with their gains (RAKE); hh is then not used.
    void walsh16(double t0, int nSym, int rep, int pnLen, const uint8_t* pn, cf& hh, double noise, std::vector<float>& soft, int g0 = 0) {
        const int nC = 16 * rep, nf = pend.nf;
        std::vector<cf> z((size_t)(nC * nf));
        if (nf == 1) pend.fH[0] = hh;
        for (int g = g0; g < g0 + nSym; g++) {
            for (int f = 0; f < nf; f++)
                for (int i = 0; i < nC; i++) z[(size_t)(f * nC + i)] = at(t0 + (double)(nC * g + i) * kSps + pend.fOff[f]) * std::conj(psk8(pn[(nC * g + i) % pnLen]));
            float met[16];
            cf cc[3][16];
            int best = 0;
            for (int n = 0; n < 16; n++) {
                met[n] = 0;
                for (int f = 0; f < nf; f++) {
                    cf a(0, 0);
                    for (int i = 0; i < nC; i++) a += walshChip(n, i & 15) ? -z[(size_t)(f * nC + i)] : z[(size_t)(f * nC + i)];
                    cc[f][n] = a;
                    met[n] += (a * std::conj(pend.fH[f])).real();
                }
                if (met[n] > met[best]) best = n;
            }
            const float scale = (float)(1.0 / (0.5 * nC * noise));
            for (int k = 0; k < 4; k++) {
                float m0 = -1e30f, m1 = -1e30f;
                for (int n = 0; n < 16; n++) { if ((n >> (3 - k)) & 1) m1 = std::max(m1, met[n]); else m0 = std::max(m0, met[n]); }
                soft.push_back(std::clamp((m0 - m1) * scale, -50.f, 50.f));
            }
            for (int f = 0; f < nf; f++) pend.fH[f] = 0.5f * pend.fH[f] + 0.5f * cc[f][best] / (float)nC;
            hh = pend.fH[0];
        }
    }

    bool have(double pos) const { return pos + 8 < (double)nY; }

    // returns false while waiting for samples
    bool progress() {
        Pending& p = pend;
        if (p.id == 1) {   // BW1: TM, HDL_ACK, HDL_EOM
            const double d0 = p.t + (double)kNPre1 * kSps;
            if (!have(d0 + (double)kNData1 * kSps)) return false;
            std::vector<float> soft;
            cf hh = p.hh;
            walsh16(d0, 36, 4, 256, ale3gPnTable(), hh, p.noise, soft);
            const Bw1Pdu r = bw1Parse(bw1Decode(soft));
            const bool show = r.crcOk || p.snr >= 3;
            if (show) emit(1, r.type, r.text, r.crcOk);
            finish(d0 + (double)kNData1 * kSps, show);
            return true;
        }
        if (p.id == 3) return progressBw3();
        if (p.id == 0) return progressTlc();
        if (p.id == 2) return progressBw2();
        if (p.id == 4) return progressBw4();
        pend.id = -1;
        return true;
    }

    bool progressBw3() {
        Pending& p = pend;
        const double d0 = p.t + (double)kNPre3 * kSps;
        for (;;) {
            if (p.stage >= 4) break;
            const int n = kBw3N[p.stage];
            if (!bw3Decodable(n)) { p.stage = 4; break; }
            const int nSym = 2 * n + 16;
            if (!have(d0 + (double)(16 * nSym) * kSps)) return false;
            const int done = (int)p.soft.size() / 4;
            if (done < nSym) { cf hh = p.hTrack == cf(0, 0) ? p.hh : p.hTrack; walsh16(d0, nSym - done, 1, 32, kPn3, hh, p.noise, p.soft, done); p.hTrack = hh; }
            const int nb = 8 * n + 64;
            const auto& o = bw3Order(n);
            std::vector<float> eb((size_t)nb);
            for (int f = 0; f < nb; f++) eb[(size_t)o[(size_t)f]] = p.soft[(size_t)f];
            auto tryDecode = [&](const std::vector<float>& b0, const std::vector<float>& b1, int& ftParity) -> bool {
                std::vector<float> s((size_t)(2 * nb));
                for (int i = 0; i < nb; i++) { s[(size_t)(2 * i)] = b0.empty() ? 0.f : b0[(size_t)i]; s[(size_t)(2 * i + 1)] = b1.empty() ? 0.f : b1[(size_t)i]; }
                const auto d = viterbi(kBw3Code, s, false);
                if ((int)d.size() != nb) return false;
                const std::vector<uint8_t> pk(d.begin(), d.begin() + 8 * n + 25);
                if (ale3gCrcBits(pk, kAle3gCrc32, 32) != (uint32_t)getBits(d, (size_t)(8 * n + 25), 32)) return false;
                ldlData(pk, n, ftParity);
                return true;
            };
            int par0 = 0, par1 = 1, comb = 2;
            std::vector<float>& prev = bw3Failed[p.stage];
            const bool ok = tryDecode(eb, {}, par0) || tryDecode({}, eb, par1) ||
                            (prev.size() == eb.size() && (tryDecode(prev, eb, comb) || tryDecode(eb, prev, comb)));
            if (ok) { prev.clear(); finish(d0 + (double)(16 * nSym) * kSps, true); return true; }
            p.failSoft[p.stage] = eb;
            p.stage++;
        }
        // no size gave a good CRC: keep the encoded blocks for the retransmission (the other half of the code), report if strong
        for (int i = 0; i < 4; i++) if (!p.failSoft[i].empty()) bw3Failed[i] = p.failSoft[i];
        const bool show = p.snr >= 3;
        if (show) emit(3, "LDL_DATA", "BW3 burst, CRC bad for 64 and 128 bytes (256 and 512: see the documentation)", false);
        finish(d0, show);
        return true;
    }

    void ldlData(const std::vector<uint8_t>& pk, int n, int how) {
        std::vector<uint8_t> data((size_t)n);
        for (int i = 0; i < n; i++) data[(size_t)i] = (uint8_t)getBits(pk, (size_t)(8 * i), 8);
        const uint32_t seq = (uint32_t)getBits(pk, (size_t)(8 * n), 17);
        char t[200];
        snprintf(t, sizeof t, "%d-byte packet, %s%s: ", n, seqText(seq).c_str(), how == 2 ? " (two transmissions combined)" : how == 1 ? " (second half of the code)" : "");
        std::vector<uint8_t> shown = data;
        if ((seq >> 16) & 1) shown.resize(std::min<size_t>(data.size(), seq & 0x3FF));
        emit(3, "LDL_DATA", std::string(t) + bytesText(shown), true, shown);
        reassemble(1, seq, data);
    }

    void reassemble(int k, uint32_t seq, const std::vector<uint8_t>& data) {
        Reasm& r = reasm[k];
        const bool eom = (seq >> 16) & 1, som = (seq >> 15) & 1;
        const int v = (int)(seq & 0x7FFF);
        if (eom && som) return;   // a single packet was shown already
        if (som) { r = Reasm(); r.total = v + 1; r.parts[0] = data; }
        else if (r.total == 0) return;
        else if (eom) { r.lastBytes = v; r.parts[r.total - 1] = data; }
        else if (v < r.total) r.parts[r.total - 1 - v] = data;
        if (r.lastBytes < 0 || (int)r.parts.size() != r.total) return;
        std::vector<uint8_t> all;
        for (auto& kv : r.parts) {
            if (kv.first == r.total - 1) all.insert(all.end(), kv.second.begin(), kv.second.begin() + std::min<long>((long)kv.second.size(), r.lastBytes));
            else all.insert(all.end(), kv.second.begin(), kv.second.end());
        }
        char t[96];
        snprintf(t, sizeof t, "%s datagram complete, %d packets, %zu bytes: ", k ? "LDL" : "HDL", r.total, all.size());
        emit(k ? 3 : 2, "datagram", std::string(t) + bytesText(all), true, all);
        r = Reasm();
    }

    // after the TLC/AGC sequence: a BW2 preamble (its FTcount found from the PN sequence), else the BW4 data
    bool progressTlc() {
        Pending& p = pend;
        const double pre = p.t + (double)kNTlc2 * kSps;
        if (p.stage == 0) {
            if (!have(pre + (double)kNPre2 * kSps)) return false;
            std::vector<cf> z((size_t)kNPre2);
            float e = 0;
            for (int k = 0; k < kNPre2; k++) { z[(size_t)k] = at(pre + (double)k * kSps); e += std::abs(z[(size_t)k]); }
            double best = 0, second = 0;
            int bestFt = 0;
            cf bestC;
            for (int ft = 0; ft < 128; ft++) {
                const auto pn = bw2Pn(ft, kNPre2);
                cf c(0, 0);
                for (int k = 0; k < kNPre2; k++) c += z[(size_t)k] * std::conj(psk8(pn[(size_t)k]));
                const double m = e > 0 ? std::abs(c) / e : 0;
                if (m > best) { second = best; best = m; bestFt = ft; bestC = c; } else if (m > second) second = m;
            }
            if (best > 0.55 && best > 1.6 * second) {
                p.id = 2; p.ft = bestFt; p.stage = 0;
                p.hKnownPrev = bestC / (float)kNPre2;
                return true;
            }
            p.stage = 1;
        }
        // BW4: 1280 chips of a 4-ary Walsh word, added in power over 20 segments of 64
        const double d0 = p.t + (double)kNTlc * kSps;
        if (!have(d0 + (double)kNData4 * kSps)) return false;
        p.id = 4;
        return true;
    }

    bool progressBw4() {
        Pending& p = pend;
        const double d0 = p.t + (double)kNTlc * kSps;
        double met[4] = {0, 0, 0, 0}, en = 0;
        for (int s = 0; s < 20; s++) for (int f = 0; f < p.nf; f++) {   // every path, added in power
            cf a[4] = {};
            for (int i = 64 * s; i < 64 * (s + 1); i++) {
                const cf z = at(d0 + (double)i * kSps + p.fOff[f]) * std::conj(psk8(kPn4[i]));
                en += std::norm(z);
                for (int hI = 0; hI < 4; hI++) a[hI] += walshChip(hI, i & 3) ? -z : z;
            }
            for (int hI = 0; hI < 4; hI++) met[hI] += std::norm(a[hI]);
        }
        int b = 0;
        for (int hI = 1; hI < 4; hI++) if (met[hI] > met[b]) b = hI;
        double second = 0;
        for (int hI = 0; hI < 4; hI++) if (hI != b) second = std::max(second, met[hI]);
        const double m = en > 0 ? met[b] / (64.0 * en) : 0;
        if (m > 0.1 && met[b] > 4 * second) {   // noise alone gives about 1/64; below this a wrong word becomes likely
            static const char* t[4] = {"NAK, datagram not complete", "NAK, datagram complete", "ACK, datagram not complete", "ACK, datagram complete"};
            char s[160];
            snprintf(s, sizeof s, "bits %d%d: %s%s", (b >> 1) & 1, b & 1, t[b], b == 3 ? " (from the data sender: LDL_EOM, end of the datagram)" : "");
            emit(4, "LDL_ACK", s, true);
            finish(d0 + (double)kNData4 * kSps, true);
        } else {
            finish(0, false);   // a TLC/AGC sequence of another burst (BW0, BW1), or noise
        }
        return true;
    }

    bool progressBw2() {
        Pending& p = pend;
        const double d0 = p.t + (double)(kNTlc2 + kNPre2) * kSps;
        const auto pn = bw2Pn(p.ft, (size_t)kNPre2 + 960 * 24);
        for (;;) {
            if (p.stage == 1) {   // after 3, 6 or 12 packets: the end of the forward transmission, or known symbols go on
                const int np = p.packets;
                const double nx = d0 + (double)(960 * np) * kSps;
                if (!have(nx + 48.0 * kSps)) return false;
                cf c(0, 0);
                double e = 0;
                for (int i = 32; i < 48; i++) { const cf v = at(nx + (double)i * kSps); c += v * std::conj(psk8(pn[(size_t)(kNPre2 + 960 * np + i)])); e += std::norm(v); }
                p.stage = 0;
                if (e <= 0 || std::norm(c) / (16.0 * e) < 0.3) break;
            }
            const int k = p.packets;
            const double pEnd = d0 + (double)(960 * (k + 1)) * kSps;
            if (!have(pEnd)) return false;
            // channel from the 16 known symbols of each frame, interpolated over the 32 data symbols
            std::vector<float> soft;
            soft.reserve(1920);
            for (int fr = 0; fr < 20; fr++) {
                const int i0 = 960 * k + 48 * fr;
                const double fp = d0 + (double)i0 * kSps;
                cf hk(0, 0);
                for (int i = 32; i < 48; i++) hk += at(fp + (double)i * kSps) * std::conj(psk8(pn[(size_t)(kNPre2 + i0 + i)]));
                hk /= 16.0f;
                // frequency from the phase change between known blocks (20 ms)
                const double dph = std::arg(hk * std::conj(p.hKnownPrev));
                const double here = fp + 40.0 * kSps;
                const cf hPrev = p.hKnownPrev;
                setCfo(cfo + 0.2 * dph / (2 * kPi * 0.02), here);
                double nz = 0;
                for (int i = 32; i < 48; i++) nz += std::norm(at(fp + (double)i * kSps) - hk * psk8(pn[(size_t)(kNPre2 + i0 + i)]));
                p.noise = 0.8 * p.noise + 0.2 * std::max(nz / 16, 1e-9);
                for (int i = 0; i < 32; i++) {
                    const float u = (float)(i + 8.5) / 48.0f;
                    const cf hc = hPrev * (1 - u) + hk * u;
                    const cf z = at(fp + (double)i * kSps) * std::conj(psk8(pn[(size_t)(kNPre2 + i0 + i)]));
                    float b0[3] = {1e30f, 1e30f, 1e30f}, b1[3] = {1e30f, 1e30f, 1e30f};
                    for (int M = 0; M < 8; M++) {
                        const float dd = std::norm(z - hc * psk8(bw2Label(M, p.ft)));
                        for (int j = 0; j < 3; j++) { if ((M >> (2 - j)) & 1) b1[j] = std::min(b1[j], dd); else b0[j] = std::min(b0[j], dd); }
                    }
                    for (int j = 0; j < 3; j++) soft.push_back(std::clamp((b1[j] - b0[j]) / (float)p.noise, -50.f, 50.f));
                }
                p.hKnownPrev = hk;
            }
            std::vector<float> s4(4 * 1920, 0.f);
            for (int i = 0; i < 1920; i++) s4[(size_t)(4 * i + p.ft % 4)] = soft[(size_t)i];
            const auto d = viterbi(kBw2Code, s4, false);
            const std::vector<uint8_t> pk(d.begin(), d.begin() + 1881);
            const bool ok = d.size() == 1920 && ale3gCrcBits(pk, kAle3gCrc32, 32) == (uint32_t)getBits(d, 1881, 32);
            p.packets++;
            if (ok) {
                p.good++;
                std::vector<uint8_t> data(kHdlPayloadBytes);
                for (int i = 0; i < kHdlPayloadBytes; i++) data[(size_t)i] = (uint8_t)getBits(pk, (size_t)(8 * i), 8);
                const uint32_t seq = (uint32_t)getBits(pk, 1864, 17);
                std::vector<uint8_t> shown = data;
                if ((seq >> 16) & 1) shown.resize(std::min<size_t>(data.size(), seq & 0x1FF));
                char t[160];
                snprintf(t, sizeof t, "packet %d (forward transmission %d), %s: ", p.packets, p.ft, seqText(seq).c_str());
                emit(2, "HDL_DATA", std::string(t) + bytesText(shown), true, shown);
                reassemble(0, seq, data);
            } else if (p.snr >= 3) {
                char t[96];
                snprintf(t, sizeof t, "packet %d (forward transmission %d): CRC bad", p.packets, p.ft);
                emit(2, "HDL_DATA", t, false);
            }
            // the end of the forward transmission: 3, 6, 12 or 24 packets, then no more known symbols
            const int np = p.packets;
            if (np == 24) break;
            if (np == 3 || np == 6 || np == 12) p.stage = 1;
        }
        char t[120];
        snprintf(t, sizeof t, "forward transmission %d: %d packets, %d with a good CRC", p.ft, p.packets, p.good);
        emit(2, "HDL_DATA", t, p.good > 0);
        if (p.good > 0) nGood--;   // the summary is not a PDU of its own
        finish(d0 + (double)(960 * p.packets) * kSps, true);
        return true;
    }
};

Ale3gTrafficRx::Ale3gTrafficRx() : p_(new Impl) {}
Ale3gTrafficRx::~Ale3gTrafficRx() = default;
void Ale3gTrafficRx::reset() { auto c = std::move(p_->cb); p_.reset(new Impl); p_->cb = std::move(c); }
void Ale3gTrafficRx::push(const cf32* x, size_t n) { p_->push(x, n); }
void Ale3gTrafficRx::setSink(std::function<void(const Ale3gTraffic&)> cb) { p_->cb = std::move(cb); }
uint64_t Ale3gTrafficRx::bursts() const { return p_->nBursts; }
uint64_t Ale3gTrafficRx::good() const { return p_->nGood; }

}  // namespace dect2::hfu
