// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R physical layer: the convolutional code, the FIRE code, the 4-burst interleaver, the SCH, the System Information messages,
// and GMSK bursts through the equaliser (noise, echo, small frequency error, timing offset).
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/gsmr/gsmr_phy.h"
#include "dect2/gsmr/gsmr_si.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
using namespace dect2;
using namespace dect2::gsmr;
#include "../common/test_util.h"

static Bits randBits(std::mt19937& r, size_t n) { Bits b(n); for (auto& v : b) v = r() & 1; return b; }
static std::vector<float> llrOf(const Bits& b, float a, std::mt19937& r, float sigma) {
    dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<float> l(b.size());
    for (size_t i = 0; i < b.size(); i++) l[i] = (b[i] ? -a : a) + sigma * nd(r);
    return l;
}

int main() {
    std::mt19937 rng(9);
    // parity of nothing is all ones (the parity is inverted)
    {
        const Bits z(184, 0);
        const Bits p = fireParity(z.data());
        int ones = 0; for (auto v : p) ones += v;
        CHECK(ones == 40, "fire parity of zeros");
        const Bits z25(25, 0);
        int o2 = 0; for (auto v : schParity(z25.data())) o2 += v;
        CHECK(o2 == 10, "sch parity of zeros");
    }
    // convolutional code: noisy round trip
    {
        int bad = 0;
        for (int t = 0; t < 30; t++) {
            Bits u = randBits(rng, 224); u.insert(u.end(), 4, 0);
            const Bits c = convEncode(u);
            const auto l = llrOf(c, 1.0f, rng, 0.55f);
            const Bits d = convDecode(l.data(), u.size());
            if (d != u) bad++;
        }
        CHECK(bad == 0, "conv code failed %d / 30", bad);
    }
    // 23 byte frame: encode, interleave over four bursts, back, decode; a corrupted frame is rejected
    {
        uint8_t m[23], o[23];
        for (int i = 0; i < 23; i++) m[i] = (uint8_t)rng();
        const Bits c = xcchEncode(m);
        CHECK(c.size() == 456, "coded size");
        Bits b[4];
        xcchInterleave(c, b);
        float store[4][114];
        const float* bp[4];
        for (int k = 0; k < 4; k++) { for (int i = 0; i < 114; i++) store[k][i] = b[k][(size_t)i] ? -1.f : 1.f; bp[k] = store[k]; }
        float l[456];
        xcchDeinterleave(bp, l);
        CHECK(xcchDecode(l, o) && !memcmp(m, o, 23), "xcch round trip");
        // a burst lost entirely (erased) is still recovered by the code
        for (int i = 0; i < 114; i++) store[2][i] = 0;
        xcchDeinterleave(bp, l);
        CHECK(xcchDecode(l, o) && !memcmp(m, o, 23), "xcch with an erased burst");
        // too many errors: rejected by the FIRE code, not accepted as garbage
        int accepted = 0;
        for (int t = 0; t < 40; t++) {
            std::vector<float> n = llrOf(c, 1.0f, rng, 2.2f);
            uint8_t g[23];
            if (xcchDecode(n.data(), g) && memcmp(g, m, 23)) accepted++;
        }
        CHECK(accepted == 0, "wrong frame accepted %d times", accepted);
    }
    // Known answers that pin the bit order (the round trips below pass for any consistent convention): layer 2 octets go out least
    // significant bit first, so an SI3 starting 0x49 puts 1,0,0,1,0,0,1,0 on the air; the SCH octets are BSIC << 2 | T1 >> 9, T1 >> 1, ...
    {
        uint8_t m[23] = {0x49, 0x06, 0x1B};
        for (int i = 3; i < 23; i++) m[i] = 0x2B;
        std::vector<float> l;
        for (uint8_t b : xcchEncode(m)) l.push_back(b ? -1.f : 1.f);
        const Bits u = convDecode(l.data(), 228);
        const uint8_t want[16] = {1, 0, 0, 1, 0, 0, 1, 0,  0, 1, 1, 0, 0, 0, 0, 0};   // 0x49, 0x06 LSB first
        bool same = true; for (int i = 0; i < 16; i++) if (u[(size_t)i] != want[i]) same = false;
        CHECK(same, "layer 2 octets are not sent least significant bit first");
        // BSIC 0b001010 (NCC 1, BCC 2), FN 21 + 51 * 26 * 3: T1 = 3, T2 = (FN mod 26) = 21 + 51*26*3 mod 26 = 21, T3 = 21, T3' = 2
        const uint32_t fn = 21 + 51u * 26u * 3u;
        std::vector<float> s;
        for (uint8_t b : schEncode(10, fn)) s.push_back(b ? -1.f : 1.f);
        const Bits d = convDecode(s.data(), 39);
        const int t1 = 3, t2 = (int)(fn % 26), t3p = 2;
        const uint8_t sb[4] = {(uint8_t)(10 << 2 | t1 >> 9), (uint8_t)(t1 >> 1), (uint8_t)((t1 & 1) << 7 | t2 << 2 | t3p >> 1), (uint8_t)(t3p & 1)};
        bool sbSame = true; for (int i = 0; i < 25; i++) if (d[(size_t)i] != ((sb[i >> 3] >> (i & 7)) & 1)) sbSame = false;
        CHECK(sbSame, "SCH information bits");
        // the field positions of the gr-gsm decoder: NCC = d7 d6 d5, BCC = d4 d3 d2, T1 = d1 d0 d15 .. d8 d23, T2 = d22 .. d18, T3' = d17 d16 d24
        auto bit = [&](int i) { return (int)d[(size_t)i]; };
        const int ncc = bit(7) << 2 | bit(6) << 1 | bit(5), bcc = bit(4) << 2 | bit(3) << 1 | bit(2);
        int gt1 = bit(1) << 10 | bit(0) << 9; for (int i = 0; i < 8; i++) gt1 |= bit(15 - i) << (8 - i); gt1 |= bit(23);
        const int gt2 = bit(22) << 4 | bit(21) << 3 | bit(20) << 2 | bit(19) << 1 | bit(18), gt3p = bit(17) << 2 | bit(16) << 1 | bit(24);
        CHECK(ncc == 1 && bcc == 2 && gt1 == t1 && gt2 == t2 && gt3p == t3p, "SCH fields at the positions gr-gsm reads: %d %d %d %d %d", ncc, bcc, gt1, gt2, gt3p);
    }
    // SCH: BSIC and frame number for every SCH position of a multiframe, and large frame numbers
    {
        int bad = 0;
        for (int bsic : {0, 5, 21, 63})
            for (uint32_t fn : {1u, 11u, 21u, 31u, 41u, 52u, 1234567u, 26u * 51 * 2048 - 50u}) {
                if (fn % 51 % 10 != 1) fn = fn - fn % 51 + 1 + 10 * ((fn % 51) / 10 > 4 ? 0 : (fn % 51) / 10);   // a frame that carries the SCH
                const Bits c = schEncode(bsic, fn);
                const auto l = llrOf(c, 1.0f, rng, 0.6f);
                int b2; uint32_t f2;
                if (c.size() != 78 || !schDecode(l.data(), b2, f2) || b2 != bsic || f2 != fn) bad++;
            }
        CHECK(bad == 0, "sch round trips failed %d", bad);
    }
    // System Information messages
    {
        Si3 s;
        s.cellId = 54321; s.lai.mcc = 262; s.lai.mnc = 10; s.lai.mncDigits = 2; s.lai.lac = 0x1234;
        s.attach = false; s.agBlks = 2; s.ccchConf = 1; s.pagMfrms = 5; s.t3212 = 20; s.rlTimeout = 7; s.dtx = 1; s.pwrc = 1;
        s.reselHystDb = 6; s.msTxPwrMax = 3; s.rxLevAccessMin = 9; s.neci = true; s.acs = true; s.maxRetrans = 2; s.txInteger = 12; s.cellBar = true; s.acBarred = 0x0400;
        Bcch b;
        CHECK(parseBcch(encodeSi3(s).data(), b) && b.type == kSi3 && b.hasSi3 && b.si3.cellId == 54321 && b.si3.lai.mcc == 262 && b.si3.lai.mnc == 10 && b.si3.lai.mncDigits == 2 && b.si3.lai.lac == 0x1234, "SI3 identity");
        CHECK(!b.si3.attach && b.si3.agBlks == 2 && b.si3.ccchConf == 1 && b.si3.pagMfrms == 5 && b.si3.t3212 == 20 && b.si3.rlTimeout == 7 && b.si3.dtx == 1 && b.si3.pwrc == 1, "SI3 control channel and options");
        CHECK(b.si3.reselHystDb == 6 && b.si3.msTxPwrMax == 3 && b.si3.rxLevAccessMin == 9 && b.si3.neci && b.si3.acs && b.si3.maxRetrans == 2 && b.si3.txInteger == 12 && b.si3.cellBar && b.si3.acBarred == 0x0400, "SI3 selection and RACH");
        s.lai.mnc = 123; s.lai.mncDigits = 3;
        CHECK(parseBcch(encodeSi4(s).data(), b) && b.type == kSi4 && b.hasLai && !b.hasSi3 && b.si3.lai.mnc == 123 && b.si3.lai.mncDigits == 3, "SI4 three digit MNC");
        CHECK(parseBcch(encodeSi1({3, 17, 124, 1, 80}, s).data(), b) && b.type == kSi1 && b.arfcns == std::vector<int>({1, 3, 17, 80, 124}), "SI1 cell allocation");
        CHECK(parseBcch(encodeSi2({2, 9, 10, 100}, 0x81, s).data(), b) && b.type == kSi2 && b.arfcns == std::vector<int>({2, 9, 10, 100}) && b.nccPermitted == 0x81, "SI2 neighbours");
        CHECK(parseBcch(encodeSi2ter().data(), b) && b.type == kSi2ter, "SI2ter type");
        const auto idle = idleFrame();
        CHECK(!parseBcch(idle.data(), b), "idle frame is not a system information message");
        // 45.002 gives the SI3 of a common network as 49 06 1b: the header bytes
        const Frame23 f3 = encodeSi3(s);
        CHECK(f3[0] == 0x49 && f3[1] == 0x06 && f3[2] == 0x1B && encodeSi4(s)[0] == 0x31, "pseudo lengths");
    }
    // frequency lists: the range formats against the explicit formulas of 44.018 Annex J
    {
        auto smod = [](int n, int m) { return ((n - 1) % m + m) % m + 1; };
        struct Bw { uint8_t d[16] = {}; int pos = 0; void put(int v, int n) { for (int i = n - 1; i >= 0; i--, pos++) if ((v >> i) & 1) d[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7)); } };
        int bad = 0;
        for (int t = 0; t < 200; t++) {
            // range 1024
            {
                Bw b; b.d[0] = 0x80 | (t & 1 ? 0x04 : 0); b.pos = 6;
                int w[17] = {};
                for (int k = 1; k <= 16; k++) { int lg = 0; for (int x = k; x > 1; x >>= 1) lg++; w[k] = (int)(rng() % (1u << (10 - lg))); b.put(w[k], 10 - lg); }
                std::set<int> want;
                if (t & 1) want.insert(0);
                auto add = [&](int k, int v) { if (w[k]) want.insert(v % 1024); };
                add(1, w[1]);
                add(2, smod(w[1] - 512 + w[2], 1023));
                add(3, smod(w[1] + w[3], 1023));
                add(4, smod(w[1] - 512 + smod(w[2] - 256 + w[4], 511), 1023));
                add(5, smod(w[1] + smod(w[3] - 256 + w[5], 511), 1023));
                add(6, smod(w[1] - 512 + smod(w[2] + w[6], 511), 1023));
                add(7, smod(w[1] + smod(w[3] + w[7], 511), 1023));
                add(8, smod(w[1] - 512 + smod(w[2] - 256 + smod(w[4] - 128 + w[8], 255), 511), 1023));
                add(9, smod(w[1] + smod(w[3] - 256 + smod(w[5] - 128 + w[9], 255), 511), 1023));
                add(10, smod(w[1] - 512 + smod(w[2] + smod(w[6] - 128 + w[10], 255), 511), 1023));
                add(11, smod(w[1] + smod(w[3] + smod(w[7] - 128 + w[11], 255), 511), 1023));
                add(12, smod(w[1] - 512 + smod(w[2] - 256 + smod(w[4] + w[12], 255), 511), 1023));
                add(13, smod(w[1] + smod(w[3] - 256 + smod(w[5] + w[13], 255), 511), 1023));
                add(14, smod(w[1] - 512 + smod(w[2] + smod(w[6] + w[14], 255), 511), 1023));
                add(15, smod(w[1] + smod(w[3] + smod(w[7] + w[15], 255), 511), 1023));
                add(16, smod(w[1] - 512 + smod(w[2] - 256 + smod(w[4] - 128 + smod(w[8] - 64 + w[16], 127), 255), 511), 1023));
                std::vector<int> got;
                if (!decodeFreqList(b.d, got) || std::set<int>(got.begin(), got.end()) != want) bad++;
            }
            // range 512
            {
                Bw b; b.d[0] = 0x88; b.pos = 7;
                const int w0 = (int)(rng() % 1024);
                b.put(w0, 10);
                int w[18] = {};
                for (int k = 1; k <= 17; k++) { int lg = 0; for (int x = k; x > 1; x >>= 1) lg++; w[k] = (int)(rng() % (1u << (9 - lg))); b.put(w[k], 9 - lg); }
                std::set<int> want{w0};
                auto add = [&](int k, int v) { if (w[k]) want.insert((w0 + v) % 1024); };
                add(1, w[1]);
                add(2, smod(w[1] - 256 + w[2], 511));
                add(3, smod(w[1] + w[3], 511));
                add(4, smod(w[1] - 256 + smod(w[2] - 128 + w[4], 255), 511));
                add(5, smod(w[1] + smod(w[3] - 128 + w[5], 255), 511));
                add(6, smod(w[1] - 256 + smod(w[2] + w[6], 255), 511));
                add(7, smod(w[1] + smod(w[3] + w[7], 255), 511));
                add(8, smod(w[1] - 256 + smod(w[2] - 128 + smod(w[4] - 64 + w[8], 127), 255), 511));
                add(9, smod(w[1] + smod(w[3] - 128 + smod(w[5] - 64 + w[9], 127), 255), 511));
                add(10, smod(w[1] - 256 + smod(w[2] + smod(w[6] - 64 + w[10], 127), 255), 511));
                add(11, smod(w[1] + smod(w[3] + smod(w[7] - 64 + w[11], 127), 255), 511));
                add(12, smod(w[1] - 256 + smod(w[2] - 128 + smod(w[4] + w[12], 127), 255), 511));
                add(13, smod(w[1] + smod(w[3] - 128 + smod(w[5] + w[13], 127), 255), 511));
                add(14, smod(w[1] - 256 + smod(w[2] + smod(w[6] + w[14], 127), 255), 511));
                add(15, smod(w[1] + smod(w[3] + smod(w[7] + w[15], 127), 255), 511));
                add(16, smod(w[1] - 256 + smod(w[2] - 128 + smod(w[4] - 64 + smod(w[8] - 32 + w[16], 63), 127), 255), 511));
                add(17, smod(w[1] + smod(w[3] - 128 + smod(w[5] - 64 + smod(w[9] - 32 + w[17], 63), 127), 255), 511));
                std::vector<int> got;
                if (!decodeFreqList(b.d, got) || std::set<int>(got.begin(), got.end()) != want) bad++;
            }
        }
        CHECK(bad == 0, "range format frequency lists: %d of 400 differ", bad);
        // variable bit map: origin 960, then bits for 961 + i
        Bw v; v.d[0] = 0x8E; v.pos = 7; v.put(960, 10);
        for (int i = 1; i <= 112; i++) v.put((i == 1 || i == 8 || i == 13) ? 1 : 0, 1);
        std::vector<int> got;
        CHECK(decodeFreqList(v.d, got) && got == std::vector<int>({960, 961, 968, 973}), "variable bit map");
        // the encoder of the test signal: the lists it writes read back
        int fitted = 0, wrong = 0;
        for (int t = 0; t < 300; t++) {
            std::set<int> pick;
            const int nn = 1 + (int)(rng() % 6);
            const int base = 955 + (int)(rng() % 19);
            while ((int)pick.size() < nn) pick.insert(base + (int)(rng() % 20) - 3);
            const std::vector<int> want(pick.begin(), pick.end());
            uint8_t cd[16];
            if (!encodeFreqList(want, cd)) continue;
            fitted++;
            std::vector<int> g;
            if (!decodeFreqList(cd, g) || g != want) wrong++;
        }
        CHECK(fitted > 200 && wrong == 0, "frequency list encoder: %d fitted, %d wrong", fitted, wrong);
        for (int sc = 0; sc < 3; sc++)
            for (int i = 0; i < gsmrTestCells(sc); i++) {
                const GsmrCellCfg c = gsmrTestCell(sc, i);
                uint8_t cd[16]; std::vector<int> g;
                CHECK(encodeFreqList(c.cellAlloc, cd) && decodeFreqList(cd, g) && g == c.cellAlloc, "cell allocation of scene %d cell %d", sc, i);
                CHECK(encodeFreqList(c.neigh, cd) && decodeFreqList(cd, g) && g == c.neigh, "neighbour list of scene %d cell %d", sc, i);
            }
        uint8_t u[16] = {0xCA};
        CHECK(!decodeFreqList(u, got), "unknown format rejected");
    }
    // GMSK: the FCCH is a tone a quarter of the symbol rate above the carrier
    {
        std::vector<cf32> x;
        dect2::normal_distribution<float> nd(0.f, 1.f);
        auto add = [&](const std::vector<cf32>& v) { x.insert(x.end(), v.begin(), v.end()); };
        auto noise = [&](size_t n) { std::vector<cf32> v(n); for (auto& c : v) c = cf32{0.1f * nd(rng), 0.1f * nd(rng)}; add(v); };
        noise(3000);
        const size_t start = x.size() + 2 * kSps;   // symbol 0 of the burst
        add(gmskBurst(fcchBurst(), kSps));
        noise(5000);
        add(gmskBurst(Bits(randBits(rng, 148)), kSps));   // a data burst is not an FCCH
        noise(4000);
        // frequency error 3 kHz
        for (size_t i = 0; i < x.size(); i++) { const double ph = 2 * M_PI * 3000.0 / kStreamRate * (double)i; x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
        // the burst is lower in the stream than noise(3000) alone because the burst array holds two symbols before it
        const auto h = findFcch(x.data(), x.size());
        CHECK(h.size() == 1, "fcch hits %zu", h.size());
        if (!h.empty()) {
            const double burstStart = h[0].center - 74.0 * kSps;
            CHECK(std::fabs(burstStart - (double)start) < 20, "fcch position %.1f vs %zu", burstStart, start);
            CHECK(std::fabs(h[0].freqErrHz - 3000) < 150, "fcch frequency error %.0f", h[0].freqErrHz);
        }
    }
    // a normal burst through the equaliser: AWGN, an echo one symbol late, a frequency error of 100 Hz, a timing offset of half a symbol
    struct Case { const char* name; float snrDb; float echo; double cfo; int off; int maxBad; };
    for (const Case& cs : {Case{"clean", 40, 0, 0, 0, 0}, Case{"12 dB", 12, 0, 0, 0, 0}, Case{"echo", 20, 0.4f, 0, 0, 0}, Case{"cfo", 20, 0, 100, 0, 0}, Case{"half-symbol timing", 20, 0, 0, 2, 0}, Case{"6 dB", 6, 0, 0, 0, 9}}) {
        int worst = 0;
        for (int t = 0; t < 10; t++) {
            const int tsc = t & 7;
            Bits data = randBits(rng, 114);
            const Bits bits = normalBurst(data, tsc);
            std::vector<cf32> g = gmskBurst(bits, kSps);
            std::vector<cf32> x(g.size() + 600, cf32(0, 0));
            const size_t at = 300;
            for (size_t i = 0; i < g.size(); i++) {
                x[at + i] += g[i];
                if (cs.echo > 0 && at + i + kSps < x.size()) x[at + i + kSps] += cs.echo * cf32(std::cos(1.1f), std::sin(1.1f)) * g[i];
            }
            const float sigma = (float)std::sqrt(std::pow(10.0, -cs.snrDb / 10) / 2);
            dect2::normal_distribution<float> nd(0.f, 1.f);
            for (size_t i = 0; i < x.size(); i++) {
                const double ph = 2 * M_PI * cs.cfo / kStreamRate * (double)i;
                x[i] = x[i] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32{sigma * nd(rng), sigma * nd(rng)};
            }
            // symbol 0 of the burst is two symbols into the array
            const double sym0 = (double)(at + 2 * kSps);
            const BurstRx r = demodBurst(x.data(), x.size(), sym0 + cs.off + 5, 6, kTsc[tsc], 26, 61);
            int bad = 0;
            if (!r.ok) bad = 99;
            else for (int i = 0; i < 57; i++) {
                if ((r.llr[3 + i] < 0) != (data[(size_t)i] != 0)) bad++;
                if ((r.llr[88 + i] < 0) != (data[(size_t)(57 + i)] != 0)) bad++;
            }
            worst = std::max(worst, bad);
        }
        CHECK(worst <= cs.maxBad, "burst %s: worst %d bad bits", cs.name, worst);
    }
    // a whole generated base station frame sequence: the generator makes what the decoder reads (checked end to end in test_gsmr_rx)
    {
        const GsmrCellCfg c = gsmrTestCell(0, 0);
        const auto f = btsFrames(c, 100, 3);
        CHECK(f.size() == 30000, "bts frame size");
        double e = 0; for (const auto& v : f) e += std::norm(v);
        CHECK(e / (double)f.size() > 0.5 && e / (double)f.size() < 1.2, "bts power %.2f", e / (double)f.size());
    }
    printf(fails ? "gsmr phy: %d FAILED\n" : "gsmr phy: all passed\n", fails);
    return fails ? 1 : 0;
}
