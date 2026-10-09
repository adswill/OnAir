// DVB-S2X (EN 302 307-2): the MODCOD table and its codes, the constellations, the PLS code, a code word round trip through every S2X MODCOD, and
// generator -> receiver round trips for a spread of them (8APSK to 256APSK, normal and short frames) through noise and a carrier offset, where every
// transport stream packet has to arrive intact and no frame may fail the BCH decoder.
#include "dect2/dvbs_s2.h"
#include "dect2/dvbs_testkit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dvbs;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the number of points on each ring of a constellation, smallest ring first
static std::vector<int> ringSizes(const cf32* c, int n) {
    std::map<int, int> m;
    for (int i = 0; i < n; i++) m[(int)std::lround(std::abs(c[i]) * 1000.0 / 5.0)]++;    // rings apart by more than 0.005
    std::vector<int> v;
    for (auto& e : m) v.push_back(e.second);
    return v;
}

static std::string name(int mod, int rate, bool sh) {
    return std::string("S2X ") + s2ModNameFor(mod, rate) + " " + s2RateName(rate) + (sh ? " short" : "");
}

int main() {
    std::mt19937 rng(777);

    // ---- table 17a: 38 normal and 17 short MODCODs, their PLS code values, dimensions (tables 4, 6, 8a, 8b, 16)
    {
        int normal = 0, shortN = 0;
        for (int r = kS2Rates; r < kS2Rates + kS2xModcods; r++) {
            int mod = -1;
            for (int m = 0; m < kS2Mods; m++) if (s2Modcod(m, r) >= 0) mod = m;
            CHECK(mod >= 0, "rate %d belongs to no modulation", r);
            if (mod < 0) continue;
            const bool sh = s2Dims(mod, r, true).ok;
            CHECK(sh != s2Dims(mod, r, false).ok, "%s: exactly one frame size", name(mod, r, sh).c_str());
            (sh ? shortN : normal)++;
            const S2Dims d = s2Dims(mod, r, sh);
            CHECK(d.nldpc == (sh ? 16200 : 64800), "%s: nldpc %d", name(mod, r, sh).c_str(), d.nldpc);
            CHECK(d.slots * 90 == d.xfecSymbols, "%s: whole slots", name(mod, r, sh).c_str());
            const int mc = s2Modcod(mod, r);
            int m2, r2;
            CHECK(s2ModcodIsS2x(mc) && s2ModcodSplit(mc, m2, r2) && m2 == mod && r2 == r, "%s: MODCOD %d does not split back", name(mod, r, sh).c_str(), mc);
            CHECK(s2PlsKind(mc << 1) == 0 && s2PlsKind(mc << 1 | 1) == 0, "%s: PLS code %d", name(mod, r, sh).c_str(), mc << 1);
            CHECK(s2RateFromName(mod, s2RateName(r), sh) == r, "%s: found by its name", name(mod, r, sh).c_str());
            CHECK(s2QefEsN0(mod, r, sh) < 30, "%s: no Es/N0", name(mod, r, sh).c_str());
        }
        CHECK(normal == 38 && shortN == 17, "38 normal and 17 short S2X MODCODs, got %d and %d", normal, shortN);
        int mod, rate;
        CHECK(s2ModcodSplit(132 >> 1, mod, rate) && mod == kQpsk && std::string(s2RateName(rate)) == "13/45", "PLS 132 is QPSK 13/45");
        CHECK(s2ModcodSplit(138 >> 1, mod, rate) && mod == k8psk && std::string(s2ModNameFor(mod, rate)) == "8APSK" && std::string(s2RateName(rate)) == "5/9-L", "PLS 138 is 8APSK 5/9-L");
        CHECK(s2ModcodSplit(194 >> 1, mod, rate) && mod == k64apsk && std::string(s2RateName(rate)) == "4/5" && std::string(s2xCodeName(rate)) == "4/5", "PLS 194 is 64APSK 4/5 with the S2 code");
        CHECK(s2ModcodSplit(248 >> 1, mod, rate) && mod == k32apsk && s2Dims(mod, rate, true).ok, "PLS 248 is 32APSK 32/45 short");
        CHECK(!s2ModcodSplit(176 >> 1, mod, rate), "PLS 176 is reserved");
        // table 4: Kbch of 13/45 and 140/180; table 8a: q
        const int r1345 = s2xRate(kQpsk, false, 0), r77 = s2xRate(k16apsk, false, 12);
        CHECK(s2Dims(kQpsk, r1345, false).kbch == 18528 && s2Dims(kQpsk, r1345, false).q == 128, "13/45: Kbch 18528, q 128");
        CHECK(std::string(s2RateName(r77)) == "77/90" && s2Dims(k16apsk, r77, false).kbch == 55248 && s2Dims(k16apsk, r77, false).q == 26, "154/180: Kbch 55248, q 26");
        const int r1145 = s2xRate(kQpsk, true, 0);
        CHECK(s2Dims(kQpsk, r1145, true).kbch == 3792 && s2Dims(kQpsk, r1145, true).q == 34, "short 11/45: Kbch 3792, q 34");
        // table 16: slots; 128APSK: 103 slots of 9 270 symbols with its padding
        const int r128 = s2xRate(k128apsk, false, 0), r64 = s2xRate(k64apsk, false, 0), r256 = s2xRate(k256apsk, false, 0);
        CHECK(s2Dims(k128apsk, r128, false).slots == 103 && s2Dims(k64apsk, r64, false).slots == 120 && s2Dims(k256apsk, r256, false).slots == 90, "slots of 64, 128 and 256APSK");
        // PLS code values: 110 S2X data codes, 18 followed but not decoded (VL-SNR and table 17b), with the lengths of table 17b
        int kinds[3] = {0, 0, 0};
        for (int c = 128; c < 256; c++) if (s2PlsKind(c) >= 0) kinds[s2PlsKind(c)]++;
        CHECK(kinds[0] == 110 && kinds[1] == 0 && kinds[2] == 18, "S2X PLS codes: %d data, %d dummy, %d other", kinds[0], kinds[1], kinds[2]);
        CHECK(s2PlsFrameSymbols(128) == 21690 && s2PlsFrameSymbols(177) == 13338 && s2PlsFrameSymbols(255) == 6714 && s2PlsFrameSymbols(129) == 33282, "table 17b lengths");
        CHECK(s2PlsFrameSymbols(200) == 90 * 104 && s2PlsFrameSymbols(201) == 90 * 104 + 36 * 6, "128APSK frame length");
        CHECK(s2PlsKind(0) == 1 && s2PlsKind(29 << 2) < 0 && s2PlsKind((11 << 2) | 2) < 0, "S2 dummy, reserved MODCOD 29, short 9/10");
    }

    // ---- constellations: unit energy, distinct points, the rings of each shape
    {
        struct Ring { int pls; std::vector<int> sizes; };
        const Ring rings[] = {
            {138, {2, 4, 2}}, {148, {8, 8}}, {158, {8, 8}}, {154, {4, 12}}, {174, {4, 12, 16}}, {178, {4, 8, 4, 16}}, {184, {16, 16, 16, 16}},
            {190, {8, 16, 20, 20}}, {186, {4, 12, 20, 28}}, {200, {16, 16, 16, 16, 16, 48}}, {210, {32, 32, 32, 32, 32, 32, 32, 32}},
        };
        for (const Ring& rg : rings) {
            int mod, rate;
            s2ModcodSplit(rg.pls >> 1, mod, rate);
            const cf32* c = s2Constellation(mod, rate);
            const int n = s2ConstellationSize(mod);
            const std::vector<int> v = ringSizes(c, n);
            std::string got;
            for (int x : v) got += std::to_string(x) + " ";
            CHECK(v == rg.sizes, "PLS %d %s: rings %s", rg.pls, name(mod, rate, false).c_str(), got.c_str());
        }
        for (int r = kS2Rates; r < kS2Rates + kS2xModcods; r++) {
            int mod = 0;
            for (int m = 0; m < kS2Mods; m++) if (s2Modcod(m, r) >= 0) mod = m;
            const cf32* c = s2Constellation(mod, r);
            const int n = s2ConstellationSize(mod);
            double e = 0, dmin = 1e9;
            for (int i = 0; i < n; i++) {
                e += std::norm(c[i]);
                for (int j = i + 1; j < n; j++) dmin = std::min(dmin, (double)std::abs(c[i] - c[j]));
            }
            const bool sh = s2Dims(mod, r, true).ok;
            CHECK(std::fabs(e / n - 1.0) < 2e-3, "%s: mean energy %.4f", name(mod, r, sh).c_str(), e / n);
            // (table 15d puts a few pairs of points almost on top of each other, 0.002 apart: no two may be equal)
            CHECK(dmin > 1e-4, "%s: two points %.5f apart", name(mod, r, sh).c_str(), dmin);
        }
        // 8+8APSK 18/30 (table 11e) and 256APSK 20/30 (table 15d): first points
        int mod, rate;
        s2ModcodSplit(158 >> 1, mod, rate);
        CHECK(std::abs(s2Constellation(mod, rate)[9] - cf32(0.4984f, 1.2088f)) < 1e-3, "table 11e label 1001");
        s2ModcodSplit(206 >> 1, mod, rate);
        CHECK(std::abs(s2Constellation(mod, rate)[0] - cf32(1.6350f, 0.1593f)) < 1e-3, "table 15d label 00000000");
    }

    // ---- PLS: every code word decodes to itself, S2X headers are turned by 90 degrees against S2 ones
    {
        int bad = 0;
        for (int c = 0; c < 256; c++) {
            if (s2PlsKind(c) < 0) continue;
            int mc; bool sh, pil;
            s2PlsSplit(c, mc, sh, pil);
            CHECK(s2PlsValue(mc, sh, pil) == c, "PLS %d: value", c);
            cf32 h[90];
            s2PlHeader(mc, sh, pil, h);
            const PlsResult r = s2PlsDecode(&h[26]);
            if (r.code != c || r.score < 0.999f || r.second > 0.6f) bad++;
        }
        CHECK(bad == 0, "%d PLS code words do not decode to themselves", bad);
        cf32 a[90], b[90];
        s2PlHeader(18, false, false, a);              // 16APSK 2/3
        s2PlHeader(66, false, false, b);              // QPSK 13/45
        float s = 0;
        for (int k = 26; k < 90; k++) s += (a[k] * std::conj(b[k])).real();
        CHECK(std::fabs(s) < 1e-3, "S2X and S2 PLS code words are orthogonal (%.3f)", s);
        for (int k = 0; k < 26; k++) CHECK(a[k] == b[k], "SOF the same");
    }

    // ---- every S2X MODCOD: BCH + LDPC encode, interleave, map, AWGN at 1.5 dB above tables 20a / 20c, demap, deinterleave, decode
    {
        for (int r = kS2Rates; r < kS2Rates + kS2xModcods; r++) {
            int mod = 0;
            for (int m = 0; m < kS2Mods; m++) if (s2Modcod(m, r) >= 0) mod = m;
            const bool sh = s2Dims(mod, r, true).ok;
            const S2Dims d = s2Dims(mod, r, sh);
            std::vector<uint8_t> bb(d.kbch), fec, il;
            for (auto& b : bb) b = rng() & 1;
            s2EncodeFec(bb, r, sh, fec);
            // the code word satisfies every parity check
            const LdpcCode& code = s2Ldpc(r, sh);
            int unsat = 0;
            for (size_t c = 0; c + 1 < code.chkStart().size(); c++) {
                int x = 0;
                for (int j = code.chkStart()[c]; j < code.chkStart()[c + 1]; j++) x ^= fec[(size_t)code.chkVar()[(size_t)j]];
                unsat += x;
            }
            CHECK(unsat == 0 && (int)fec.size() == d.nldpc, "%s: %d parity checks fail on the encoder output", name(mod, r, sh).c_str(), unsat);
            s2BitInterleave(fec, mod, r, il);
            CHECK((int)il.size() == d.xfecSymbols * d.bitsPerSym, "%s: XFECFRAME of %zu bits", name(mod, r, sh).c_str(), il.size());
            std::vector<cf32> sym((size_t)d.xfecSymbols);
            s2MapBits(il.data(), (int)il.size(), mod, r, sym.data());
            const double esn0 = s2QefEsN0(mod, r, sh) + 1.5;
            const float sigma2 = (float)(0.5 * std::pow(10.0, -esn0 / 10.0));
            std::normal_distribution<float> nd(0.f, std::sqrt(sigma2));
            for (auto& s : sym) s += cf32(nd(rng), nd(rng));
            std::vector<float> llr((size_t)d.xfecSymbols * d.bitsPerSym), dl((size_t)d.nldpc);
            s2Demap(sym.data(), d.xfecSymbols, mod, r, sigma2, llr.data());
            s2BitDeinterleaveLlr(llr.data(), d.nldpc, mod, r, dl.data());
            std::vector<uint8_t> hard, out;
            int iters = 0;
            const bool ok = code.decodeFast(dl, 50, hard, &iters, s2LdpcAlpha(r));
            const int fixed = ok ? s2BchDecode(hard.data(), r, sh, out) : -1;
            CHECK(ok && fixed >= 0 && out == bb, "%s: frame at %.2f dB did not decode (LDPC %s after %d iterations)", name(mod, r, sh).c_str(), esn0, ok ? "ok" : "failed", iters);
        }
        printf("code word round trips through all %d S2X MODCODs\n", kS2xModcods);
    }

    // ---- generator -> receiver: 2 Msym/s at 4 Msps, 8 bit samples, 3 dB above the Es/N0 of tables 20a / 20c, 25 kHz carrier offset
    struct Case { int pls; bool pilots; };
    const Case cases[] = {{132, false}, {140, false}, {232, true}, {150, true}, {236, false}, {180, false}, {246, true}, {194, false}, {186, true}, {200, false},
                          {210, false}, {212, true}};
    // S2X_PLS=<code>: run only that case (and S2X_CFO=<Hz> to change the carrier offset), for looking into one
    const int only = std::getenv("S2X_PLS") ? std::atoi(std::getenv("S2X_PLS")) : -1;
    const double cfo = std::getenv("S2X_CFO") ? std::atof(std::getenv("S2X_CFO")) : 25e3;
    for (const Case& cs : cases) {
        if (only >= 0 && cs.pls != only) continue;
        int mod, rate;
        if (!s2ModcodSplit(cs.pls >> 1, mod, rate)) { CHECK(false, "PLS %d", cs.pls); continue; }
        const bool sh = s2Dims(mod, rate, true).ok;
        RunConfig rc = makeRun(3, mod, rate, 2e6, 4e6, s2QefEsN0(mod, rate, sh) + 3.0, 3.0);
        rc.sig.tx.shortFrame = sh;
        rc.sig.tx.pilots = cs.pilots;
        rc.sig.cfoHz = cfo;
        const RunResult r = runCase(rc);
        std::string what = name(mod, rate, sh) + (cs.pilots ? " pilots" : "");
        const double perSec = dvbsNetBitrate(rc.sig.tx) / 1504.0;
        CHECK(r.good >= perSec * 1.6, "%s: only %llu good packets (wanted %.0f); %s", what.c_str(), (unsigned long long)r.good, perSec * 1.6, dvbsSummary(r.tel).c_str());
        CHECK(r.bad == 0 && r.gaps == 0, "%s: %llu damaged packets, %llu jumps in the counter", what.c_str(), (unsigned long long)r.bad, (unsigned long long)r.gaps);
        CHECK(r.tel.bchBad == 0, "%s: %llu frames failed the BCH (or LDPC) decoder", what.c_str(), (unsigned long long)r.tel.bchBad);
        const std::string shown = std::string(r.tel.standard == 3 ? "S2X " : "") + r.tel.modulationName + " " + r.tel.codeRate + (r.tel.frameSize == 2 ? " short" : "");
        CHECK(shown == name(mod, rate, sh) && r.tel.modcod == cs.pls && r.tel.pilots == cs.pilots, "%s: reported as %s (PLS %d)", what.c_str(), shown.c_str(), r.tel.modcod);
        if (r.good < perSec * 1.6 || r.tel.bchBad) for (size_t i = 0; i < r.log.size() && i < 12; i++) printf("    log: %s\n", r.log[i].c_str());
        printf("%s: %llu packets, first after %.2f s, Es/N0 %.1f dB, MER %.1f dB, LDPC iterations %.1f, %s\n", what.c_str(), (unsigned long long)r.good,
               r.firstPacketSecs, rc.sig.snrDb, r.tel.merDb, r.tel.ldpcIterAvg, dvbsSummary(r.tel).c_str());
    }

    printf(fails ? "dvbs s2x: FAILED (%d)\n" : "dvbs s2x: ok\n", fails);
    return fails ? 1 : 0;
}
