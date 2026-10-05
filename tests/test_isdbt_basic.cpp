// ISDB-T basics: geometry and bit rates against the tables of the standard, carrier layout, pilot sequences, TMCC coding, constellations.
#include "dect2/isdbt.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2::isdbt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // frame lengths and bandwidth (Table 3-1/3-2)
    CHECK(std::fabs(frameSeconds(1, kGi4) * 1e3 - 64.26) < 0.01, "frame length mode 1 1/4: %f", frameSeconds(1, kGi4) * 1e3);
    CHECK(std::fabs(frameSeconds(1, kGi8) * 1e3 - 57.834) < 0.01, "frame length mode 1 1/8");
    CHECK(std::fabs(frameSeconds(2, kGi16) * 1e3 - 109.242) < 0.01, "frame length mode 2 1/16");
    CHECK(std::fabs(frameSeconds(3, kGi32) * 1e3 - 212.058) < 0.01, "frame length mode 3 1/32");
    CHECK(totalCarriers(1) == 1405 && totalCarriers(2) == 2809 && totalCarriers(3) == 5617, "carrier counts");
    CHECK(centerCarrier(1) == 702 && centerCarrier(2) == 1404 && centerCarrier(3) == 2808, "centre carriers");
    // packets per frame for one segment (Table 3-3)
    struct Row { int mod, rate, mode, n; } rows[] = {
        {kDqpsk, kR12, 1, 12}, {kDqpsk, kR12, 3, 48}, {kQpsk, kR78, 1, 21}, {kQpsk, kR56, 2, 40}, {k16Qam, kR23, 1, 32}, {k16Qam, kR78, 3, 168},
        {k64Qam, kR12, 1, 36}, {k64Qam, kR34, 2, 108}, {k64Qam, kR78, 1, 63}, {k64Qam, kR78, 3, 252}};
    for (auto& r : rows) {
        Layer l; l.segments = 1; l.mod = r.mod; l.rate = r.rate;
        CHECK(packetsPerFrame(r.mode, l) == r.n, "packets per frame mod %d rate %d mode %d: %d (want %d)", r.mod, r.rate, r.mode, packetsPerFrame(r.mode, l), r.n);
    }
    // total data rate of 13 segments (Table 3-4): 64QAM 3/4 mode 1: 702 packets, 1/8 guard: 18.255 Mbit/s
    Params p; p.mode = 1; p.guard = kGi8; p.layer[0].segments = 13; p.layer[0].mod = k64Qam; p.layer[0].rate = kR34;
    CHECK(p.valid(), "13-segment configuration is valid");
    CHECK(packetsPerFrame(1, p.layer[0]) == 702, "702 packets per frame");
    CHECK(std::fabs(totalBitrate(p) / 1e6 - 18.255) < 0.002, "total rate %f", totalBitrate(p) / 1e6);
    p.layer[0].mod = k16Qam; p.layer[0].rate = kR12; p.guard = kGi4;
    CHECK(std::fabs(totalBitrate(p) / 1e6 - 7.302) < 0.002, "16QAM 1/2 1/4: %f", totalBitrate(p) / 1e6);

    // carrier roles: 96/192/384 data carriers in every segment and symbol, and the right number of pilots, TMCC and AC carriers
    for (int mode = 1; mode <= 3; mode++) {
        const int cps = carriersPerSegment(mode), dps = dataPerSegment(mode);
        std::vector<uint8_t> roles((size_t)cps);
        for (int seg = 0; seg < kSegments; seg++) {
            for (int diff = 0; diff < 2; diff++) {
                for (int sym = 0; sym < 8; sym++) {
                    segmentRoles(mode, seg, diff != 0, sym, roles.data());
                    int cnt[6] = {0};
                    for (int i = 0; i < cps; i++) cnt[roles[i]]++;
                    const int ac1 = 2 << (mode - 1), tmcc = diff ? 5 << (mode - 1) : 1 << (mode - 1), ac2 = diff ? (mode == 1 ? 4 : mode == 2 ? 9 : 19) : 0;
                    const int sp = diff ? 0 : cps / 12;
                    CHECK(cnt[kData] == dps, "mode %d seg %d diff %d sym %d: %d data carriers", mode, seg, diff, sym, cnt[kData]);
                    CHECK(cnt[kAC1] == ac1 && cnt[kAC2] == ac2 && cnt[kTMCC] == tmcc && cnt[kSP] == sp && cnt[kCP] == (diff ? 1 : 0),
                          "mode %d seg %d diff %d: roles sp %d cp %d tmcc %d ac1 %d ac2 %d", mode, seg, diff, cnt[kSP], cnt[kCP], cnt[kTMCC], cnt[kAC1], cnt[kAC2]);
                }
            }
        }
    }
    // the pilot sequence: the start value of every segment is the state after the carriers of the segments below it (Table 3-16)
    for (int mode = 1; mode <= 3; mode++) {
        unsigned r[11];
        for (int i = 0; i < 11; i++) r[i] = 1;
        const int cps = carriersPerSegment(mode);
        bool ok = true;
        for (int pos = 0; pos < kSegments; pos++) {
            const int seg = kSegmentAtPosition[pos];
            const auto& w = prbsW(mode, seg);
            for (int i = 0; i < cps; i++) {
                if (w[(size_t)i] != r[10]) ok = false;
                const unsigned fb = r[8] ^ r[10];
                for (int k = 10; k > 0; k--) r[k] = r[k - 1];
                r[0] = fb;
            }
        }
        CHECK(ok, "pilot sequence runs through the segments (mode %d)", mode);
    }
    // carrier randomizing tables are permutations
    {
        const uint16_t* t[3] = {tables::kRandomizing1, tables::kRandomizing2, tables::kRandomizing3};
        for (int m = 0; m < 3; m++) {
            const int n = 96 << m;
            std::vector<int> seen((size_t)n, 0);
            for (int i = 0; i < n; i++) seen[t[m][i]]++;
            bool ok = true;
            for (int i = 0; i < n; i++) if (seen[(size_t)i] != 1) ok = false;
            CHECK(ok, "randomizing table of mode %d is a permutation", m + 1);
        }
    }

    // segment layout: 1-segment partial reception layer A (segment 0), differential layer B, synchronous layer C
    {
        Params q; q.mode = 3; q.partial = true;
        q.layer[0].segments = 1; q.layer[0].mod = kQpsk;
        q.layer[1].segments = 3; q.layer[1].mod = kDqpsk;
        q.layer[2].segments = 9; q.layer[2].mod = k64Qam;
        SegmentInfo info[kSegments];
        CHECK(q.valid() && segmentLayout(q, info), "layout valid");
        CHECK(info[0].partial && info[0].group == 0 && info[0].layer == 0, "segment 0 is the partial reception layer");
        CHECK(info[1].diff && info[1].group == 1 && info[1].index == 0 && info[1].groupSize == 3 && info[3].index == 2, "differential group");
        CHECK(!info[4].diff && info[4].group == 2 && info[4].index == 0 && info[4].groupSize == 9 && info[12].index == 8, "synchronous group");
        Params bad = q; bad.layer[1].mod = k16Qam; bad.layer[2].mod = kDqpsk;
        CHECK(!bad.valid(), "differential segments after synchronous ones are refused");
    }

    // TMCC: coding of the parameters, parity code and error correction
    {
        Params q; q.mode = 2; q.partial = true;
        q.layer[0].segments = 1; q.layer[0].mod = kQpsk; q.layer[0].rate = kR23; q.layer[0].ti = 2;
        q.layer[1].segments = 12; q.layer[1].mod = k64Qam; q.layer[1].rate = kR34; q.layer[1].ti = 1;
        Tmcc t = tmccFromParams(q);
        uint8_t info[kTmccInfoBits];
        tmccPack(t, info);
        Tmcc t2; tmccUnpack(info, t2);
        Params q2; q2.mode = 2;
        CHECK(paramsFromTmcc(t2, q2) && q2 == q, "TMCC parameters survive packing");
        uint8_t bits[kSymbolsPerFrame];
        tmccFrameBits(info, true, false, bits);
        CHECK(tmccValid(bits + 20), "TMCC code word valid");
        uint8_t odd[kSymbolsPerFrame];
        tmccFrameBits(info, false, true, odd);
        bool inv = true;
        for (int i = 1; i <= 16; i++) if (odd[i] == bits[i]) inv = false;
        CHECK(inv && odd[17] == 1 && bits[17] == 0 && !std::memcmp(odd + 20, bits + 20, 184), "alternating synchronising word and segment type");
        // correct up to four errors among the least reliable bits
        std::mt19937 rng(5);
        int okCount = 0, tries = 200;
        for (int k = 0; k < tries; k++) {
            uint8_t w[184]; std::memcpy(w, bits + 20, 184);
            float rel[184];
            for (int i = 0; i < 184; i++) rel[i] = 1.f + (float)(rng() % 1000) / 100.f;
            const int errs = 1 + (int)(rng() % 4);
            for (int e = 0; e < errs; e++) { int pos = (int)(rng() % 184); w[pos] ^= 1; rel[pos] = 0.1f + 0.01f * e; }
            if (tmccCorrect(w, rel) && !std::memcmp(w, bits + 20, 184)) okCount++;
        }
        CHECK(okCount == tries, "TMCC correction: %d of %d", okCount, tries);
        uint8_t w[184]; std::memcpy(w, bits + 20, 184);
        for (int i = 0; i < 184; i += 11) w[i] ^= 1;   // 17 errors spread everywhere: must not be accepted as another word silently
        float rel[184]; for (auto& r : rel) r = 1.f;
        CHECK(!tmccCorrect(w, rel), "heavily damaged TMCC word is refused");
    }

    // constellations: unit power, Gray labelling, noise-free hard decisions
    {
        const int mods[4] = {kQpsk, k16Qam, k64Qam, kDqpsk};
        for (int mi = 0; mi < 4; mi++) {
            const int mod = mods[mi], m = bitsPerCell(mod);
            double pw = 0;
            bool ok = true;
            for (unsigned lab = 0; lab < (1u << m); lab++) {
                const cf32 z = mapLabel(mod, lab);
                pw += std::norm(z);
                float llr[6];
                demapCell(mod, z, 0.01f, llr);
                for (int b = 0; b < m; b++) if (((llr[b] < 0) ? 1u : 0u) != ((lab >> (m - 1 - b)) & 1)) ok = false;
            }
            pw /= (double)(1u << m);
            CHECK(std::fabs(pw - 1.0) < 1e-5, "unit average power for modulation %d: %f", mod, pw);
            CHECK(ok, "hard decisions recover the label (modulation %d)", mod);
        }
        // Gray property of 64QAM: nearest neighbours differ in one bit
        const float d = 2.f / std::sqrt(42.f);
        int bad = 0;
        for (unsigned a = 0; a < 64; a++)
            for (unsigned b = 0; b < 64; b++) {
                if (a == b) continue;
                const cf32 za = mapLabel(k64Qam, a), zb = mapLabel(k64Qam, b);
                if (std::abs(za - zb) < d * 1.01f && __builtin_popcount(a ^ b) != 1) bad++;
            }
        CHECK(bad == 0, "64QAM is a Gray mapping");
    }
    // the fast demapper agrees with the comparison against every point
    {
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.f, 0.6f);
        double worst = 0;
        for (int mod : {kQpsk, k16Qam, k64Qam}) {
            for (int k = 0; k < 20000; k++) {
                const cf32 z(nd(rng), nd(rng));
                float a[6], b[6];
                demapCell(mod, z, 0.05f, a);
                demapCellGeneric(mod, z, 0.05f, b);
                for (int i = 0; i < bitsPerCell(mod); i++) worst = std::max(worst, (double)std::fabs(a[i] - b[i]) / (1.0 + std::fabs(b[i])));
            }
        }
        CHECK(worst < 1e-3, "fast demapper differs from the reference by %g", worst);
    }
    printf(fails ? "isdbt basic: FAILED\n" : "isdbt basic: ok\n");
    return fails ? 1 : 0;
}
