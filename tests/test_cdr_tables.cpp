// CDR tables and building blocks against values printed in GY/T 268.1-2013 / GY/T 268.2-2013 (as published by the NRTA) and against
// independent implementations, so that the generator and the receiver are not only checked against each other. Quick.
#include "dect2/cdr_defs.h"
#include "dect2/cdr_ldpc.h"
#include "dect2/cdr_mux.h"
#include "dect2/dab.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::cdr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool range(const std::vector<int>& v, int lo, int hi) {
    if ((int)v.size() != hi - lo + 1) return false;
    for (size_t i = 0; i < v.size(); i++) if (v[i] != lo + (int)i) return false;
    return true;
}
static bool near(cf32 a, double re, double im) { return std::fabs(a.real() - re) < 1e-5 && std::fabs(a.imag() - im) < 1e-5; }

int main() {
    // ---- Table 1: the sub-frame always lasts 130560 T; Ts = Tcp + Tu, TB = TBcp + Tu
    const int tu[3] = {2048, 1024, 2048}, tcp[3] = {240, 140, 56}, tbcp[3] = {384, 332, 168}, sn[3] = {56, 111, 61}, nv[3] = {242, 122, 242};
    for (int tm = 1; tm <= 3; tm++) {
        const TxParams& p = *txParams(tm);
        CHECK(p.tu == tu[tm - 1] && p.tcp == tcp[tm - 1] && p.tbcp == tbcp[tm - 1] && p.sn == sn[tm - 1] && p.nv == nv[tm - 1], "Table 1 mode %d", tm);
        CHECK(p.ts == p.tcp + p.tu && p.tb == p.tbcp + p.tu && p.tb + p.sn * p.ts == kSubframeLen, "sub-frame length mode %d", tm);
        CHECK(std::fabs(p.df - kFs / p.ns) < 1e-9 && std::fabs(p.dfb - kFs / p.nb) < 1e-9, "carrier spacing mode %d", tm);
    }
    // ---- Annex C, Tables C.1 to C.4
    CHECK(range(halfCarriers(1, false, 5, true, false), 503, 623), "C.1 DB5(U)");
    CHECK(range(halfCarriers(1, false, 4, false, false), 129, 249), "C.1 DB4(L)");
    CHECK(range(halfCarriers(3, false, 3, false, false), -121, -1), "C.1 DB3(L)");
    CHECK(range(halfCarriers(1, false, 1, false, false), -623, -503), "C.1 DB1(L)");
    CHECK(range(halfCarriers(2, false, 5, true, false), 251, 311), "C.1 DB5(U) mode 2");
    CHECK(range(halfCarriers(1, true, 4, true, false), 377, 497), "C.2 DA4(U)");
    CHECK(range(halfCarriers(1, true, 3, false, false), 5, 125), "C.2 DA3(L)");
    CHECK(range(halfCarriers(1, true, 2, true, false), -125, -5), "C.2 DA2(U)");
    CHECK(range(halfCarriers(2, true, 3, false, false), 1, 61), "C.2 DA3(L) mode 2");
    CHECK(range(halfCarriers(2, true, 1, false, false), -249, -189), "C.2 DA1(L) mode 2");
    CHECK(range(halfCarriers(1, false, 5, false, true), 191, 250), "C.3 DB5(L)");
    CHECK(range(halfCarriers(1, false, 2, false, true), -185, -126), "C.3 DB2(L)");
    CHECK(range(halfCarriers(2, false, 4, true, true), 64, 93), "C.3 DB4(U) mode 2");
    CHECK(range(halfCarriers(1, true, 4, true, true), 189, 248), "C.4 A4 upper");
    CHECK(range(halfCarriers(1, true, 2, false, true), -123, -64), "C.4 A2 lower");
    CHECK(range(halfCarriers(2, true, 3, true, true), 32, 61), "C.4 A3 upper mode 2");
    // ---- layouts: Tables 9 to 12, A.1, A.2 and the pilot count of 5.5 for every mode
    const int p14[3] = {23040, 23040, 25344}, p34_64[3] = {207360, 207360, 228096}, q[3][3] = {{846, 1698, 2550}, {782, 1570, 2358}, {674, 1354, 2034}};
    for (int tm = 1; tm <= 3; tm++)
        for (int sm : spectrumModeIndices()) {
            const auto L = layoutFor(tm, sm);
            CHECK(L != nullptr, "layout %d/%d", tm, sm);
            if (!L) continue;
            const int ni = L->ni;
            CHECK((int)L->msdsPos.size() == (tm == 3 ? 50688 : 46080) * ni, "Table 12 MSDS %d/%d", tm, sm);
            CHECK((int)L->sdisPos.size() == (tm == 1 ? 1704 : tm == 2 ? 1576 : 1360) * ni, "Table 12 SDIS %d/%d", tm, sm);
            CHECK(L->msdBits(kQpsk, 0) == p14[tm - 1] * ni && L->msdBits(k64Qam, 3) == p34_64[tm - 1] * ni, "Table A.1 %d/%d", tm, sm);
            for (int m = 0; m < 3; m++) CHECK(L->sdiBits(m) == (q[tm - 1][m] + 6) * ni - 6, "Table A.2 %d/%d", tm, sm);
            CHECK((int)L->syncCarrier.size() == (tm == 2 ? 60 : 120) * ni, "Table 13 L %d/%d", tm, sm);
            int pilots = 0;
            for (int e = 0; e < 3 * L->cols; e++) pilots += L->kind[(size_t)e] == kElemPilot;
            CHECK(pilots == (tm == 2 ? 32 : 62) * ni, "pl %d/%d: %d", tm, sm, pilots);
            std::set<int> used(L->sdisPos.begin(), L->sdisPos.end());
            for (int p : L->msdsPos) used.insert(p);
            CHECK(used.size() == L->sdisPos.size() + L->msdsPos.size(), "SDIS and MSDS cells overlap %d/%d", tm, sm);
            // Table 9: SI symbol columns of the first sub-band
            const int c0 = tm == 2 ? 14 : 10, c1 = tm == 2 ? 83 : 143;
            CHECK(L->kind[(size_t)c0] == kElemSi && L->kind[(size_t)c1] == kElemSi && L->siSym[(size_t)c0] == 0, "Table 9 %d/%d", tm, sm);
            // Table 10: the SI symbols of row 1 come back in the row after the first siRows
            CHECK(L->siSym[(size_t)(L->tp->siRows * L->cols + c0)] == 0, "Table 10 %d/%d", tm, sm);
            // 5.6.2: the pilots of row 1 sit on columns 12p + 121 and 12p + 122 (modes 1, 3) or 12p + 61 / 62 (mode 2)
            CHECK(L->kind[0] == kElemPilot && L->kind[(size_t)(tm == 2 ? 60 : 120)] == kElemPilot && L->kind[(size_t)(tm == 2 ? 61 : 121)] == kElemPilot, "5.6.2 %d/%d", tm, sm);
            CHECK(std::abs(L->beaconSeq[0] - cf32(1, 0)) < 1e-6f, "Pb(0)");
        }
    // 5.9.1: Pb(1) = exp(+j 2 pi m / Nzc) (n = 1 is odd), Pb(2) = exp(-j 2 pi m 3 / Nzc)
    {
        const auto L = layoutFor(1, 1);
        const double a1 = 2 * M_PI * 48 / 967.0, a2 = -2 * M_PI * 48 * 3 / 967.0;
        CHECK(near(L->beaconSeq[1], std::cos(a1), std::sin(a1)) && near(L->beaconSeq[2], std::cos(a2), std::sin(a2)), "Pb(1), Pb(2)");
        CHECK(L->carrier.front() == -121 && L->carrier.back() == 121 && L->syncCarrier.front() == -60, "spectrum mode 1 carriers");
        const auto L9 = layoutFor(1, 9);
        CHECK(L9->carrier.front() == -497 && L9->carrier[120] == -377 && L9->carrier[121] == 377, "spectrum mode 9: DA1(L) and DA4(U)");
        const auto L22 = layoutFor(1, 22);
        CHECK(L22->carrier.front() == -371 && L22->carrier.back() == 371, "spectrum mode 22: DB2(L) and DB4(U)");
    }
    // ---- 5.4.1 constellations (Figures 8 to 10)
    {
        const uint8_t q10[2] = {1, 0}, q01[2] = {0, 1};
        CHECK(near(mapBits(q10, kQpsk), -M_SQRT1_2, M_SQRT1_2) && near(mapBits(q01, kQpsk, (float)std::sqrt(2.0)), 1, -1), "QPSK");
        const uint8_t a[4] = {0, 0, 0, 0}, b[4] = {1, 1, 0, 1}, c[4] = {0, 1, 1, 1};
        const double s10 = 1 / std::sqrt(10.0), s42 = 1 / std::sqrt(42.0);
        CHECK(near(mapBits(a, k16Qam), 3 * s10, 3 * s10) && near(mapBits(b, k16Qam), -3 * s10, -1 * s10) && near(mapBits(c, k16Qam), s10, -s10), "16QAM");
        const uint8_t d[6] = {0, 0, 0, 0, 0, 0}, e[6] = {1, 1, 0, 1, 0, 0}, f[6] = {0, 1, 1, 0, 1, 1}, g[6] = {1, 0, 1, 1, 1, 1};
        CHECK(near(mapBits(d, k64Qam), 7 * s42, 7 * s42) && near(mapBits(e, k64Qam), -7 * s42, -1 * s42) && near(mapBits(f, k64Qam), 3 * s42, -5 * s42) &&
              near(mapBits(g, k64Qam), -3 * s42, 3 * s42), "64QAM");
        // the demapper inverts the mapper
        std::mt19937 rng(3);
        for (int mod = 0; mod < 3; mod++)
            for (int t = 0; t < 200; t++) {
                uint8_t bits[6];
                for (auto& x : bits) x = rng() & 1;
                float llr[6];
                demap(mapBits(bits, mod), 0.01f, mod, llr);
                for (int k = 0; k < modBits(mod); k++) CHECK((llr[k] < 0) == (bits[k] == 1), "demap %d", mod);
            }
    }
    // ---- 5.3.1 interleaver: p(0) = 0, p(i) = (5 p(i-1) + g) mod s, s = 256 and g = 63 for the 216 SI bits
    {
        const std::vector<int>& R = interleaver(216);
        CHECK(R.size() == 216 && R[0] == 0 && R[1] == 63 && R[2] == 122 && R[3] == (5 * 122 + 63) % 256, "SI interleaver");
        std::set<int> s(R.begin(), R.end());
        CHECK(s.size() == 216 && *s.rbegin() == 215, "permutation");
        const std::vector<int>& R2 = interleaver(46080);
        CHECK(R2.size() == 46080 && R2[1] == 16383, "MSDS interleaver (s = 65536, g = 16383)");
    }
    // ---- 5.1 PRBS: the 1 of the initial state 100000000000 leaves the last stage after 11 shifts
    {
        Prbs p;
        int first = -1;
        for (int i = 0; i < 12; i++) if (p.next() && first < 0) first = i;
        CHECK(first == 11, "PRBS start %d", first);
    }
    // ---- 5.2.1 convolutional code against the DAB encoder (EN 300 401: the same generators 133 171 145 133), and the decoder
    {
        std::mt19937 rng(5);
        std::vector<uint8_t> in(300), a(4 * 306), b(4 * 306), out(300);
        for (auto& x : in) x = rng() & 1;
        convEncode(in.data(), 300, a.data());
        dab::convEncode(in.data(), 300, b.data());
        CHECK(a == b, "conv code differs from the DAB encoder");
        std::vector<float> llr(a.size());
        for (size_t i = 0; i < a.size(); i++) llr[i] = (a[i] ? -1.f : 1.f) + (i % 7 == 0 ? (a[i] ? 1.6f : -1.6f) : 0.f);   // every 7th bit flipped
        convDecode(llr.data(), 300, out.data());
        CHECK(out == in, "Viterbi");
    }
    // ---- 4.6 system information with CRC-6 (initial state all ones)
    {
        SysInfo s;
        s.spec = 23; s.nominal = 3; s.frame = 2; s.subframe = 1; s.alloc = 3; s.sdiMod = 1; s.msdMod = 2; s.rateHi = 1;
        uint8_t b[48];
        siToBits(s, b);
        CHECK(b[0] == 1 && b[1] == 1 && b[9] == 1, "b0..b9 when not cooperating");
        CHECK(b[13] == 0 && b[14] == 1 && b[15] == 0 && b[16] == 1 && b[17] == 1 && b[18] == 1, "Table 4: 23 = 010111");
        SysInfo r;
        CHECK(siFromBits(b, r) && r.spec == 23 && r.frame == 2 && r.subframe == 1 && r.alloc == 3 && r.sdiMod == 1 && r.msdMod == 2 && r.rateHi == 1, "SI round trip");
        b[20] ^= 1;
        CHECK(!siFromBits(b, r), "CRC-6 misses an error");
        // an independent CRC-6: polynomial division of (b0..b41 with the first six inverted) x^6 by x^6+x^5+x^3+x^2+x+1
        uint8_t d[48] = {};
        siToBits(s, d);
        std::vector<int> m(48, 0);
        for (int i = 0; i < 42; i++) m[(size_t)i] = d[i] ^ (i < 6 ? 1 : 0);
        const int g[7] = {1, 1, 0, 1, 1, 1, 1};   // x^6 .. x^0
        for (int i = 0; i < 42; i++) if (m[(size_t)i]) for (int k = 0; k < 7; k++) m[(size_t)(i + k)] ^= g[k];
        bool same = true;
        for (int i = 0; i < 6; i++) same = same && m[(size_t)(42 + i)] == d[42 + i];
        CHECK(same, "CRC-6 differs from the long division");
    }
    // ---- sub-frame allocation, Figures 25 and 26
    {
        const int f2[16][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}, {0, 3}, {1, 3}, {2, 0}, {3, 0}, {2, 1}, {3, 1}, {2, 2}, {3, 2}, {2, 3}, {3, 3}};
        bool ok2 = true, ok3 = true, ok1 = true;
        for (int i = 0; i < 16; i++) {
            int p, qq;
            physToLogical(2, i / 4, i % 4, p, qq); ok2 = ok2 && p == f2[i][0] && qq == f2[i][1];
            physToLogical(3, i / 4, i % 4, p, qq); ok3 = ok3 && p == i % 4 && qq == i / 4;
            physToLogical(1, i / 4, i % 4, p, qq); ok1 = ok1 && p == i / 4 && qq == i % 4;
        }
        CHECK(ok1 && ok2 && ok3, "allocation");
    }
    // ---- LDPC: rows as printed in Annex D, encoder and decoder
    {
        CHECK(cdrLdpc(0).row(0) == std::vector<int>({2755, 3458, 3798, 4650}), "D.1 row 0");
        CHECK(cdrLdpc(0).row(61) == std::vector<int>({2560, 3519, 3603, 4711}), "D.1 row 61");
        CHECK(cdrLdpc(0).row(3000) == std::vector<int>({163, 313, 824, 3896}), "D.1 row 3000");
        CHECK(cdrLdpc(1).row(0) == std::vector<int>({320, 2062, 2827, 7581, 8339}), "D.2 row 0");
        CHECK(cdrLdpc(1).row(3002) == std::vector<int>({589, 3101, 6953, 8596, 9139}), "D.2 row 3002");
        CHECK(cdrLdpc(2).row(2430) == std::vector<int>({144, 2430, 3291, 3714, 7038, 7294}), "D.3 row 2430");
        CHECK(cdrLdpc(2).row(2432) == std::vector<int>({146, 2432, 3293, 7040, 7296}), "D.3 row 2432 (printed with five columns)");
        CHECK(cdrLdpc(2).row(3000) == std::vector<int>({942, 1186, 3567, 3803, 7608, 7864}), "D.3 row 3000");
        CHECK(cdrLdpc(3).row(1616) == std::vector<int>({66, 583, 844, 1862, 2098, 2891, 3147, 3846, 4362, 4947, 5655, 5911, 6679, 8528, 8784}), "D.4 row 1616");
        CHECK(cdrLdpc(3).row(1632) == std::vector<int>({82, 599, 860, 1878, 2114, 2907, 3163, 3390, 3862, 4378, 4963, 5671, 5927, 6695, 8544, 8800}), "D.4 row 1632");
        std::mt19937 rng(9);
        for (int r = 0; r < 4; r++) {
            const CdrLdpc& c = cdrLdpc(r);
            CHECK(c.checks() == 9216 - ldpcInfoBits(r) && c.encoderReady(), "rate %s", ldpcRateText(r));
            std::vector<uint8_t> info((size_t)c.k()), w(9216), out((size_t)c.k());
            for (auto& x : info) x = rng() & 1;
            c.encode(info.data(), w.data());
            CHECK(c.syndromeWeight(w.data()) == 0, "encoder rate %s", ldpcRateText(r));
            CHECK(std::equal(info.begin(), info.end(), w.begin()), "systematic");
            std::vector<float> llr(9216);
            std::normal_distribution<float> nd(0, 0.5f);
            for (int i = 0; i < 9216; i++) llr[(size_t)i] = 8.f * ((w[(size_t)i] ? -1.f : 1.f) + nd(rng));
            const CdrLdpc::Result res = c.decode(llr.data(), out.data());
            CHECK(res.ok && out == info, "decoder rate %s", ldpcRateText(r));
        }
    }
    // ---- GY/T 268.2: CRCs (Annex C: MSB first, all-ones start, inverted: the CRC-32/BZIP2 and inverted CRC-8/NRSC-5 check values)
    {
        const uint8_t s[] = "123456789";
        CHECK(crc32(s, 9) == 0xFC891918u, "CRC-32 %08X", crc32(s, 9));
        CHECK(crc8(s, 9) == (uint8_t)~0xF7, "CRC-8 %02X", crc8(s, 9));
    }
    // ---- GY/T 268.2 tables: round trips, the country code example of 6.3.2 and the CRC protection
    {
        Smct t;
        SmctEntry e;
        e.smfId = 1; e.txMode = 0xF; e.services = {0x1001, 0x1002, 0x1003};
        t.frames = {e};
        Nit n;
        n.networkId = 0x123456789; n.freqs = {10610000}; n.name = {'O', 'n', 'A', 'i', 'r'};
        n.neighbours = {{0x55, {9390000, 9400000}}};
        const std::vector<uint8_t> sb = smctBytes(t), nb = nitBytes(n);
        CHECK(sb.size() == 6 + 2 + 6 + 2 + 4 && ((sb[1] << 8) | sb[2]) == (int)sb.size() - 4, "SMCT size");
        CHECK(nb[5] == 0x43 && nb[6] == 0x48 && nb[7] == 0x4E, "CHN = 0100 0011 0100 1000 0100 1110");
        const std::vector<uint8_t> cf = controlFrameBytes({sb, nb});
        ControlFrame c;
        CHECK(parseControlFrame(cf.data(), cf.size(), c) && c.haveSmct && c.haveNit && c.tablesOk == 2, "control frame");
        CHECK(c.smct.frames.size() == 1 && c.smct.frames[0].services == e.services && c.smct.frames[0].txMode == 0xF, "SMCT");
        CHECK(c.nit.networkId == 0x123456789 && c.nit.freqs == n.freqs && c.nit.name == n.name && c.nit.country == "CHN" && c.nit.neighbours.size() == 1 &&
              c.nit.neighbours[0].freqs.size() == 2, "NIT");
        std::vector<uint8_t> bad = cf;
        bad[cf.size() - 10] ^= 4;
        ControlFrame c2;
        CHECK(parseControlFrame(bad.data(), bad.size(), c2) && c2.tablesBad == 1 && c2.tablesOk == 1, "NIT CRC");
        MuxSubFrame s;
        s.startTime = 14400; s.hasAudio = true; s.hasExt = true; s.hasData = true;
        AudioStreamDesc st;
        st.algo = 0; st.rate100 = 480; st.sampleRateCode = 7; st.channelsCode = 2; st.language = "chi";
        s.streams = {st};
        s.audio = {{0, 0, std::vector<uint8_t>(100, 7)}, {0, 480, std::vector<uint8_t>(101, 9)}};
        s.data = {{160, {'h', 'i'}}};
        const std::vector<uint8_t> sub = subFrameBytes(s);
        ServiceFrameHeader h;
        h.smfId = 3;
        const std::vector<uint8_t> f = serviceFrameBytes(h, {sub, sub}, 1000);
        ServiceFrame out;
        CHECK(f.size() == 1000 && parseServiceFrame(f.data(), f.size(), out) && out.h.smfId == 3 && out.subs.size() == 2, "service frame");
        if (out.subs.size() == 2) {
            const ParsedSubFrame& p = out.subs[1];
            CHECK(p.headerOk && p.audioOk && p.dataOk && p.sf.startTime == 14400 && p.sf.streams.size() == 1 && p.sf.streams[0].rate100 == 480 &&
                  p.sf.streams[0].sampleRateCode == 7 && p.sf.streams[0].language == "chi" && p.sf.audio.size() == 2 && p.sf.audio[1].relTime == 480 &&
                  p.sf.audio[1].data.size() == 101 && p.sf.data.size() == 1 && printableText(p.sf.data[0].data) == "hi", "sub-frame");
        }
        CHECK(sampleRateHz(7) == 48000 && sampleRateHz(3) == 22050 && std::string(dataUnitTypeText(160)) == "data broadcast", "Tables 9, 12");
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
