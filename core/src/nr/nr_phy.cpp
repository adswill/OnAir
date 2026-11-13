// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells: SSB sequences, the PBCH chain, the MIB, bands and the sync raster, and the test signal (nr_phy.h).
#include "dect2/nr/nr_phy.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace dect2::nr {

// ---------------------------------------------------------------------------------------------------------------- sequences
void pss(int nid2, float d[127]) {
    int x[127 + 7] = {0, 1, 1, 0, 1, 1, 1};
    for (int i = 0; i < 127; i++) x[i + 7] = (x[i + 4] + x[i]) & 1;
    for (int n = 0; n < 127; n++) d[n] = 1.0f - 2.0f * (float)x[(n + 43 * nid2) % 127];
}

void sss(int nid1, int nid2, float d[127]) {
    int x0[127 + 7] = {1, 0, 0, 0, 0, 0, 0}, x1[127 + 7] = {1, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 127; i++) { x0[i + 7] = (x0[i + 4] + x0[i]) & 1; x1[i + 7] = (x1[i + 1] + x1[i]) & 1; }
    const int m0 = 15 * (nid1 / 112) + 5 * nid2, m1 = nid1 % 112;
    for (int n = 0; n < 127; n++) d[n] = (1.0f - 2.0f * (float)x0[(n + m0) % 127]) * (1.0f - 2.0f * (float)x1[(n + m1) % 127]);
}

void pbchDmrs(int pci, int issb, cf32 r[144]) {
    const uint32_t cinit = (uint32_t)((1u << 11) * (uint32_t)(issb + 1) * (uint32_t)(pci / 4 + 1) + (1u << 6) * (uint32_t)(issb + 1) + (uint32_t)(pci % 4));
    uint8_t c[288];
    cell::gold(cinit, c, 288);
    const float s = (float)M_SQRT1_2;
    for (int m = 0; m < 144; m++) r[m] = cf32(s * (1 - 2 * c[2 * m]), s * (1 - 2 * c[2 * m + 1]));
}

static bool inPbch(int l, int k) {
    if (l == 1 || l == 3) return true;
    if (l == 2) return k < 48 || k >= 192;
    return false;
}

std::vector<Re> pbchDataRes(int pci) {
    std::vector<Re> v;
    v.reserve(432);
    const int nu = pci % 4;
    for (int l = 1; l <= 3; l++)
        for (int k = 0; k < 240; k++) if (inPbch(l, k) && k % 4 != nu) v.push_back({l, k});
    return v;
}

std::vector<Re> pbchDmrsRes(int pci) {
    std::vector<Re> v;
    v.reserve(144);
    const int nu = pci % 4;
    for (int l = 1; l <= 3; l++)
        for (int k = 0; k < 240; k++) if (inPbch(l, k) && k % 4 == nu) v.push_back({l, k});
    return v;
}

// ---------------------------------------------------------------------------------------------------------------- MIB and the PBCH payload
Bits mibBits(const Mib& m) {
    Bits b;
    auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); };
    put(0, 1);                                // BCCH-BCH-MessageType: mib
    put((uint32_t)(m.sfn >> 4) & 63, 6);      // systemFrameNumber (6 MSBs)
    put(m.scs30 ? 1 : 0, 1);
    put((uint32_t)m.kSsb & 15, 4);
    put(m.dmrsTypeAPos == 3 ? 1 : 0, 1);
    put((uint32_t)m.coreset0 & 15, 4);
    put((uint32_t)m.ss0 & 15, 4);
    put(m.barred ? 0 : 1, 1);                 // cellBarred: barred (0), notBarred (1)
    put(m.intraFreqResel ? 0 : 1, 1);         // intraFreqReselection: allowed (0), notAllowed (1)
    put(0, 1);                                // spare
    return b;
}

bool mibParse(const Bits& a, Mib& m) {
    if (a.size() < 24 || a[0] != 0) return false;   // messageClassExtension is not a MIB
    size_t p = 1;
    auto get = [&](int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | a[p++]; return v; };
    const int sfnHi = (int)get(6);
    m.sfn = (sfnHi << 4) | (m.sfn & 15);
    m.scs30 = get(1) != 0;
    m.kSsb = (m.kSsb & 16) | (int)get(4);
    m.dmrsTypeAPos = get(1) ? 3 : 2;
    m.coreset0 = (int)get(4);
    m.ss0 = (int)get(4);
    m.barred = get(1) == 0;
    m.intraFreqResel = get(1) == 0;
    return true;
}

namespace {

// 38.212 Table 7.1.1-1
const int kG[32] = {16, 23, 18, 17, 8, 30, 10, 6, 24, 7, 0, 5, 3, 2, 1, 4, 9, 11, 12, 13, 14, 15, 19, 20, 21, 22, 25, 26, 27, 28, 29, 31};

// Where bit i of the 32-bit payload goes (7.1.1): SFN bits to G(0..9), the half frame bit to G(10), bits A+5..A+7 to G(11..13), the rest
// to G(14..)
const std::array<int, 32>& payloadPos() {
    static const std::array<int, 32> P = [] {
        std::array<int, 32> p{};
        int jSfn = 0, jHrf = 10, jSsb = 11, jOther = 14;
        for (int i = 0; i < 32; i++) {
            if ((i >= 1 && i <= 6) || (i >= 24 && i <= 27)) p[(size_t)i] = kG[jSfn++];
            else if (i == 28) p[(size_t)i] = kG[jHrf];
            else if (i >= 29) p[(size_t)i] = kG[jSsb++];
            else p[(size_t)i] = kG[jOther++];
        }
        return p;
    }();
    return P;
}

// 7.1.2 scrambling of the interleaved payload (Lmax 4 / 8: the half frame bit and the 2nd and 3rd LSB of the SFN are not scrambled)
void payloadScramble(uint8_t* a, int pci, int v) {
    const auto& P = payloadPos();
    const int M = 32 - 3;
    uint8_t c[29 * 4];
    cell::gold((uint32_t)pci, c, sizeof c);
    bool keep[32] = {};
    keep[P[25]] = keep[P[26]] = keep[P[28]] = true;   // SFN bit 2 (3rd LSB), bit 1 (2nd LSB), half frame
    int j = 0;
    for (int i = 0; i < 32; i++) if (!keep[i]) a[i] ^= c[j++ + v * M];
}

} // namespace

Bits pbchPayload(const Mib& m, int pci) {
    Bits a = mibBits(m);
    a.resize(32, 0);
    for (int b = 0; b < 4; b++) a[(size_t)(24 + b)] = (uint8_t)((m.sfn >> (3 - b)) & 1);   // 4th, 3rd, 2nd, 1st LSB
    a[28] = m.hrf ? 1 : 0;
    a[29] = (uint8_t)((m.kSsb >> 4) & 1);    // FR1, Lmax 4 / 8: the MSB of k_SSB, then two reserved bits
    a[30] = a[31] = 0;
    const auto& P = payloadPos();
    Bits ap(32, 0);
    for (int i = 0; i < 32; i++) ap[(size_t)P[(size_t)i]] = a[(size_t)i];
    const int v = (((m.sfn >> 2) & 1) << 1) | ((m.sfn >> 1) & 1);
    payloadScramble(ap.data(), pci, v);
    cell::appendCrc(ap, cell::kCrc24C, 24);
    return ap;
}

bool pbchUnpack(const Bits& c, int pci, int lmax, Mib& m) {
    if (c.size() < 56) return false;
    const auto& P = payloadPos();
    Bits ap(c.begin(), c.begin() + 32);
    const int b2 = ap[(size_t)P[25]], b1 = ap[(size_t)P[26]];   // not scrambled
    payloadScramble(ap.data(), pci, (b2 << 1) | b1);
    Bits a(32);
    for (int i = 0; i < 32; i++) a[(size_t)i] = ap[(size_t)P[(size_t)i]];
    Mib r;
    r.sfn = 0;
    for (int b = 0; b < 4; b++) r.sfn |= a[(size_t)(24 + b)] << (3 - b);
    r.kSsb = a[29] << 4;
    if (!mibParse(a, r)) return false;
    r.hrf = a[28] != 0;
    r.lmax = lmax;
    m = r;
    return true;
}

static const PolarCode& pbchCode() { static const PolarCode pc = polarCode(56, 864, 9, true); return pc; }

Bits pbchEncode(const Mib& m, int pci) {
    const Bits c = pbchPayload(m, pci);
    Bits e = polarRateMatch(pbchCode(), polarEncode(pbchCode(), c));
    const int nu = m.lmax == 4 ? (m.ssbIdx & 3) : (m.ssbIdx & 7);
    std::vector<uint8_t> s((size_t)864 * 8);
    cell::gold((uint32_t)pci, s.data(), s.size());
    for (int i = 0; i < 864; i++) e[(size_t)i] ^= s[(size_t)(i + nu * 864)];
    return e;
}

static bool crcOk(const Bits& b, void*) { return cell::checkCrc(b, cell::kCrc24C, 24); }

bool pbchDecode(const float* llr, int pci, int issb, Mib& m, int L) {
    static thread_local std::vector<uint8_t> s;
    static thread_local int sPci = -1;
    if (sPci != pci) { s.resize((size_t)864 * 8); cell::gold((uint32_t)pci, s.data(), s.size()); sPci = pci; }
    // Lmax 8: nu = issb; Lmax 4: nu = issb & 3, issb = i_SSB + 4 n_hf
    for (int lmax : {8, 4}) {
        if (lmax == 4 && issb < 4) break;   // the same bits as Lmax 8: told apart by the half frame bit below
        const int nu = lmax == 4 ? (issb & 3) : issb;
        std::vector<float> e(864);
        for (int i = 0; i < 864; i++) e[(size_t)i] = s[(size_t)(i + nu * 864)] ? -llr[i] : llr[i];
        const std::vector<float> d = polarRateRecover(pbchCode(), e.data());
        Bits c;
        if (!polarDecode(pbchCode(), d, L, c, crcOk, nullptr)) continue;
        Mib r;
        if (!pbchUnpack(c, pci, lmax, r)) continue;
        if (lmax == 4) { if (!r.hrf) continue; r.ssbIdx = issb - 4; }
        else r.ssbIdx = issb;   // issb < 4: Lmax 4 and 8 give the same bits (the caller knows the band's Lmax)
        m = r;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------- bands and the sync raster
const std::vector<NrBand>& nrBands() {
    // 38.104 Table 5.2-1 (downlink) and 38.101-1 Table 5.4.3.3-1 (GSCN range per SSB subcarrier spacing); the order is the preference
    // when a GSCN lies in several bands
    static const std::vector<NrBand> t = {
        {78, 3300, 3800, 30, 7711, 8051, 1, true, "3.5 GHz TDD"},   {77, 3300, 4200, 30, 7711, 8329, 1, true, "C-band TDD"},
        {28, 758, 803, 15, 1901, 2002, 1, false, "APT 700"},        {20, 791, 821, 15, 1982, 2047, 1, false, "EU 800 DD"},
        {8, 925, 960, 15, 2318, 2395, 1, false, "E-GSM 900"},       {3, 1805, 1880, 15, 4517, 4693, 1, false, "DCS 1800"},
        {1, 2110, 2170, 15, 5279, 5419, 1, false, "IMT 2100"},      {7, 2620, 2690, 15, 6554, 6718, 1, false, "IMT-E 2600"},
        {38, 2570, 2620, 15, 6431, 6544, 1, true, "TDD 2600"},      {40, 2300, 2400, 30, 5762, 5989, 1, true, "TDD 2300"},
        {41, 2496, 2690, 30, 6252, 6714, 3, true, "TDD 2500"},      {41, 2496, 2690, 15, 6246, 6717, 3, true, "TDD 2500"},
        {79, 4400, 5000, 30, 8480, 8880, 16, true, "4.7 GHz TDD"}, {48, 3550, 3700, 30, 7884, 7982, 1, true, "CBRS"},
        {5, 869, 894, 15, 2177, 2230, 1, false, "CLR 850"},         {5, 869, 894, 30, 2183, 2224, 1, false, "CLR 850"},
        {2, 1930, 1990, 15, 4829, 4969, 1, false, "PCS 1900"},      {25, 1930, 1995, 15, 4829, 4981, 1, false, "E-PCS 1900"},
        {12, 729, 746, 15, 1828, 1858, 1, false, "Lower 700"},      {71, 617, 652, 15, 1547, 1624, 1, false, "600 MHz"},
        {66, 2110, 2200, 15, 5279, 5494, 1, false, "AWS-3"},        {66, 2110, 2200, 30, 5285, 5488, 1, false, "AWS-3"},
        {34, 2010, 2025, 15, 5030, 5056, 1, true, "TDD 2000"},      {34, 2010, 2025, 30, 5036, 5050, 1, true, "TDD 2000"},
        {39, 1880, 1920, 15, 4706, 4795, 1, true, "TDD 1900"},      {50, 1432, 1517, 15, 3584, 3787, 1, true, "TDD 1500"},
    };
    return t;
}

const NrBand* nrBandInfo(int band) {
    for (const auto& b : nrBands()) if (b.band == band) return &b;
    return nullptr;
}

double gscnToHz(int g) {
    if (g >= 2 && g <= 7498) {
        const int N = (g + 1) / 3, M = (g - 3 * N) * 2 + 3;   // GSCN = 3N + (M - 3) / 2, M 1, 3, 5
        return N * 1.2e6 + M * 50e3;
    }
    if (g >= 7499 && g <= 22255) return 3000e6 + (g - 7499) * 1.44e6;
    return 0;
}

int hzToGscn(double hz) {
    if (hz < 3000e6 - 0.5e6) {
        int best = -1; double bd = 1e18;
        const int N = (int)std::lround(hz / 1.2e6);
        for (int n = N - 1; n <= N + 1; n++)
            for (int M : {1, 3, 5}) {
                if (n < 1) continue;
                const double f = n * 1.2e6 + M * 50e3, d = std::fabs(f - hz);
                if (d < bd) { bd = d; best = 3 * n + (M - 3) / 2; }
            }
        return best;
    }
    const long N = std::lround((hz - 3000e6) / 1.44e6);
    if (N < 0 || N > 14756) return -1;
    return 7499 + (int)N;
}

int nrBandOfGscn(int g, int scs) {
    for (const auto& b : nrBands())
        if (g >= b.gscnLo && g <= b.gscnHi && (g - b.gscnLo) % b.gscnStep == 0 && (scs == 0 || scs == b.scsKhz)) return b.band;
    return 0;
}

int lmaxFor(int band, double hz) {
    const NrBand* b = nrBandInfo(band);
    if (!b) return hz > 3e9 ? 8 : 4;
    if (b->tdd) return hz > 2.4e9 ? 8 : 4;   // unpaired spectrum: Lmax 8 above 2.4 GHz (Rel-15 Case A / C)
    return hz > 3e9 ? 8 : 4;
}

bool coreset0Info(int ssbScs, int pdcchScs, int idx, int& rbs, int& syms, int& off) {
    struct R { int8_t rbs, syms, off; };
    // 38.213 Table 13-1 (SSB 15, PDCCH 15), 13-2 (15, 30), 13-3 (30, 15), 13-4 (30, 30); rbs 0 = reserved
    static const R t1[16] = {{24, 2, 0}, {24, 2, 2}, {24, 2, 4}, {24, 3, 0}, {24, 3, 2}, {24, 3, 4}, {48, 1, 12}, {48, 1, 16}, {48, 2, 12}, {48, 2, 16}, {48, 3, 12}, {48, 3, 16}, {96, 1, 38}, {96, 2, 38}, {96, 3, 38}, {0, 0, 0}};
    static const R t2[16] = {{24, 2, 5}, {24, 2, 6}, {24, 2, 7}, {24, 2, 8}, {24, 3, 5}, {24, 3, 6}, {24, 3, 7}, {24, 3, 8}, {48, 1, 18}, {48, 1, 20}, {48, 2, 18}, {48, 2, 20}, {48, 3, 18}, {48, 3, 20}, {0, 0, 0}, {0, 0, 0}};
    static const R t3[16] = {{48, 1, 2}, {48, 1, 6}, {48, 2, 2}, {48, 2, 6}, {48, 3, 2}, {48, 3, 6}, {96, 1, 28}, {96, 2, 28}, {96, 3, 28}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    static const R t4[16] = {{24, 2, 0}, {24, 2, 1}, {24, 2, 2}, {24, 2, 3}, {24, 2, 4}, {24, 3, 0}, {24, 3, 1}, {24, 3, 2}, {24, 3, 3}, {24, 3, 4}, {48, 1, 12}, {48, 1, 14}, {48, 1, 16}, {48, 2, 12}, {48, 2, 14}, {48, 2, 16}};
    if (idx < 0 || idx > 15) return false;
    const R* t = ssbScs == 15 ? (pdcchScs == 15 ? t1 : t2) : (pdcchScs == 15 ? t3 : t4);
    if (!t[idx].rbs) return false;
    rbs = t[idx].rbs; syms = t[idx].syms; off = t[idx].off;
    return true;
}

int ssbFirstSymbol(int i, int lmax) {
    if (i < 0 || i >= lmax) return -1;
    return (i & 1 ? 8 : 2) + 14 * (i >> 1);   // Case A and Case C: {2, 8} + 14 n
}

// ---------------------------------------------------------------------------------------------------------------- timing
int Numerology::cpLen(int l) const {
    const int mu = scsKhz / 30;   // 0 for 15 kHz, 1 for 30 kHz
    const int base = nfft * 144 / 2048, extra = nfft * 16 / 2048 * (1 << mu);
    const int perHalfMs = 7 << mu;   // symbols per 0.5 ms
    return base + (l % perHalfMs == 0 ? extra : 0);
}

int Numerology::symStart(int l) const {
    const int perHalfMs = 7 << (scsKhz / 30);
    const int blk = l / perHalfMs, r = l % perHalfMs;
    const int halfMs = (int)(rate() * 0.0005 + 0.5);
    int s = blk * halfMs;
    for (int j = 0; j < r; j++) s += nfft + cpLen(j);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- test signal
std::vector<cf32> generate(const GenCell& g, int nHalf) {
    Numerology nm; nm.scsKhz = g.scsKhz; nm.nfft = 256;
    const int H = nm.halfLen(), N = nm.nfft;
    std::vector<cf32> out((size_t)H * (size_t)nHalf, cf32(0, 0));
    std::mt19937 rng(g.seed);
    Fft f(N);
    float dP[127], dS[127];
    pss(g.pci % 3, dP);
    sss(g.pci / 3, g.pci % 3, dS);
    const std::vector<Re> dataRe = pbchDataRes(g.pci), dmrsRe = pbchDmrsRes(g.pci);
    const float qp = (float)M_SQRT1_2;
    const float scale = 0.2f / std::sqrt(240.0f);   // rms about 0.2 (the inverse FFT is not normalised)
    std::vector<cf32> grid((size_t)N);
    auto qpsk = [&]() { return cf32(rng() & 1 ? -qp : qp, rng() & 1 ? -qp : qp); };
    auto putSym = [&](int half, int l) {
        f.inverse(grid.data());
        const int cp = nm.cpLen(l), st = nm.symStart(l);
        cf32* o = &out[(size_t)half * (size_t)H + (size_t)st];
        for (int i = 0; i < cp; i++) o[i] = grid[(size_t)(N - cp + i)] * scale;
        for (int i = 0; i < N; i++) o[cp + i] = grid[(size_t)i] * scale;
    };
    auto bin = [&](int k) { return (size_t)((k - 120 + N) % N); };
    const int perPeriod = std::max(1, g.periodMs / 5);
    const int sfnStart = g.mib.sfn;
    for (int h = 0; h < nHalf; h++) {
        const int hf = (h + (g.mib.hrf ? 1 : 0));
        const int sfn = (sfnStart + hf / 2) & 1023;
        const bool hrf = (hf & 1) != 0;
        std::vector<int> ssbAt(nm.symsPerHalf(), -1);   // symbol -> SSB index starting there
        if (hf % perPeriod == 0)
            for (int i = 0; i < g.lmax; i++) if (g.ssbMask >> i & 1) ssbAt[(size_t)ssbFirstSymbol(i, g.lmax)] = i;
        for (int l = 0; l < nm.symsPerHalf(); l++) {
            if (ssbAt[(size_t)l] < 0) {
                if (!g.load) continue;
                bool inSsb = false;
                for (int d = 1; d < 4; d++) if (l - d >= 0 && ssbAt[(size_t)(l - d)] >= 0) inSsb = true;
                if (inSsb) continue;
                std::fill(grid.begin(), grid.end(), cf32(0, 0));
                for (int k = -8; k < 248; k++) grid[bin(k)] = qpsk() * 0.7f;   // other channels of the carrier (within the SSB's band and 8 subcarriers each side)
                putSym(h, l);
                continue;
            }
            const int i = ssbAt[(size_t)l];
            const float gain = (size_t)i < g.beamDb.size() ? std::pow(10.f, g.beamDb[(size_t)i] / 20.f) : 1.f;
            Mib m = g.mib;
            m.sfn = sfn; m.hrf = hrf; m.ssbIdx = i; m.lmax = g.lmax;
            const Bits e = pbchEncode(m, g.pci);
            const int issb = g.lmax == 4 ? (i + 4 * (hrf ? 1 : 0)) : i;
            cf32 r[144];
            pbchDmrs(g.pci, issb, r);
            cf32 sym[4][240];
            for (auto& s : sym) for (auto& v : s) v = 0;
            for (int n = 0; n < 127; n++) { sym[0][56 + n] = dP[n]; sym[2][56 + n] = dS[n]; }
            for (size_t j = 0; j < dataRe.size(); j++) sym[dataRe[j].l][dataRe[j].k] = cf32(qp * (1 - 2 * e[2 * j]), qp * (1 - 2 * e[2 * j + 1]));
            for (size_t j = 0; j < dmrsRe.size(); j++) sym[dmrsRe[j].l][dmrsRe[j].k] = r[j];
            for (int s = 0; s < 4; s++) {
                std::fill(grid.begin(), grid.end(), cf32(0, 0));
                if (g.load) for (int k = -8; k < 248; k++) if (k < 0 || k >= 240) grid[bin(k)] = qpsk() * 0.7f;   // the carrier around the SSB
                for (int k = 0; k < 240; k++) if (sym[s][k] != cf32(0, 0)) grid[bin(k)] = sym[s][k] * gain;
                putSym(h, l + s);
            }
            l += 3;
        }
    }
    return out;
}

} // namespace dect2::nr
