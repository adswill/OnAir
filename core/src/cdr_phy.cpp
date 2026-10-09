// CDR physical layer building blocks (see cdr_defs.h); clause and table numbers refer to GY/T 268.1-2013 and GY/T 268.2-2013.
#include "dect2/cdr_defs.h"
#include "dect2/cdr_ldpc.h"
#include "dect2/drm_fec.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2::cdr {

namespace {

const TxParams kTx[3] = {
    // tm ns    nb    tu    tcp  ts    tbcp tb    tg  sn   nv   df        dfb       L    nzc  m   siRows/Row siPos                                  sdis    msds   sdis  Q
    {1, 2048, 1024, 2048, 240, 2288, 384, 2432, 23, 56, 242, 398.4375, 796.875, 120, 967, 48, 27, 4, {{11, 55, 75, 103}, {144, 164, 192, 228}}, 2, 0, 46080, 1704, {846, 1698, 2550}},
    {2, 1024, 512, 1024, 140, 1164, 332, 1356, 12, 111, 122, 796.875, 1593.75, 60, 487, 12, 54, 2, {{15, 43, 0, 0}, {84, 104, 0, 0}}, 3, 72, 46080, 1576, {782, 1570, 2358}},
    {3, 2048, 1024, 2048, 56, 2104, 168, 2216, 21, 61, 242, 398.4375, 796.875, 120, 967, 48, 27, 4, {{11, 55, 75, 103}, {144, 164, 192, 228}}, 1, 128, 50688, 1360, {674, 1354, 2034}},
};

const SpectrumMode kSpec[6] = {
    {1, false, 1, 0, 2, {{3, false}, {3, true}}, 0, 50, "100 kHz, all digital"},
    {2, true, 2, 50, 4, {{2, false}, {2, true}, {3, false}, {3, true}}, 0, 100, "200 kHz, all digital"},
    {9, true, 1, 150, 2, {{1, false}, {4, true}}, 150, 200, "2 x 50 kHz beside stereo FM"},
    {10, false, 2, 200, 4, {{1, false}, {1, true}, {5, false}, {5, true}}, 150, 250, "2 x 100 kHz beside stereo FM"},
    {22, false, 1, 100, 2, {{2, false}, {4, true}}, 100, 150, "2 x 50 kHz beside mono FM"},
    {23, true, 2, 150, 4, {{1, false}, {1, true}, {4, false}, {4, true}}, 100, 200, "2 x 100 kHz beside mono FM"},
};

// 5.6.2: columns (1-based) of the scattered pilots in a row of Ms,t whose row number a gives type = (a - 1) mod 3
std::vector<int> pilotColumns(int tm, int type) {
    std::vector<int> c;
    if (tm == 2) {
        static const int up[3] = {62, 66, 70}, lo[3] = {61, 57, 53}, nUp[3] = {5, 4, 4}, nLo[3] = {5, 4, 4};
        for (int p = -nLo[type]; p <= 0; p++) c.push_back(12 * p + lo[type]);
        for (int p = 0; p <= nUp[type]; p++) c.push_back(12 * p + up[type]);
    } else {
        static const int up[3] = {122, 126, 130}, lo[3] = {121, 117, 113}, nUp[3] = {10, 9, 9}, nLo[3] = {10, 9, 9};
        for (int p = -nLo[type]; p <= 0; p++) c.push_back(12 * p + lo[type]);
        for (int p = 0; p <= nUp[type]; p++) c.push_back(12 * p + up[type]);
    }
    return c;
}

std::shared_ptr<const Layout> buildLayout(int tm, int sm) {
    const TxParams* tp = txParams(tm);
    const SpectrumMode* sp = spectrumMode(sm);
    if (!tp || !sp) return nullptr;
    auto L = std::make_shared<Layout>();
    Layout& l = *L;
    l.tm = tm; l.sm = sm; l.tp = tp; l.spec = sp; l.ni = sp->ni; l.nv = tp->nv; l.sn = tp->sn; l.cols = tp->nv * sp->ni;
    for (int h = 0; h < sp->halves; h++) {
        const auto c = halfCarriers(tm, sp->classA, sp->half[h].band, sp->half[h].upper, false);
        l.carrier.insert(l.carrier.end(), c.begin(), c.end());
        const auto s = halfCarriers(tm, sp->classA, sp->half[h].band, sp->half[h].upper, true);
        l.syncCarrier.insert(l.syncCarrier.end(), s.begin(), s.end());
    }
    if ((int)l.carrier.size() != l.cols || (int)l.syncCarrier.size() != tp->L * l.ni) return nullptr;
    // 5.9.1: Pb(n) = exp(-j (-1)^n 2 pi m (n (n + 1) / 2) / Nzc)
    for (int n = 0; n < (int)l.syncCarrier.size(); n++) {
        const long long tri = ((long long)n * (n + 1) / 2) % tp->nzc;
        const double ph = 2 * M_PI * (double)tp->m * (double)tri / tp->nzc * ((n & 1) ? -1.0 : 1.0);
        l.beaconSeq.push_back(cf32((float)std::cos(ph), (float)-std::sin(ph)));
    }
    const int sn = l.sn, cols = l.cols, nv = l.nv, half = nv / 2;
    l.kind.assign((size_t)(sn * cols), kElemData);
    l.siSym.assign(l.kind.size(), -1);
    l.siHalf.assign(l.kind.size(), -1);
    l.pilot.assign(l.kind.size(), cf32(0, 0));
    for (int a = 0; a < sn; a++) {
        const std::vector<int> pc = pilotColumns(tm, a % 3);
        for (int t = 0; t < l.ni; t++) {
            for (int u = 0; u < 2; u++)
                for (int k = 0; k < tp->siPerRow; k++) {
                    const size_t e = (size_t)(a * cols + t * nv + tp->siPos[u][k] - 1);
                    l.kind[e] = kElemSi;
                    l.siSym[e] = (int16_t)((a % tp->siRows) * tp->siPerRow + k);
                    l.siHalf[e] = (int16_t)(2 * t + u);
                }
            for (int b : pc) {
                const size_t e = (size_t)(a * cols + t * nv + b - 1);
                if (l.kind[e] != kElemData) return nullptr;     // the tables never collide
                l.kind[e] = kElemPilot;
            }
        }
    }
    (void)half;
    // 5.5 / 5.6.2: pl pilot symbols from the x^11 + x^9 + 1 register (initial 10100101010; pI from the last stage, pQ after the third),
    // QPSK with beta = sqrt 2, written into the pilot elements of rows 1 to 3 from left to right, top to bottom; later rows repeat them
    {
        uint16_t r = 0;
        const int init[11] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
        for (int i = 0; i < 11; i++) r |= (uint16_t)(init[i] << i);          // bit i = stage D(i+1)
        for (int a = 0; a < 3 && a < sn; a++)
            for (int c = 0; c < cols; c++) {
                const size_t e = (size_t)(a * cols + c);
                if (l.kind[e] != kElemPilot) continue;
                const uint8_t b[2] = {(uint8_t)((r >> 10) & 1), (uint8_t)((r >> 2) & 1)};
                const int fb = ((r >> 8) ^ (r >> 10)) & 1;
                r = (uint16_t)(((r << 1) | fb) & 0x7FF);
                l.pilot[e] = mapBits(b, kQpsk, (float)std::sqrt(2.0));
            }
        for (int a = 3; a < sn; a++)
            for (int c = 0; c < cols; c++) {
                const size_t e = (size_t)(a * cols + c);
                if (l.kind[e] == kElemPilot) l.pilot[e] = l.pilot[(size_t)((a % 3) * cols + c)];
            }
    }
    // 5.6.3: SDIS and MSDS fill orders (sub-matrices M1,t .. M4,t, then the next sub-band t: Figure 19)
    const int perSub = sn * cols;
    std::vector<int> fillIndex((size_t)(4 * perSub), -1);
    std::vector<uint8_t> isMsds((size_t)(4 * perSub), 0);
    std::vector<int> fill;
    for (int t = 0; t < l.ni; t++)
        for (int q = 0; q < 4; q++)
            for (int a = 0; a < sn; a++) {
                int seen = 0;
                for (int b = 0; b < nv; b++) {
                    const int c = t * nv + b;
                    if (l.kind[(size_t)(a * cols + c)] != kElemData) continue;
                    const int pos = q * perSub + a * cols + c;
                    const bool sdis = a < tp->sdisRows || (a == tp->sdisRows && seen < tp->sdisValid);
                    seen++;
                    if (sdis) l.sdisPos.push_back(pos);
                    else { fillIndex[(size_t)pos] = (int)fill.size(); fill.push_back(pos); isMsds[(size_t)pos] = 1; }
                }
            }
    if ((int)l.sdisPos.size() != tp->sdisPerBand * l.ni || (int)fill.size() != tp->msdsPerBand * l.ni) return nullptr;
    // 5.7: interleaving blocks B_j: the MSDS of row r' (counted over the MSDS rows of the logical frame) from sub-band l, where
    // j = ((r' - 1)(NI - 1) + (l - 1)) mod NI + 1; each block goes through the interleaver of 5.3.1 and back to the same places
    const int rowsPerSub = sn - tp->sdisRows, ni = l.ni;
    const std::vector<int>& R = interleaver(tp->msdsPerBand);
    l.msdsPos.assign(fill.size(), -1);
    for (int j = 1; j <= ni; j++) {
        std::vector<int> list;
        list.reserve((size_t)tp->msdsPerBand);
        for (int rp = 1; rp <= 4 * rowsPerSub; rp++) {
            const int k = (rp - 1) / rowsPerSub, a = tp->sdisRows + (rp - 1) % rowsPerSub;
            const int lm1 = (((j - 1) - (rp - 1) * (ni - 1)) % ni + ni) % ni;
            for (int b = 0; b < nv; b++) {
                const int pos = k * perSub + a * cols + lm1 * nv + b;
                if (isMsds[(size_t)pos]) list.push_back(pos);
            }
        }
        if ((int)list.size() != tp->msdsPerBand) return nullptr;
        for (int n = 0; n < tp->msdsPerBand; n++) l.msdsPos[(size_t)fillIndex[(size_t)list[(size_t)R[(size_t)n]]]] = list[(size_t)n];
    }
    return L;
}

} // namespace

const TxParams* txParams(int tm) { return tm >= 1 && tm <= 3 ? &kTx[tm - 1] : nullptr; }

const char* modText(int mod) { static const char* t[3] = {"QPSK", "16QAM", "64QAM"}; return mod >= 0 && mod < 3 ? t[mod] : "-"; }

const SpectrumMode* spectrumMode(int index) {
    for (const auto& s : kSpec) if (s.index == index) return &s;
    return nullptr;
}
const std::vector<int>& spectrumModeIndices() { static const std::vector<int> v = {1, 2, 9, 10, 22, 23}; return v; }
int nominalCode(int khz) { return khz / 50; }

std::vector<int> halfCarriers(int tm, bool classA, int band, bool upper, bool sync) {
    static const int ofdm13B[5] = {-502, -250, 0, 250, 502}, ofdm13A[4] = {-376, -126, 126, 376};
    static const int ofdm2B[5] = {-250, -126, 0, 126, 250}, ofdm2A[4] = {-188, -62, 62, 188};
    static const int sync13B[5] = {-251, -125, 0, 125, 251}, sync13A[4] = {-188, -63, 63, 188};
    static const int sync2B[5] = {-125, -63, 0, 63, 125}, sync2A[4] = {-94, -31, 31, 94};
    const bool t2 = tm == 2;
    if (band < 1 || band > (classA ? 4 : 5)) return {};
    const int* c = sync ? (t2 ? (classA ? sync2A : sync2B) : (classA ? sync13A : sync13B)) : (t2 ? (classA ? ofdm2A : ofdm2B) : (classA ? ofdm13A : ofdm13B));
    const int h = sync ? (t2 ? 30 : 60) : (t2 ? 61 : 121);
    const int centre = c[band - 1];
    std::vector<int> v;
    for (int i = 1; i <= h; i++) v.push_back(upper ? centre + i : centre - h - 1 + i);
    return v;
}

int Layout::msdBits(int mod, int rate) const { return codewordsFor(mod) * ldpcInfoBits(rate); }

void physToLogical(int alloc, int frame, int sub, int& p, int& q) {
    if (alloc == 2) {             // Figure 25: SF1,1 SF2,1 SF1,2 SF2,2 | SF1,3 SF2,3 SF1,4 SF2,4 over two physical frames
        const int z = (frame & 1) * 4 + sub;
        p = (frame & ~1) + (z & 1);
        q = z >> 1;
    } else if (alloc == 3) {      // Figure 26: physical frame f carries sub-frame f of every logical frame
        p = sub;
        q = frame;
    } else { p = frame; q = sub; }
}

void logicalToPhys(int alloc, int p, int q, int& frame, int& sub) {
    for (int f = 0; f < 4; f++)
        for (int s = 0; s < 4; s++) {
            int pp, qq;
            physToLogical(alloc, f, s, pp, qq);
            if (pp == p && qq == q) { frame = f; sub = s; return; }
        }
    frame = p; sub = q;
}

std::shared_ptr<const Layout> layoutFor(int tm, int sm) {
    static std::mutex mu;
    static std::map<int, std::shared_ptr<const Layout>> cache;
    std::lock_guard<std::mutex> lk(mu);
    const int key = tm * 100 + sm;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto l = buildLayout(tm, sm);
    cache[key] = l;
    return l;
}

const std::vector<int>& interleaver(int nmux) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<std::vector<int>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = cache[nmux];
    if (!slot) {
        slot = std::make_unique<std::vector<int>>();
        int s = 1;
        while (s < nmux) s <<= 1;                  // s = 2^ceil(log2 NMUX)
        const int g = s / 4 - 1;
        long long p = 0;
        for (int i = 0; i < s; i++) {
            if (i > 0) p = (5 * p + g) % s;
            if (p < nmux) slot->push_back((int)p);
        }
    }
    return *slot;
}

// ---------------------------------------------------------------- constellations

namespace {
inline float level16(int s, int a) { return (float)((1 - 2 * s) * (3 - 2 * a)); }
inline float level64(int s, int a, int b) { return (float)((1 - 2 * s) * (7 - 4 * a - 2 * (a ^ b))); }
const float kQ16 = (float)(1.0 / std::sqrt(10.0)), kQ64 = (float)(1.0 / std::sqrt(42.0));

// per axis: LLRs of the axis bits (sign bit first) from the coordinate x
void axisLlr(float x, float nvar, int mod, float beta, float* out) {
    if (mod == kQpsk) { const float a = beta * (float)M_SQRT1_2; out[0] = 4.f * a * x / nvar; return; }
    const int nb = mod == k16Qam ? 2 : 3, nl = 1 << nb;
    float best[3][2] = {{1e30f, 1e30f}, {1e30f, 1e30f}, {1e30f, 1e30f}};
    for (int v = 0; v < nl; v++) {
        const int s = (v >> (nb - 1)) & 1, a = (v >> (nb - 2)) & 1, b = nb == 3 ? (v & 1) : 0;
        const float lv = nb == 2 ? level16(s, a) * kQ16 : level64(s, a, b) * kQ64;
        const float d = (x - lv) * (x - lv);
        const int bits[3] = {s, a, b};
        for (int k = 0; k < nb; k++) best[k][bits[k]] = std::min(best[k][bits[k]], d);
    }
    for (int k = 0; k < nb; k++) out[k] = (best[k][1] - best[k][0]) / nvar;
}
} // namespace

cf32 mapBits(const uint8_t* b, int mod, float beta) {
    if (mod == kQpsk) { const float a = beta * (float)M_SQRT1_2; return cf32((1 - 2 * (b[0] & 1)) * a, (1 - 2 * (b[1] & 1)) * a); }
    if (mod == k16Qam) return cf32(level16(b[0] & 1, b[2] & 1) * kQ16, level16(b[1] & 1, b[3] & 1) * kQ16);
    return cf32(level64(b[0] & 1, b[2] & 1, b[4] & 1) * kQ64, level64(b[1] & 1, b[3] & 1, b[5] & 1) * kQ64);
}

void demap(cf32 z, float nvar, int mod, float* llr, float beta) {
    nvar = std::max(nvar, 1e-6f);
    float li[3], lq[3];
    axisLlr(z.real(), nvar, mod, beta, li);
    axisLlr(z.imag(), nvar, mod, beta, lq);
    // I carries v0 v2 v4, Q carries v1 v3 v5
    const int nb = mod == kQpsk ? 1 : mod == k16Qam ? 2 : 3;
    for (int k = 0; k < nb; k++) { llr[2 * k] = li[k]; llr[2 * k + 1] = lq[k]; }
}

void scrambleBits(uint8_t* bits, int n) {
    Prbs p;
    for (int i = 0; i < n; i++) bits[i] ^= (uint8_t)p.next();
}

// ---------------------------------------------------------------- system information

int crc6(const uint8_t* bits, int n) {
    int r[6] = {1, 1, 1, 1, 1, 1};     // r[0] is the stage that gives b47, r[5] gives b42
    for (int i = 0; i < n; i++) {
        const int f = (bits[i] & 1) ^ r[5];
        r[5] = r[4] ^ f; r[4] = r[3]; r[3] = r[2] ^ f; r[2] = r[1] ^ f; r[1] = r[0] ^ f; r[0] = f;
    }
    return (r[5] << 5) | (r[4] << 4) | (r[3] << 3) | (r[2] << 2) | (r[1] << 1) | r[0];
}

namespace {
void putBits(uint8_t* b, int pos, int n, int v) { for (int i = 0; i < n; i++) b[pos + i] = (uint8_t)((v >> (n - 1 - i)) & 1); }
int getBits(const uint8_t* b, int pos, int n) { int v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[pos + i] & 1); return v; }
}

void siToBits(const SysInfo& si, uint8_t* b) {
    std::memset(b, 0, kSiBits);
    b[0] = si.multiFreq ? 0 : 1;
    putBits(b, 1, 9, si.multiFreq ? si.nextFreq : 511);
    putBits(b, 10, 3, si.nominal);
    putBits(b, 13, 6, si.spec);
    putBits(b, 19, 2, si.frame);
    putBits(b, 21, 2, si.subframe);
    putBits(b, 23, 2, si.alloc);
    putBits(b, 25, 2, si.sdiMod);
    putBits(b, 27, 2, si.msdMod);
    putBits(b, 29, 2, si.hier);
    b[31] = si.uniform ? 1 : 0;
    putBits(b, 32, 2, si.rateHi);
    putBits(b, 34, 2, si.hier ? si.rateLo : 0);
    putBits(b, 42, 6, crc6(b, 42));
}

bool siFromBits(const uint8_t* b, SysInfo& si) {
    if (crc6(b, 42) != getBits(b, 42, 6)) return false;
    si.multiFreq = b[0] == 0;
    si.nextFreq = getBits(b, 1, 9);
    si.nominal = getBits(b, 10, 3);
    si.spec = getBits(b, 13, 6);
    si.frame = getBits(b, 19, 2);
    si.subframe = getBits(b, 21, 2);
    si.alloc = getBits(b, 23, 2);
    si.sdiMod = getBits(b, 25, 2);
    si.msdMod = getBits(b, 27, 2);
    si.hier = getBits(b, 29, 2);
    si.uniform = b[31] != 0;
    si.rateHi = getBits(b, 32, 2);
    si.rateLo = getBits(b, 34, 2);
    return true;
}

void siSymbols(const SysInfo& si, cf32* out) {
    uint8_t bits[kSiBits], code[kSiCoded], v[kSiCoded];
    siToBits(si, bits);
    convEncode(bits, kSiBits, code);
    const std::vector<int>& R = interleaver(kSiCoded);
    for (int n = 0; n < kSiCoded; n++) v[n] = code[R[(size_t)n]];
    for (int i = 0; i < kSiSymbols; i++) out[i] = mapBits(v + 2 * i, kQpsk, (float)std::sqrt(2.0));
}

// ---------------------------------------------------------------- convolutional code

void convEncode(const uint8_t* a, int n, uint8_t* x) {
    // 5.2.1: x0 = a_i + a_i-2 + a_i-3 + a_i-5 + a_i-6 (133), x1 = a_i + a_i-1 + a_i-2 + a_i-3 + a_i-6 (171),
    //        x2 = a_i + a_i-1 + a_i-4 + a_i-6 (145), x3 = x0 (133); a_i = 0 outside 0 .. n-1; u_4i+k = x_k,i
    auto at = [&](int i) -> int { return i >= 0 && i < n ? (a[i] & 1) : 0; };
    for (int i = 0; i < n + 6; i++) {
        const int x0 = at(i) ^ at(i - 2) ^ at(i - 3) ^ at(i - 5) ^ at(i - 6);
        const int x1 = at(i) ^ at(i - 1) ^ at(i - 2) ^ at(i - 3) ^ at(i - 6);
        const int x2 = at(i) ^ at(i - 1) ^ at(i - 4) ^ at(i - 6);
        x[4 * i] = (uint8_t)x0; x[4 * i + 1] = (uint8_t)x1; x[4 * i + 2] = (uint8_t)x2; x[4 * i + 3] = (uint8_t)x0;
    }
}

void convDecode(const float* llr, int n, uint8_t* bits) {
    // the DRM mother code (133 171 145 133 171 145) starts with the same four generators: the last two outputs are left erased
    std::vector<float> soft((size_t)(n + 6) * 6, 0.f);
    for (int i = 0; i < n + 6; i++)
        for (int k = 0; k < 4; k++) soft[(size_t)i * 6 + (size_t)k] = llr[4 * i + k];
    drm::viterbiDecode(soft.data(), n, bits);
}

// ---------------------------------------------------------------- GY/T 268.2 Annex C

uint8_t crc8(const uint8_t* d, size_t n) {
    uint8_t r = 0xFF;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const int in = (d[i] >> b) & 1, top = r >> 7;
            r = (uint8_t)(r << 1);
            if (in ^ top) r ^= 0x31;           // x^8 + x^5 + x^4 + 1
        }
    return (uint8_t)~r;
}

uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t r = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const uint32_t in = (d[i] >> b) & 1u, top = r >> 31;
            r <<= 1;
            if (in ^ top) r ^= 0x04C11DB7u;
        }
    return ~r;
}

} // namespace dect2::cdr
