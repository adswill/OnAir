// DRM known answers: CRC and energy dispersal vectors, cell counts and code sizes against the tables of ETSI ES 201 980 (V4.3.1), the pilot rules,
// the message formats, and decoding of real over-the-air cells (FAC of modes A, B and C, SDC and MSC of mode A).
#include "dect2/drm_defs.h"
#include "dect2/drm_fec.h"
#include "dect2/drm_msg.h"
#include "data/drm/real_cells.h"
#include "../core/src/drm_internal.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::drm;
using dect2::drm::kAfsRefs;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> unb64(const char* s) {
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    uint32_t acc = 0; int n = 0;
    for (const char* p = s; *p; p++) {
        const char* q = strchr(tab, *p);
        if (!q || !*p) continue;
        acc = (acc << 6) | (uint32_t)(q - tab); n += 6;
        if (n >= 8) { n -= 8; out.push_back((uint8_t)(acc >> n)); }
    }
    return out;
}
static std::vector<cf32> cellsOf(const char* s) {
    const auto b = unb64(s);
    std::vector<cf32> c(b.size() / 2);
    for (size_t i = 0; i < c.size(); i++) c[i] = cf32((float)(int8_t)b[2 * i] / 64.f, (float)(int8_t)b[2 * i + 1] / 64.f);
    return c;
}
static std::vector<uint8_t> hexBytes(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) { unsigned x; sscanf(std::string(s, 2).c_str(), "%2x", &x); v.push_back((uint8_t)x); }
    return v;
}

// ---- clause 7.2.2 and Annex D
static void testCrcPrbs() {
    // CRC catalogue values (check value of "123456789"): CRC-16/GENIBUS (poly 0x1021, init FFFF, output inverted) = 0xD64E, CRC-8/SAE-J1850 (poly 0x1D, init FF, output inverted) = 0x4B
    const uint8_t* s = (const uint8_t*)"123456789";
    CHECK(crc16Bytes(s, 9) == 0xD64E, "CRC-16 of 123456789 is %04X", crc16Bytes(s, 9));
    CHECK(crc8Bytes(s, 9) == 0x4B, "CRC-8 of 123456789 is %02X", crc8Bytes(s, 9));
    // Table 26: first 16 bits of the PRBS
    uint8_t p[16];
    prbs(p, 16);
    const uint8_t t26[16] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
    CHECK(memcmp(p, t26, 16) == 0, "PRBS differs from Table 26");
    // the sequence has the period of a maximal length register of degree 9
    std::vector<uint8_t> q(1100);
    prbs(q.data(), q.size());
    bool periodic = true;
    for (int i = 0; i < 511; i++) if (q[(size_t)i] != q[(size_t)i + 511]) periodic = false;
    CHECK(periodic, "PRBS period is not 511");
    int ones = 0;
    for (int i = 0; i < 511; i++) ones += q[(size_t)i];
    CHECK(ones == 256, "PRBS has %d ones in a period (expected 256)", ones);
}

// ---- cell counts: Tables 25 and 41 to 45, FAC and SDC data field lengths: Table 21
struct Counts { int mode, occ, sdc, sfa, sfu, mux; };
static void testLayout() {
    const Counts c[] = {
        {0, 0, 167, 3778, 3777, 1259}, {0, 1, 190, 4268, 4266, 1422}, {0, 2, 359, 7897, 7896, 2632}, {0, 3, 405, 8877, 8877, 2959}, {0, 4, 754, 16394, 16392, 5464}, {0, 5, 846, 18354, 18354, 6118},
        {1, 0, 130, 2900, 2898, 966}, {1, 1, 150, 3330, 3330, 1110}, {1, 2, 282, 6153, 6153, 2051}, {1, 3, 322, 7013, 7011, 2337}, {1, 4, 588, 12747, 12747, 4249}, {1, 5, 662, 14323, 14322, 4774},
        {2, 3, 288, 5532, 5532, 1844}, {2, 5, 607, 11603, 11601, 3867},
        {3, 3, 152, 3679, 3678, 1226}, {3, 5, 332, 7819, 7818, 2606},
        {4, 0, 936, 29842, 29840, 7460},
    };
    for (const Counts& k : c) {
        auto L = layout(k.mode, k.occ);
        CHECK(L != nullptr, "no layout for mode %d occupancy %d", k.mode, k.occ);
        if (!L) continue;
        CHECK(L->nSdc == k.sdc, "mode %c occ %d: %d SDC cells (Table 25: %d)", "ABCDE"[k.mode], k.occ, L->nSdc, k.sdc);
        CHECK(L->nSfa == k.sfa, "mode %c occ %d: %d MSC cells per super frame (Tables 41-45: %d)", "ABCDE"[k.mode], k.occ, L->nSfa, k.sfa);
        CHECK(L->nMux * L->frames == k.sfu, "mode %c occ %d: %d useful cells (Tables 41-45: %d)", "ABCDE"[k.mode], k.occ, L->nMux * L->frames, k.sfu);
        CHECK(L->nMux == k.mux, "mode %c occ %d: N_MUX %d (expected %d)", "ABCDE"[k.mode], k.occ, L->nMux, k.mux);
        CHECK(L->nFac == (k.mode == 4 ? 244 : 65), "mode %c: %d FAC cells", "ABCDE"[k.mode], L->nFac);
        // no two kinds of cell share a position: all FAC cells of all frames really are FAC cells
        int nfac = 0, nsdc = 0, nmsc = 0;
        for (uint8_t t : L->type) { nfac += t == kCellFac; nsdc += t == kCellSdc; nmsc += t == kCellMsc; }
        CHECK(nfac == L->nFac * L->frames, "mode %c occ %d: FAC cells overlap pilots (%d of %d)", "ABCDE"[k.mode], k.occ, nfac, L->nFac * L->frames);
        CHECK(nsdc == L->nSdc && nmsc == L->nSfa, "cell type grid disagrees with the cell lists");
    }
    // Table 21: data field length of the SDC (bytes) for SDC mode 0 and 1
    struct T21 { int mode, occ, b0, b1; };
    const T21 t21[] = {{0, 0, 37, 17}, {0, 1, 43, 20}, {0, 2, 85, 41}, {0, 3, 97, 47}, {0, 4, 184, 91}, {0, 5, 207, 102}, {1, 0, 28, 13}, {1, 1, 33, 15}, {1, 2, 66, 32}, {1, 3, 76, 37},
                       {1, 4, 143, 70}, {1, 5, 161, 79}, {2, 3, 68, 32}, {2, 5, 147, 72}, {3, 3, 33, 15}, {3, 5, 78, 38}, {4, 0, 113, 55}};
    for (const T21& t : t21) {
        CHECK(sdcDataBytes(t.mode, t.occ, 0) == t.b0, "Table 21 mode %c occ %d SDC mode 0: %d (expected %d)", "ABCDE"[t.mode], t.occ, sdcDataBytes(t.mode, t.occ, 0), t.b0);
        CHECK(sdcDataBytes(t.mode, t.occ, 1) == t.b1, "Table 21 mode %c occ %d SDC mode 1: %d (expected %d)", "ABCDE"[t.mode], t.occ, sdcDataBytes(t.mode, t.occ, 1), t.b1);
    }
    // symbol timing: the symbol, useful and guard durations of Table 2 and the frame length of 400 ms (mode E 100 ms)
    for (int m = 0; m < 5; m++) {
        const ModeParams& p = modeParams(m);
        CHECK(std::fabs(p.frameMs() - (m == 4 ? 100.0 : 400.0)) < 1e-6, "mode %c frame length %.3f ms", p.name, p.frameMs());
    }
}

// ---- pilots: rules of clause 8.4
static void testPilots() {
    // frequency references are continuous tones: the phase step per symbol 2 pi k Ts/Tu is a multiple of 2 pi, or of pi for the two carriers of mode D (flipped every symbol)
    for (int m = 0; m < 4; m++) {
        const ModeParams& p = modeParams(m);
        auto L = layout(m, m == 2 || m == 3 ? 3 : 2);
        for (const Pilot& pl : L->pilots[2]) {
            if (pl.kind != kCellFreqRef) continue;
            cf32 r0, r1;
            const bool a = pilotRef(m, 3, 2, pl.k, r0), b = pilotRef(m, 3, 3, pl.k, r1);
            CHECK(a && b, "frequency reference missing");
            const double steps = (double)pl.k * (p.tu12 + p.tg12) / p.tu12;      // phase advance of the carrier from one symbol to the next, in cycles
            const double frac = steps - std::floor(steps);
            const bool flips = std::fabs(frac - 0.5) < 1e-9;
            CHECK(flips || frac < 1e-9, "mode %c carrier %d: Ts/Tu * k = %.3f is neither integer nor half integer", p.name, pl.k, steps);
            const bool flipped = std::abs(r0 + r1) < 1e-4;
            CHECK(flips == flipped, "mode %c carrier %d: tone continuity and the sign flip of the reference disagree", p.name, pl.k);
        }
    }
    // the boosted carriers of Table 59 are gain reference positions
    for (int m = 0; m < 5; m++)
        for (int occ = 0; occ < 6; occ++) {
            auto L = layout(m, occ);
            if (!L) continue;
            int boosted = 0;
            for (int s = 0; s < L->ns; s++)
                for (const Pilot& p : L->pilots[(size_t)s])
                    if (p.kind == kCellGainRef && std::fabs(std::abs(p.ref) - 2.0) < 1e-4) boosted++;
            // every one of the four carriers is a gain reference in at least one symbol, and only those are boosted
            std::set<int> seen;
            for (int s = 0; s < L->ns; s++)
                for (const Pilot& p : L->pilots[(size_t)s])
                    if (p.kind == kCellGainRef && std::fabs(std::abs(p.ref) - 2.0) < 1e-4) seen.insert(p.k);
            CHECK(seen.size() == 4, "mode %c occ %d: %zu boosted carriers (Table 59 lists 4)", "ABCDE"[m], occ, seen.size());
            (void)boosted;
        }
    // AFS cells that are also gain references agree in phase (the note under Table 61): the table of AFS phases and the gain reference formula are independent data
    {
        int agree = 0, mismatch = 0;
        for (int i = 0; i < 54; i++)
            for (int col = 1; col <= 2; col++) {
                const int s = col == 1 ? 4 : 39, k = kAfsRefs[i][0];
                cf32 r;
                int kd = 0;
                if (!pilotRef(kModeE, 0, s, k, r, &kd) || kd != kCellGainRef) continue;
                const double ph = 2 * M_PI * kAfsRefs[i][col] / 1024.0;
                const cf32 want((float)std::cos(ph), (float)std::sin(ph));
                if (std::abs(r / std::abs(r) - want) < 1e-3) agree++; else mismatch++;
            }
        CHECK(mismatch == 0 && agree >= 20, "mode E AFS cells on gain reference positions: %d agree, %d differ", agree, mismatch);
        printf("mode E: %d AFS cells coincide with gain references, phases agree\n", agree);
    }
}

// ---- puncturing tables 27 and 28
static void testPuncturing() {
    const Rate rates[] = {{1, 6}, {1, 4}, {3, 10}, {1, 3}, {4, 11}, {2, 5}, {1, 2}, {4, 7}, {3, 5}, {2, 3}, {8, 11}, {3, 4}, {4, 5}, {7, 8}, {8, 9}};
    for (const Rate& r : rates) {
        // one level of N cells at this rate in part B: the information bits plus the tail fill 2N coded bits exactly
        MlcParams p;
        p.levels = 1; p.n2 = 600;
        p.rxB[0] = r.rx; p.ryB[0] = r.ry;
        MlcCode c(p);
        CHECK(c.infoBits() == r.rx * ((2 * 600 - 12) / r.ry), "rate %d/%d: %d information bits", r.rx, r.ry, c.infoBits());
    }
}

// ---- Annex J: information bits per multiplex frame (EEP) and per SDC block
static void testAnnexJ() {
    struct Row { int mode, qamBits, pl; int occ[6]; };      // 0 = occupancy not defined
    const Row msc[] = {
        {0, 6, 0, {3757, 4248, 7878, 8857, 16374, 18336}}, {0, 6, 1, {4509, 5096, 9450, 10628, 19646, 21998}}, {0, 6, 2, {5322, 6018, 11157, 12547, 23193, 25976}},
        {0, 6, 3, {5898, 6664, 12364, 13908, 25704, 28788}}, {0, 4, 0, {2505, 2832, 5250, 5904, 10914, 12222}}, {0, 4, 1, {3131, 3540, 6565, 7381, 13645, 15280}},
        {1, 6, 0, {2880, 3312, 6133, 6991, 12727, 14304}}, {1, 6, 1, {3456, 3972, 7361, 8390, 15272, 17162}}, {1, 6, 2, {4080, 4692, 8688, 9900, 18026, 20264}},
        {1, 6, 3, {4520, 5196, 9630, 10980, 19980, 22456}}, {1, 4, 0, {1920, 2208, 4089, 4662, 8484, 9534}}, {1, 4, 1, {2400, 2760, 5111, 5826, 10606, 11920}},
        {2, 6, 0, {0, 0, 0, 5514, 0, 11581}}, {2, 6, 1, {0, 0, 0, 6615, 0, 13898}}, {2, 6, 2, {0, 0, 0, 7808, 0, 16406}}, {2, 6, 3, {0, 0, 0, 8654, 0, 18188}},
        {2, 4, 0, {0, 0, 0, 3675, 0, 7722}}, {2, 4, 1, {0, 0, 0, 4595, 0, 9651}},
        {3, 6, 0, {0, 0, 0, 3660, 0, 7800}}, {3, 6, 1, {0, 0, 0, 4391, 0, 9359}}, {3, 6, 2, {0, 0, 0, 5185, 0, 11050}}, {3, 6, 3, {0, 0, 0, 5746, 0, 12242}},
        {3, 4, 0, {0, 0, 0, 2439, 0, 5199}}, {3, 4, 1, {0, 0, 0, 3050, 0, 6500}},
        {4, 4, 0, {9938, 0, 0, 0, 0, 0}}, {4, 4, 1, {12243, 0, 0, 0, 0, 0}}, {4, 4, 2, {14907, 0, 0, 0, 0, 0}}, {4, 4, 3, {18635, 0, 0, 0, 0, 0}},
        {4, 2, 0, {3727, 0, 0, 0, 0, 0}}, {4, 2, 1, {4969, 0, 0, 0, 0, 0}}, {4, 2, 2, {5962, 0, 0, 0, 0, 0}}, {4, 2, 3, {7454, 0, 0, 0, 0, 0}},
    };
    int checked = 0;
    for (const Row& r : msc)
        for (int occ = 0; occ < 6; occ++) {
            if (!r.occ[occ]) continue;
            auto L = layout(r.mode, occ);
            MlcParams p;
            p.levels = r.qamBits / 2;
            p.n2 = L->nMux;
            for (int l = 0; l < p.levels; l++) { const Rate q = mscRate(r.mode, r.qamBits, r.pl, l); p.rxB[l] = q.rx; p.ryB[l] = q.ry; }
            MlcCode c(p);
            CHECK(c.infoBits() == r.occ[occ], "Annex J: mode %c occ %d %d-QAM level %d: L = %d (expected %d)", "ABCDE"[r.mode], occ, 1 << r.qamBits, r.pl, c.infoBits(), r.occ[occ]);
            checked++;
        }
    struct Sdc { int mode, sdcMode; int occ[6]; };
    const Sdc sdc[] = {{0, 0, {321, 366, 705, 798, 1494, 1680}}, {0, 1, {161, 184, 353, 399, 748, 840}}, {1, 0, {246, 288, 552, 630, 1164, 1311}}, {1, 1, {124, 144, 276, 316, 582, 656}},
                       {2, 0, {0, 0, 0, 564, 0, 1200}}, {2, 1, {0, 0, 0, 282, 0, 601}}, {3, 0, {0, 0, 0, 291, 0, 651}}, {3, 1, {0, 0, 0, 146, 0, 326}}, {4, 0, {930, 0, 0, 0, 0, 0}}, {4, 1, {465, 0, 0, 0, 0, 0}}};
    for (const Sdc& r : sdc)
        for (int occ = 0; occ < 6; occ++) {
            if (!r.occ[occ]) continue;
            auto L = layout(r.mode, occ);
            MlcParams p;
            p.n2 = L->nSdc;
            if (r.mode == 4) { p.levels = 1; p.rxB[0] = 1; p.ryB[0] = r.sdcMode == 0 ? 2 : 4; }
            else if (r.sdcMode == 0) { p.levels = 2; p.rxB[0] = 1; p.ryB[0] = 3; p.rxB[1] = 2; p.ryB[1] = 3; }
            else { p.levels = 1; p.rxB[0] = 1; p.ryB[0] = 2; }
            MlcCode c(p);
            CHECK(c.infoBits() == r.occ[occ], "Annex J: SDC mode %c occ %d SDC mode %d: L = %d (expected %d)", "ABCDE"[r.mode], occ, r.sdcMode, c.infoBits(), r.occ[occ]);
            checked++;
        }
    // FAC: 72 bits (modes A to D) and 116 bits (mode E)
    {
        MlcParams p; p.levels = 1; p.n2 = 65; p.rxB[0] = 3; p.ryB[0] = 5; p.fac = true;
        CHECK(MlcCode(p).infoBits() == 72, "FAC information bits %d", MlcCode(p).infoBits());
        MlcParams e; e.levels = 1; e.n2 = 244; e.rxB[0] = 1; e.ryB[0] = 4; e.fac = true;
        CHECK(MlcCode(e).infoBits() == 116, "mode E FAC information bits %d", MlcCode(e).infoBits());
    }
    printf("Annex J: %d code sizes checked\n", checked);
}

// ---- encoder and decoder: noiseless round trips over many configurations, including unequal error protection
static void roundTrip(const MlcParams& p, const char* what, uint32_t seed) {
    MlcCode c(p);
    std::mt19937 rng(seed);
    std::vector<uint8_t> u((size_t)c.infoBits()), v(u.size());
    for (auto& b : u) b = (uint8_t)(rng() & 1);
    std::vector<cf32> cells((size_t)c.cells());
    c.encode(u.data(), cells.data());
    double pw = 0;
    for (auto& z : cells) pw += std::norm(z);
    pw /= (double)cells.size();
    CHECK(pw > 0.9 && pw < 1.1, "%s: mean cell power %.3f", what, pw);
    std::vector<float> w(cells.size(), 100.f);
    c.decode(cells.data(), w.data(), v.data(), 1);
    CHECK(u == v, "%s: noiseless decode differs", what);
}
static void testRoundTrips() {
    auto setRates = [](MlcParams& p, int mode, int qamBits, int plA, int plB) {
        p.levels = qamBits / 2;
        for (int l = 0; l < p.levels; l++) {
            const Rate a = mscRate(mode, qamBits, plA, l), b = mscRate(mode, qamBits, plB, l);
            p.rxA[l] = a.rx; p.ryA[l] = a.ry; p.rxB[l] = b.rx; p.ryB[l] = b.ry;
        }
    };
    struct Cfg { int mode, occ, qam, plA, plB, x; const char* name; };
    const Cfg cfg[] = {
        {0, 3, 6, 1, 1, 0, "A 10 kHz 64-QAM EEP"}, {1, 2, 4, 0, 0, 0, "B 9 kHz 16-QAM EEP"}, {2, 3, 6, 3, 3, 0, "C 10 kHz 64-QAM PL3 EEP"}, {3, 5, 4, 1, 1, 0, "D 20 kHz 16-QAM EEP"},
        {0, 3, 6, 0, 1, 78, "A 10 kHz 64-QAM UEP (Annex C: 78 bytes in part A)"}, {0, 5, 4, 1, 0, 120, "A 20 kHz 16-QAM UEP"}, {1, 3, 6, 2, 3, 200, "B 10 kHz 64-QAM UEP"},
        {4, 0, 2, 1, 1, 0, "E 4-QAM EEP"}, {4, 0, 4, 1, 2, 0, "E 16-QAM EEP"}, {4, 0, 4, 1, 3, 400, "E 16-QAM UEP"},
    };
    for (const Cfg& k : cfg) {
        auto L = layout(k.mode, k.occ);
        MlcParams p;
        setRates(p, k.mode, k.qam, k.plA, k.plB);
        if (k.x == 0) p.n2 = L->nMux;
        else {
            const int lcm = mscRyLcm(k.mode, k.qam, k.plA);
            double sum = 0;
            for (int l = 0; l < p.levels; l++) sum += (double)p.rxA[l] / p.ryA[l];
            p.n1 = lcm * (int)std::ceil(8.0 * k.x / (2.0 * lcm * sum) - 1e-9);
            p.n2 = L->nMux - p.n1;
        }
        roundTrip(p, k.name, 1234 + (uint32_t)k.x);
    }
    // SDC and FAC
    for (int mode = 0; mode < 5; mode++) {
        auto L = layout(mode, mode >= 2 && mode < 4 ? 3 : (mode == 4 ? 0 : 2));
        for (int sm = 0; sm < 2; sm++) {
            MlcParams p;
            p.n2 = L->nSdc;
            if (mode == 4) { p.levels = 1; p.rxB[0] = 1; p.ryB[0] = sm == 0 ? 2 : 4; }
            else if (sm == 0) { p.levels = 2; p.rxB[0] = 1; p.ryB[0] = 3; p.rxB[1] = 2; p.ryB[1] = 3; }
            else { p.levels = 1; p.rxB[0] = 1; p.ryB[0] = 2; }
            roundTrip(p, "SDC", 77 + (uint32_t)mode);
        }
        MlcParams f;
        f.levels = 1; f.n2 = L->nFac; f.fac = true; f.rxB[0] = mode == 4 ? 1 : 3; f.ryB[0] = mode == 4 ? 4 : 5;
        roundTrip(f, "FAC", 9 + (uint32_t)mode);
    }
}

// ---- constellation: points of figures 26, 29 and 30
static void testQam() {
    uint8_t b[6] = {0, 0, 0, 0, 0, 0};
    cf32 z = qamMap(3, b);
    CHECK(std::fabs(z.real() - 7 * qamNorm(3)) < 1e-6 && std::fabs(z.imag() - 7 * qamNorm(3)) < 1e-6, "64-QAM 000 000 is not (7a, 7a)");
    const uint8_t b2[6] = {1, 1, 1, 1, 1, 1};
    z = qamMap(3, b2);
    CHECK(std::fabs(z.real() + 7 * qamNorm(3)) < 1e-6 && std::fabs(z.imag() + 7 * qamNorm(3)) < 1e-6, "64-QAM 111 111 is not (-7a, -7a)");
    const uint8_t b3[6] = {1, 1, 0, 0, 0, 1};   // i = 110 -> +1a, q = 001 -> -1a
    z = qamMap(3, b3);
    CHECK(std::fabs(z.real() - qamNorm(3)) < 1e-6 && std::fabs(z.imag() + qamNorm(3)) < 1e-6, "64-QAM 110 001 is not (1a, -1a)");
    const uint8_t b4[4] = {1, 0, 0, 1};         // 16-QAM i = 10 -> +1a, q = 01 -> -1a
    z = qamMap(2, b4);
    CHECK(std::fabs(z.real() - qamNorm(2)) < 1e-6 && std::fabs(z.imag() + qamNorm(2)) < 1e-6, "16-QAM 10 01 is not (1a, -1a)");
    const uint8_t b5[4] = {1, 1, 0, 0};         // i = 11 -> -3a, q = 00 -> +3a
    z = qamMap(2, b5);
    CHECK(std::fabs(z.real() + 3 * qamNorm(2)) < 1e-6 && std::fabs(z.imag() - 3 * qamNorm(2)) < 1e-6, "16-QAM 11 00 is not (-3a, 3a)");
    const uint8_t b6[2] = {1, 0};
    z = qamMap(1, b6);
    CHECK(std::fabs(z.real() + qamNorm(1)) < 1e-6 && std::fabs(z.imag() - qamNorm(1)) < 1e-6, "4-QAM 1 0 is not (-1a, 1a)");
    for (int levels = 1; levels <= 3; levels++) {
        // each constellation point maps back to its bits
        const int n = 1 << (2 * levels);
        double pw = 0;
        for (int v = 0; v < n; v++) {
            uint8_t bits[6], back[6];
            for (int i = 0; i < 2 * levels; i++) bits[i] = (uint8_t)((v >> i) & 1);
            const cf32 c = qamMap(levels, bits);
            qamDemapHard(levels, c, back);
            CHECK(memcmp(bits, back, (size_t)(2 * levels)) == 0, "QAM demap of %d-level point %d", levels, v);
            pw += std::norm(c);
        }
        CHECK(std::fabs(pw / n - 1.0) < 1e-5, "%d-level constellation has power %.4f", levels, pw / n);
    }
}

// ---- messages
static void testMessages() {
    // FAC: build, parse, CRC
    FacInfo f;
    f.identity = 1; f.occupancy = 3; f.interleaver = 1; f.mscMode = 0; f.sdcMode = 0; f.numServicesCode = 4;
    f.svc[0].id = 0xABCDEF; f.svc[0].shortId = 2; f.svc[0].language = 5; f.svc[0].descriptor = 10;
    uint8_t bits[120];
    facBuild(f, bits);
    FacInfo g;
    CHECK(facParse(bits, false, g), "FAC parse failed");
    CHECK(g.occupancy == 3 && g.identity == 1 && g.interleaver == 1 && g.svc[0].id == 0xABCDEF && g.svc[0].shortId == 2 && g.svc[0].language == 5 && g.svc[0].descriptor == 10, "FAC fields changed");
    bits[17] ^= 1;
    CHECK(!facParse(bits, false, g), "FAC with a flipped bit passed the CRC");
    FacInfo e;
    e.modeE = true; e.nSvc = 2; e.svc[0].id = 5; e.svc[1].id = 6; e.svc[1].shortId = 1;
    facBuild(e, bits);
    CHECK(facParse(bits, true, g) && g.svc[1].id == 6 && g.svc[1].shortId == 1, "mode E FAC round trip");
    CHECK(facAudioServices(4) == 1 && facDataServices(4) == 0 && facAudioServices(5) == 1 && facDataServices(5) == 1 && facAudioServices(0) == 4 && facDataServices(15) == 4, "service count codes");
    // SDC entities
    SdcAudio a;
    a.shortId = 1; a.streamId = 0; a.coding = 0; a.sbr = 1; a.mode = 2; a.rateCode = 3; a.text = true;
    SdcMux mx;
    mx.nStreams = 2; mx.protA = 0; mx.protB = 1; mx.stream[0] = {0, 1105}; mx.stream[1] = {59, 0};
    SdcTime tm;
    tm.mjd = dateToMjd(2026, 10, 6); tm.hour = 17; tm.minute = 42; tm.hasOffset = true; tm.offsetHalfHours = 8;
    std::vector<uint8_t> field;
    sdcPutEntity(field, 0, 0, sdcEntityMux(mx));
    sdcPutEntity(field, 9, 0, sdcEntityAudio(a));
    sdcPutEntity(field, 1, 0, sdcEntityLabel(1, "OnAir DRM"));
    sdcPutEntity(field, 12, 0, sdcEntityLang(1, "eng", "gb"));
    sdcPutEntity(field, 8, 0, sdcEntityTime(tm));
    SdcInfo si;
    const int n = sdcParse(field.data(), (int)field.size(), si);
    CHECK(n == 5, "%d SDC entities parsed", n);
    CHECK(si.mux.present && si.mux.nStreams == 2 && si.mux.protB == 1 && si.mux.stream[0].lenB == 1105 && si.mux.stream[1].lenA == 59, "SDC type 0");
    CHECK(si.audio[1].present && si.audio[1].sbr == 1 && si.audio[1].mode == 2 && si.audio[1].rateHz() == 24000 && si.audio[1].text, "SDC type 9");
    CHECK(si.label[1].present && si.label[1].text == "OnAir DRM", "SDC label '%s'", si.label[1].text.c_str());
    CHECK(si.lang[1].present && si.lang[1].language == "eng" && si.lang[1].country == "gb", "SDC type 12");
    CHECK(si.time.present && si.time.mjd == tm.mjd && si.time.hour == 17 && si.time.minute == 42 && si.time.offsetHalfHours == 8, "SDC type 8");
    int y, mo, d;
    mjdToDate(tm.mjd, y, mo, d);
    CHECK(y == 2026 && mo == 10 && d == 6, "MJD %d is %d-%d-%d", tm.mjd, y, mo, d);
    CHECK(dateToMjd(1858, 11, 17) == 0 && dateToMjd(2000, 1, 1) == 51544, "MJD epoch");
    std::vector<uint8_t> sb;
    sdcBuildBits(5, field, 85, sb);
    int afs = 0;
    CHECK(sdcCheckBits(sb.data(), 85, afs) && afs == 5, "SDC block check");
    sb[100] ^= 1;
    CHECK(!sdcCheckBits(sb.data(), 85, afs), "SDC block with an error passed");
    // text message: encode, decode, a changed message, and a lost unit
    TextEncoder te;
    TextDecoder td;
    const std::string msg = "OnAir DRM test signal: station name and a text message of more than sixteen characters";
    te.set(msg);
    uint8_t unit[4];
    bool changed = false;
    int frames = 0;
    for (; frames < 200 && td.text() != msg; frames++) { te.next(unit); changed |= td.feed(unit); }
    CHECK(td.text() == msg && changed, "text message '%s' after %d frames", td.text().c_str(), frames);
    te.set("short");
    for (int i = 0; i < 100 && td.text() != "short"; i++) { te.next(unit); td.feed(unit); }
    CHECK(td.text() == "short", "second message '%s'", td.text().c_str());
    // AAC audio super frame: EEP and UEP
    std::mt19937 rng(5);
    for (int nfr : {5, 10}) {
        for (int hp : {0, 6}) {
            std::vector<std::vector<uint8_t>> fr;
            std::vector<uint8_t> crc;
            for (int i = 0; i < nfr; i++) {
                std::vector<uint8_t> x(60 + rng() % 40);
                for (auto& v : x) v = (uint8_t)rng();
                fr.push_back(x);
                crc.push_back((uint8_t)rng());
            }
            int len = aacHeaderBytes(nfr) + nfr;           // the last frame runs to the end of the logical frame: the frames fill it exactly
            for (const auto& x : fr) len += (int)x.size();
            std::vector<uint8_t> lf;
            CHECK(aacSuperFrameBuild(fr, crc, hp, len, lf), "AAC super frame build");
            AacSuperFrame sf;
            const int lenA = hp ? aacHeaderBytes(nfr) + nfr * (hp + 1) : 0;
            CHECK(aacSuperFrameParse(lf.data(), len, lenA, nfr, sf), "AAC super frame parse (%d frames, hp %d)", nfr, hp);
            CHECK(sf.frames == fr && sf.crc == crc, "AAC super frame content (%d frames, hp %d)", nfr, hp);
        }
    }
    CHECK(aacNumFrames(false, 12000) == 5 && aacNumFrames(false, 24000) == 10 && aacNumFrames(true, 24000) == 5 && aacNumFrames(true, 48000) == 10 && aacNumFrames(false, 48000) == 0, "frames per audio super frame");
    CHECK(aacHeaderBytes(5) == 6 && aacHeaderBytes(10) == 14, "audio super frame header size");
}

// ---- real cells: independent of the generator
static void realFac(const char* what, const char* cellsB64, const char* expect, bool modeE = false) {
    const auto z = cellsOf(cellsB64);
    CHECK(z.size() == 65, "%s: %zu FAC cells", what, z.size());
    MlcParams p;
    p.levels = 1; p.n2 = 65; p.rxB[0] = 3; p.ryB[0] = 5; p.fac = true;
    MlcCode c(p);
    std::vector<float> w(z.size(), 1.f);
    std::vector<uint8_t> u((size_t)c.infoBits()), pr(u.size());
    c.decode(z.data(), w.data(), u.data(), 1);
    prbs(pr.data(), pr.size());
    for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
    FacInfo f;
    CHECK(facParse(u.data(), modeE, f), "%s: the FAC of the real signal did not pass its CRC", what);
    std::string s;
    for (size_t i = 0; i < 72; i++) s.push_back((char)('0' + u[i]));
    CHECK(s == expect, "%s: FAC bits\n  %s\n  %s", what, s.c_str(), expect);
}

static void testReal() {
    realFac("real FAC, Deutschlandradio (mode A)", kRealFacA, kRealFacABits);
    realFac("real FAC, Deutsche Welle (mode B)", kRealFacB, kRealFacBBits);
    realFac("real FAC, RNW (mode C)", kRealFacC, kRealFacCBits);
    {   // the fields of the mode A signal: 9 kHz, long interleaving, 64-QAM, SDC 16-QAM, one audio service
        const auto z = cellsOf(kRealFacA);
        MlcParams p; p.levels = 1; p.n2 = 65; p.rxB[0] = 3; p.ryB[0] = 5; p.fac = true;
        MlcCode c(p);
        std::vector<float> w(z.size(), 1.f);
        std::vector<uint8_t> u((size_t)c.infoBits()), pr(u.size());
        c.decode(z.data(), w.data(), u.data(), 1);
        prbs(pr.data(), pr.size());
        for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
        FacInfo f;
        facParse(u.data(), false, f);
        CHECK(f.occupancy == 2 && f.interleaver == 0 && f.mscMode == 0 && f.sdcMode == 0 && facAudioServices(f.numServicesCode) == 1 && facDataServices(f.numServicesCode) == 0,
              "real FAC fields: occupancy %d interleaver %d msc %d sdc %d services %d", f.occupancy, f.interleaver, f.mscMode, f.sdcMode, f.numServicesCode);
    }
    {   // SDC of the same station: 359 cells, 16-QAM with rates 1/3 and 2/3, 85 data bytes
        const auto z = cellsOf(kRealSdcA);
        const auto wb = unb64(kRealSdcAW);
        CHECK(z.size() == 359 && wb.size() == 359, "real SDC: %zu cells", z.size());
        std::vector<float> w(wb.size());
        for (size_t i = 0; i < w.size(); i++) w[i] = (float)wb[i] / 50.f;
        MlcParams p;
        p.levels = 2; p.n2 = 359; p.rxB[0] = 1; p.ryB[0] = 3; p.rxB[1] = 2; p.ryB[1] = 3;
        MlcCode c(p);
        CHECK(c.infoBits() == 705, "real SDC: %d information bits", c.infoBits());
        std::vector<uint8_t> u((size_t)c.infoBits()), pr(u.size());
        c.decode(z.data(), w.data(), u.data(), 2);
        prbs(pr.data(), pr.size());
        for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
        int afs = -1;
        CHECK(sdcCheckBits(u.data(), 85, afs), "real SDC: CRC failed");
        std::vector<uint8_t> field(85);
        for (int i = 0; i < 85 * 8; i++) field[(size_t)i / 8] |= (uint8_t)(u[(size_t)(4 + i)] << (7 - i % 8));
        const auto expect = hexBytes(kRealSdcAData);
        CHECK(field == expect, "real SDC: data field differs from the prototype's");
        SdcInfo si;
        sdcParse(field.data(), 85, si);
        CHECK(si.mux.present && si.mux.nStreams == 1 && si.mux.protA == 0 && si.mux.protB == 1 && si.mux.stream[0].lenA == 0 && si.mux.stream[0].lenB == 1181, "real SDC: multiplex description");
        CHECK(si.audio[0].present && si.audio[0].coding == 0 && si.audio[0].sbr == 1 && si.audio[0].mode == 1 && si.audio[0].rateHz() == 24000 && !si.audio[0].text,
              "real SDC: audio information coding %d sbr %d mode %d rate %d", si.audio[0].coding, si.audio[0].sbr, si.audio[0].mode, si.audio[0].rateHz());
        CHECK(si.label[0].present && si.label[0].text == "DeutschlandRadio", "real SDC: label '%s'", si.label[0].text.c_str());
        CHECK(si.lang[0].present && si.lang[0].language == "deu" && si.lang[0].country == "de", "real SDC: language '%s' country '%s'", si.lang[0].language.c_str(), si.lang[0].country.c_str());
    }
    {   // one multiplex frame of the same station after the time deinterleaver: 2632 cells of 64-QAM, levels 1/3, 2/3, 4/5 (protection level 1), 1181 bytes of audio
        const auto z = cellsOf(kRealMscA);
        const auto wb = unb64(kRealMscAW);
        CHECK(z.size() == 2632 && wb.size() == 2632, "real MSC: %zu cells", z.size());
        std::vector<float> w(wb.size());
        for (size_t i = 0; i < w.size(); i++) w[i] = (float)wb[i] / 50.f;
        MlcParams p;
        p.levels = 3; p.n2 = 2632;
        for (int l = 0; l < 3; l++) { const Rate r = mscRate(0, 6, 1, l); p.rxB[l] = r.rx; p.ryB[l] = r.ry; }
        MlcCode c(p);
        CHECK(c.infoBits() == 9450, "real MSC: %d bits", c.infoBits());
        std::vector<uint8_t> u((size_t)c.infoBits()), pr(u.size());
        c.decode(z.data(), w.data(), u.data(), 2);
        prbs(pr.data(), pr.size());
        for (size_t i = 0; i < u.size(); i++) u[i] ^= pr[i];
        std::vector<uint8_t> lf(1181);
        for (int i = 0; i < 1181 * 8; i++) lf[(size_t)i / 8] |= (uint8_t)(u[(size_t)i] << (7 - i % 8));
        AacSuperFrame sf;
        const bool ok = aacSuperFrameParse(lf.data(), 1181, 0, 10, sf);
        CHECK(ok && sf.headerOk, "real MSC: the audio super frame header (borders 12 bits each, increasing) is not valid");
        if (ok) {
            int total = 0;
            for (auto& fr : sf.frames) total += (int)fr.size();
            CHECK(total == 1181 - 14 - 10, "real MSC: frame sizes add up to %d", total);
            CHECK(sf.frames[0].size() == 120, "real MSC: first frame has %zu bytes (the prototype decoded 120)", sf.frames[0].size());
        }
    }
}

int main() {
    testCrcPrbs();
    testLayout();
    testPilots();
    testPuncturing();
    testAnnexJ();
    testRoundTrips();
    testQam();
    testMessages();
    testReal();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
