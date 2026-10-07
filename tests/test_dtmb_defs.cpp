// DTMB tables against what the standard (as quoted by other implementations) and the codes' own properties say.
#include "dect2/dtmb_defs.h"
#include "dect2/dtmb_ldpc.h"
#include "dect2/dtmb_map.h"
#include <random>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// bit n of a hexadecimal string, most significant first; 1 means chip -1
static int hexBit(const std::string& h, int n) {
    const char d = h[(size_t)(n / 4)];
    const int v = d <= '9' ? d - '0' : d - 'A' + 10;
    return (v >> (3 - n % 4)) & 1;
}

static void checkVector(Header h, const std::string& hex) {
    const auto& c = pnChips(h);
    const int n = headerInfo(h).length;
    CHECK((int)c.size() == n, "length %d", (int)c.size());
    int bad = 0;
    for (int i = 0; i < n; i++) if ((hexBit(hex, i) ? -1 : 1) != c[(size_t)i]) bad++;
    CHECK(bad == 0, "%s: %d chips differ from the vector", headerInfo(h).name, bad);
}

int main() {
    // Header vectors of GB 20600 appendices D, E and clause 4.6.2.2 as quoted by github.com/ningzichun/dtmb-sdr (tests/test_pn.cpp).
    checkVector(Header::Pn420,
        "B0A5E9FEA1CF0D9A3DC7407C4A22D5C8C938109BCCEFCB2B69063"
        "58AA60BAFB7614BD3FD439E1B347B8E80F89445AB91927021379");
    checkVector(Header::Pn945,
        "FB946DFF3259AD7E9A7C9CDC081A82531A292661E8F1233A432F2D8055B"
        "ABDE0EBA16959060BE1BD4B9EDAA88E44D953B15C5DA03FB3F1884F38EEF"
        "AC28708B1F728DBFE64B35AFD34F939B8103504A634524CC3D1E24674865"
        "E5B00AB757BC1D742D2B20C17C37A973DB5511C89B2A762B8BB407F678");
    checkVector(Header::Pn595,
        "004934D7CC7C8EFC380FFC713B2BBD47A5417FAABD0E9196B3D633F2A9994FA708D914DEEAE67773"
        "A9D07B70C4A59A22D2E98B0292FBC63761E4E5886FE71A94212DF5C5C87DAA2F673E0");

    // PN420 seeds of the phase table (Table 2 in the standard, as quoted by the same project): first eight chips of the header of frame n
    {
        const int idx[8] = {0, 1, 2, 111, 112, 113, 224, 225};
        const int seed[8] = {0xB0, 0x61, 0xD8, 0x9A, 0x83, 0x9A, 0xB0, 0xB0};
        for (int k = 0; k < 8; k++) {
            int8_t h[420];
            pnHeader(Header::Pn420, pnPhase(Header::Pn420, idx[k]), h);
            int v = 0;
            for (int b = 0; b < 8; b++) v = (v << 1) | (h[b] < 0);
            CHECK(v == seed[k], "frame %d seed %02X expected %02X", idx[k], v, seed[k]);
        }
    }

    // The cores are m-sequences: balanced, two-level periodic autocorrelation
    for (Header h : {Header::Pn420, Header::Pn945}) {
        const auto& c = pnChips(h);
        const int P = headerInfo(h).core;
        int sum = 0;
        for (int i = 0; i < P; i++) sum += c[(size_t)i];
        CHECK(sum == -1, "%s core sum %d", headerInfo(h).name, sum);
        int worst = 0;
        for (int lag = 1; lag < P; lag++) {
            int a = 0;
            for (int i = 0; i < P; i++) a += c[(size_t)i] * c[(size_t)((i + lag) % P)];
            if (a != -1) worst++;
        }
        CHECK(worst == 0, "%s: %d lags with autocorrelation other than -1", headerInfo(h).name, worst);
        // the header is the sequence continued periodically
        bool periodic = true;
        for (int i = P; i < headerInfo(h).length; i++) if (c[(size_t)i] != c[(size_t)(i - P)]) periodic = false;
        CHECK(periodic, "%s is not periodic", headerInfo(h).name);
    }
    {   // PN595: x^10 + x^3 + 1 has period 1023; the header is its first 595 chips
        std::vector<int> b = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        int period = 0;
        std::vector<int> s = b;
        for (int i = 1; i <= 2000; i++) {
            const int nb = s[s.size() - 10] ^ s[s.size() - 3];
            s.push_back(nb);
            bool same = true;
            for (int j = 0; j < 10; j++) if (s[s.size() - 10 + (size_t)j] != b[(size_t)j]) same = false;
            if (same) { period = i; break; }
        }
        CHECK(period == 1023, "PN595 generator period %d", period);
    }

    // Phase schedule: values and the turn-around
    CHECK(pnPhase(Header::Pn420, 0) == 0 && pnPhase(Header::Pn420, 1) == 1 && pnPhase(Header::Pn420, 2) == 254 && pnPhase(Header::Pn420, 3) == 2, "schedule start");
    CHECK(pnPhase(Header::Pn420, 112) == 255 - 56 && pnPhase(Header::Pn420, 113) == 56 && pnPhase(Header::Pn420, 224) == 0 && pnPhase(Header::Pn420, 225) == 0, "schedule turn");
    for (Header h : {Header::Pn420, Header::Pn945}) {
        std::set<int> seen;
        for (int f = 0; f < headerInfo(h).framesPerSuper / 2 + 1; f++) seen.insert(pnPhase(h, f));
        CHECK((int)seen.size() == headerInfo(h).framesPerSuper / 2 + 1, "phases of the first half repeat");
        CHECK(pnPhase(h, 0) == 0, "phase 0");
    }
    CHECK(frameLength(Header::Pn420) * headerInfo(Header::Pn420).framesPerSuper == 945000, "super-frame length PN420");
    CHECK(frameLength(Header::Pn595) * headerInfo(Header::Pn595).framesPerSuper == 945000, "super-frame length PN595");
    CHECK(frameLength(Header::Pn945) * headerInfo(Header::Pn945).framesPerSuper == 945000, "super-frame length PN945");

    // System information: 22 indices, all words differ by 16 chips (or 32 for the complement pair)
    {
        std::vector<std::vector<uint8_t>> w;
        std::vector<int> idx;
        for (int i = 3; i <= 24; i++) {
            Profile p;
            CHECK(profileFromSi(i, p), "index %d", i);
            CHECK(siIndex(p) == i, "index %d round trip gives %d", i, siIndex(p));
            std::vector<uint8_t> c(36);
            siChips(i, c.data());
            w.push_back(c); idx.push_back(i);
            for (int s = 0; s < 4; s++) CHECK(c[(size_t)s] == 0, "leading chips");
        }
        int bad = 0;
        for (size_t a = 0; a < w.size(); a++) for (size_t b = a + 1; b < w.size(); b++) {
            int d = 0;
            for (int s = 4; s < 36; s++) d += w[a][(size_t)s] != w[b][(size_t)s];
            const bool pair = (idx[a] - 3) / 2 == (idx[b] - 3) / 2;
            if (d != (pair ? 32 : 16)) bad++;
        }
        CHECK(bad == 0, "%d word pairs at the wrong distance", bad);
        Profile p;
        CHECK(!profileFromSi(2, p) && !profileFromSi(25, p), "range");
        Profile nr; nr.map = Mapping::Qam4Nr; nr.rate = Rate::R04;
        CHECK(!profileValid(nr) && siIndex(nr) == -1, "NR at 0.4 does not exist");
        Profile q32; q32.map = Mapping::Qam32; q32.rate = Rate::R06;
        CHECK(!profileValid(q32), "32QAM at 0.6 does not exist");
    }
    {   // positions of the system information and the carrier interleaver
        const auto& pos = siPositions();
        std::set<int> u(pos.begin(), pos.end());
        CHECK(u.size() == 36 && pos[0] == 0 && pos[35] == 3779 && pos[1] == 140 && pos[2] == 279 && pos[3] == 419 && pos[4] == 420, "SI positions");
        const auto& m = carrierMap();
        std::set<int> phys(m.begin(), m.end());
        CHECK(phys.size() == 3780 && *phys.begin() == 0 && *phys.rbegin() == 3779, "carrier map is not a permutation");
        CHECK(m[0] == 0, "logical 0 maps to carrier 0");
    }

    // 4QAM-NR: a (16,256,6) code with weights 0, 6, 8, 10, 16 in numbers 1, 112, 30, 112, 1; systematic
    {
        int wd[17] = {};
        std::set<int> words;
        for (int x = 0; x < 256; x++) {
            const int w = (x << 8) | nrParity((uint8_t)x);
            words.insert(w);
            wd[__builtin_popcount((unsigned)w)]++;
        }
        CHECK(words.size() == 256, "NR words");
        CHECK(wd[0] == 1 && wd[6] == 112 && wd[8] == 30 && wd[10] == 112 && wd[16] == 1, "NR weights %d %d %d %d %d", wd[0], wd[6], wd[8], wd[10], wd[16]);
        int minDist = 99;
        for (int a = 0; a < 256; a++) for (int b = a + 1; b < 256; b++) {
            const int d = __builtin_popcount((unsigned)(((a << 8) | nrParity((uint8_t)a)) ^ ((b << 8) | nrParity((uint8_t)b))));
            minDist = std::min(minDist, d);
        }
        CHECK(minDist == 6, "NR minimum distance %d", minDist);
        CHECK(nrParity(0x01) == 0xFE && nrParity(0x02) == 0x97 && nrParity(0xFF) == 0xFF && nrParity(0xFD) == 0x68, "NR table entries");
    }

    // Constellations: unit power, 4 / 16 / 32 / 64 distinct points, Gray along each axis of the square ones
    for (Mapping m : {Mapping::Qam4, Mapping::Qam16, Mapping::Qam32, Mapping::Qam64}) {
        const QamPoint* p = qamPoints(m);
        const int n = qamCount(m);
        double pw = 0;
        std::set<std::pair<int, int>> u;
        for (int i = 0; i < n; i++) { pw += p[i].re * p[i].re + p[i].im * p[i].im; u.insert({(int)std::lround(p[i].re * 1000), (int)std::lround(p[i].im * 1000)}); }
        CHECK(std::fabs(pw / n - 1.0) < 1e-5, "%s power %f", mappingName(m), pw / n);
        CHECK((int)u.size() == n, "%s points distinct", mappingName(m));
        if (m == Mapping::Qam32) {
            // cross shape: no corner points
            float mx = 0; for (int i = 0; i < n; i++) mx = std::max(mx, std::fabs(p[i].re));
            int corners = 0;
            for (int i = 0; i < n; i++) if (std::fabs(std::fabs(p[i].re) - mx) < 1e-4 && std::fabs(std::fabs(p[i].im) - mx) < 1e-4) corners++;
            CHECK(corners == 0, "32QAM corners");
            continue;
        }
        // each point's nearest neighbours along an axis differ in exactly one bit
        const int a = bitsPerSymbol(m) / 2;
        const float step = (m == Mapping::Qam4) ? 0 : (m == Mapping::Qam16 ? 4.f / std::sqrt(40.f) : 2.f / std::sqrt(42.f));
        int badGray = 0;
        if (step > 0) for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
            const bool sameQ = std::fabs(p[i].im - p[j].im) < 1e-4, adjI = std::fabs(std::fabs(p[i].re - p[j].re) - step) < 1e-4;
            if (sameQ && adjI && __builtin_popcount((unsigned)(i ^ j)) != 1) badGray++;
        }
        CHECK(badGray == 0, "%s Gray", mappingName(m));
        (void)a;
    }
    {   // spot checks of the labelling: 16QAM level -3 / -1 / +1 / +3 on I carry (b0,b1) = 00, 10, 11, 01
        const QamPoint* p = qamPoints(Mapping::Qam16);
        const float s = 1.f / std::sqrt(10.f);
        CHECK(std::fabs(p[0].re + 3 * s) < 1e-5 && std::fabs(p[1].re + 1 * s) < 1e-5 && std::fabs(p[3].re - 1 * s) < 1e-5 && std::fabs(p[2].re - 3 * s) < 1e-5, "16QAM I labels");
        const QamPoint* q4 = qamPoints(Mapping::Qam4);
        CHECK(q4[0].re < 0 && q4[0].im < 0 && q4[1].re > 0 && q4[1].im < 0 && q4[2].re < 0 && q4[2].im > 0, "4QAM labels");
    }

    // Net bit rates: the published range of DTMB is 4.813 to 32.486 Mbit/s (PN945 4QAM 0.4 to PN420 64QAM 0.8)
    {
        Profile lo; lo.map = Mapping::Qam4; lo.rate = Rate::R04;
        Profile hi; hi.map = Mapping::Qam64; hi.rate = Rate::R08;
        CHECK(std::fabs(netBitrate(Header::Pn945, lo) - 4.8128e6) < 1e3, "4QAM 0.4 PN945 %f", netBitrate(Header::Pn945, lo));
        CHECK(std::fabs(netBitrate(Header::Pn420, hi) - 32.4864e6) < 1e3, "64QAM 0.8 PN420 %f", netBitrate(Header::Pn420, hi));
        Profile q32; q32.map = Mapping::Qam32; q32.rate = Rate::R08;
        CHECK(packetsPerFrame(q32) == 10, "32QAM packets per frame");
        Profile nr; nr.map = Mapping::Qam4Nr; nr.rate = Rate::R08;
        CHECK(packetsPerFrame(nr) == 2, "NR packets per frame");
        // packets per frame of the transport stream for every profile: 4QAM 2/3/4, 16QAM 4/6/8, 64QAM 6/9/12
        const int e4[3] = {2, 3, 4}, e16[3] = {4, 6, 8}, e64[3] = {6, 9, 12};
        for (int r = 0; r < 3; r++) {
            Profile p; p.rate = (Rate)r;
            p.map = Mapping::Qam4; CHECK(packetsPerFrame(p) == e4[r], "4QAM");
            p.map = Mapping::Qam16; CHECK(packetsPerFrame(p) == e16[r], "16QAM");
            p.map = Mapping::Qam64; CHECK(packetsPerFrame(p) == e64[r], "64QAM");
        }
    }
    // Mapper and demapper: hard decisions of the soft demapper give the bits back, the table driven block version agrees with the direct one
    {
        std::mt19937 rng(4);
        for (Mapping m : {Mapping::Qam4, Mapping::Qam16, Mapping::Qam32, Mapping::Qam64}) {
            const int bps = bitsPerSymbol(m);
            const size_t n = 4000;
            std::vector<uint8_t> bits(n * (size_t)bps);
            for (auto& b : bits) b = (uint8_t)(rng() & 1);
            std::vector<cf32> sym(n);
            mapSymbols(m, bits.data(), n, sym.data());
            std::vector<float> var(n, 0.05f), llrB(n * (size_t)bps), llrS(bps);
            std::normal_distribution<float> nd(0.f, 0.05f);
            for (auto& s : sym) s += cf32(nd(rng), nd(rng));
            demapBlock(m, sym.data(), var.data(), n, llrB.data());
            int wrong = 0, differ = 0;
            for (size_t i = 0; i < n; i++) {
                demapSymbol(m, sym[i], var[i], llrS.data());
                for (int b = 0; b < bps; b++) {
                    wrong += (llrB[i * (size_t)bps + (size_t)b] < 0) != (bits[i * (size_t)bps + (size_t)b] != 0);
                    if (std::fabs(llrB[i * (size_t)bps + (size_t)b] - llrS[(size_t)b]) * var[i] > 0.03f) differ++;   // error in squared distance
                }
            }
            CHECK(wrong < (int)(n * (size_t)bps / 100), "%s: %d bit errors at 25 dB", mappingName(m), wrong);
            CHECK(differ < (int)(n * (size_t)bps / 200), "%s: table demapper differs from the direct one in %d of %d LLRs", mappingName(m), differ, (int)(n * (size_t)bps));
        }
    }
    printf(failures ? "dtmb_defs: %d FAILED\n" : "dtmb_defs: all passed\n", failures);
    return failures ? 1 : 0;
}
