// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) PHY tables against independent references: the published S1G data rates (tone counts, bits per symbol, valid MCS), the
// 802.11 constellation and puncturing definitions, the 127-bit scrambler sequence of the 802.11 standard, and the structural properties of every
// width (tone layouts 1 - 16 MHz, interleaver permutations for every width and MCS, STF periodicity, SIG field round trips of both preambles).
#include "dect2/halow/halow_proto.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::halow;
#include "../common/test_util.h"

static bool nearBy(double a, double b, double e = 0.006) { return std::fabs(a - b) <= e; }

int main() {
    // data tones, pilots, used tones: the 802.11ah clause 23 numbers (24 + 2, 52 + 4, 108 + 6, 234 + 8, 468 + 16)
    const int nsd[5] = {24, 52, 108, 234, 468}, nsp[5] = {2, 4, 6, 8, 16};
    for (int w = 0; w < kNumWidths; w++) {
        const WidthInfo& wi = info(w);
        CHECK((int)wi.data.dataK.size() == nsd[w] && (int)wi.data.pilotK.size() == nsp[w] && wi.nUsed == nsd[w] + nsp[w], "layout %d MHz: %zu + %zu", 1 << w, wi.data.dataK.size(), wi.data.pilotK.size());
        CHECK(wi.ncol * wi.nrow == wi.ncbps && wi.sigNcol * wi.sigNrow == wi.sigNcbps, "interleaver size %d", w);
        CHECK(nfft(w) == (w <= 2 ? 128 : w == 3 ? 256 : 512) && std::fabs(gridRate(w) - nfft(w) * 31250.0) < 1, "grid %d", w);
        // no tone twice, none at DC, all inside the FFT, pilots not among the data tones
        std::vector<int> seen((size_t)nfft(w), 0);
        for (int k : wi.data.dataK) { CHECK(k != 0 && std::abs(k) < nfft(w) / 2, "tone %d", k); seen[(size_t)bin(k, nfft(w))]++; }
        for (int k : wi.data.pilotK) seen[(size_t)bin(k, nfft(w))]++;
        bool uniq = true;
        for (int v : seen) if (v > 1) uniq = false;
        CHECK(uniq, "a tone used twice at %d MHz", 1 << w);
        // the SIG field: one 2 MHz copy per 2 MHz of the channel (1 at 2 MHz, 2, 4, 8), every copy 52 tone slots
        if (w >= W2) CHECK((int)wi.sig.size() == (w == W2 ? 1 : 1 << (w - 1)), "SIG copies %zu at %d MHz", wi.sig.size(), 1 << w);
        for (const Layout& l : wi.sig) CHECK((int)l.dataK.size() == wi.sigNcbps, "SIG tones");
        // the interleaver is a permutation for every modulation
        for (int nb : {1, 2, 4, 6, 8}) {
            const int ncbps = wi.nsd * nb;
            std::vector<int> cnt((size_t)ncbps, 0);
            for (int k = 0; k < ncbps; k++) cnt[(size_t)interleaveJ(wi.ncol, wi.nrow * nb, ncbps, nb, k)]++;
            bool perm = true;
            for (int v : cnt) if (v != 1) perm = false;
            CHECK(perm, "interleaver %d MHz, %d bits per tone is not a permutation", 1 << w, nb);
        }
        // STF: period 8 us, unit power; LTF: +-1 on exactly the used tones
        std::vector<cf32> x((size_t)nfft(w));
        stfTime(w, x.data());
        const int N = nfft(w), lag = N / 4;
        double e = 0, d = 0;
        for (int i = 0; i < N; i++) { e += std::norm(x[(size_t)i]); d += std::norm(x[(size_t)i] - x[(size_t)((i + lag) % N)]); }
        CHECK(std::fabs(e / N - 1) < 0.01 && d < 1e-6 * e, "STF %d MHz: power %.3f period error %.2e", 1 << w, e / N, d / e);
        std::vector<float> lf((size_t)N);
        ltfSpectrum(w, lf.data());
        int nz = 0;
        for (float v : lf) if (v != 0.f) { nz++; CHECK(std::fabs(std::fabs(v) - 1) < 1e-6, "LTF value"); }
        CHECK(nz == wi.nUsed && lf[0] == 0.f, "LTF %d MHz tones %d of %d", 1 << w, nz, wi.nUsed);
    }
    // the published S1G data rates (long GI, one spatial stream), Mbit/s: 1 MHz 0.3 .. 4.0 and MCS 10 0.15; 2 MHz 0.65 .. 7.8; 4 MHz 1.35 .. 18; 8 MHz 2.925 .. 39; 16 MHz 5.85 .. 78
    CHECK(nearBy(dataRateMbps(W1, 0, 1, false), 0.30) && nearBy(dataRateMbps(W1, 10, 1, false), 0.15) && nearBy(dataRateMbps(W1, 9, 1, false), 4.0), "1 MHz rates");
    CHECK(nearBy(dataRateMbps(W2, 0, 1, false), 0.65) && nearBy(dataRateMbps(W2, 1, 1, false), 1.3) && nearBy(dataRateMbps(W2, 8, 1, false), 7.8), "2 MHz rates");
    CHECK(nearBy(dataRateMbps(W4, 0, 1, false), 1.35) && nearBy(dataRateMbps(W4, 4, 1, false), 8.1) && nearBy(dataRateMbps(W4, 9, 1, false), 18.0), "4 MHz rates");
    CHECK(nearBy(dataRateMbps(W8, 0, 1, false), 2.925) && nearBy(dataRateMbps(W8, 7, 1, false), 29.25) && nearBy(dataRateMbps(W8, 9, 1, false), 39.0), "8 MHz rates");
    CHECK(nearBy(dataRateMbps(W16, 0, 1, false), 5.85) && nearBy(dataRateMbps(W16, 9, 1, false), 78.0) && nearBy(dataRateMbps(W16, 9, 1, true), 86.667) && nearBy(dataRateMbps(W16, 9, 4, true), 346.667), "16 MHz rates");
    // valid MCS: 2 MHz MCS 9 only with 3 streams, 1 MHz MCS 10 only at 1 MHz, MCS 10 never above
    CHECK(!mcsValid(W2, 9, 1) && !mcsValid(W2, 9, 2) && mcsValid(W2, 9, 3) && !mcsValid(W2, 9, 4), "2 MHz MCS 9");
    CHECK(mcsValid(W1, 10, 1) && !mcsValid(W2, 10, 1) && !mcsValid(W16, 10, 1) && !mcsValid(W4, 11, 1) && !mcsValid(W4, -1, 1), "MCS 10");
    for (int w = W4; w < kNumWidths; w++) for (int m = 0; m <= 9; m++) CHECK(mcsValid(w, m, 1), "%d MHz MCS %d must be valid with one stream", 1 << w, m);
    for (int m = 0; m <= 9; m++) CHECK(mcsValid(W1, m, 1), "1 MHz MCS %d", m);
    CHECK(encoders(W8, 9, 1) == 1 && encoders(W16, 9, 1) == 2 && encoders(W16, 6, 1) == 1, "encoders");
    // sample rate rules: 1 MHz from 2 Msps up to 16 MHz from 16 Msps (20 recommended)
    CHECK(maxWidthMhz(1e6) == 0 && maxWidthMhz(2e6) == 2 && maxWidthMhz(4e6) == 4 && maxWidthMhz(10e6) == 8 && maxWidthMhz(20e6) == 16 && recommendedRateHz(W16) == 20e6 && recommendedRateHz(W1) == 2e6, "rates");
    // constellations (17.3.5.8, 21.3.10.9): 16-QAM 00 -> -3, 01 -> -1, 11 -> +1, 10 -> +3; 64-QAM 000 -> -7, 011 -> -3, 110 -> +1, 100 -> +7; normalisation 1 / sqrt(10, 42, 170)
    {
        const uint8_t a[4] = {0, 0, 0, 0}, b[4] = {1, 0, 0, 1}, c[4] = {1, 1, 0, 1};
        const float k10 = 1.f / std::sqrt(10.f);
        CHECK(nearBy(mapQam(a, 4).real(), -3 * k10, 1e-5) && nearBy(mapQam(a, 4).imag(), -3 * k10, 1e-5), "16-QAM 0000");
        CHECK(nearBy(mapQam(b, 4).real(), 3 * k10, 1e-5) && nearBy(mapQam(b, 4).imag(), -1 * k10, 1e-5), "16-QAM 1001");
        CHECK(nearBy(mapQam(c, 4).real(), 1 * k10, 1e-5) && nearBy(mapQam(c, 4).imag(), -1 * k10, 1e-5), "16-QAM 1101");
        const uint8_t d[6] = {0, 1, 1, 1, 0, 0}, e[6] = {1, 0, 0, 0, 0, 0};
        const float k42 = 1.f / std::sqrt(42.f);
        CHECK(nearBy(mapQam(d, 6).real(), -3 * k42, 1e-5) && nearBy(mapQam(d, 6).imag(), 7 * k42, 1e-5), "64-QAM 011 100");
        CHECK(nearBy(mapQam(e, 6).real(), 7 * k42, 1e-5) && nearBy(mapQam(e, 6).imag(), -7 * k42, 1e-5), "64-QAM 100 000");
        const uint8_t f[8] = {1, 0, 0, 0, 0, 0, 0, 0};
        CHECK(nearBy(mapQam(f, 8).real(), 15 / std::sqrt(170.f), 1e-5) && nearBy(mapQam(f, 8).imag(), -15 / std::sqrt(170.f), 1e-5), "256-QAM corner");
        // unit average power of every constellation
        for (int nb : {1, 2, 4, 6, 8}) {
            double p = 0;
            const int n = 1 << nb;
            for (int v = 0; v < n; v++) { uint8_t bt[8]; for (int i = 0; i < nb; i++) bt[i] = (uint8_t)(v >> (nb - 1 - i) & 1); p += std::norm(mapQam(bt, nb)); }
            CHECK(nearBy(p / n, 1.0, 1e-4), "constellation power %d bits: %.4f", nb, p / n);
        }
        // the soft demapper agrees with the mapper for every symbol of every constellation, with noise-free input
        for (int nb : {1, 2, 4, 6, 8}) {
            bool ok = true;
            for (int v = 0; v < (1 << nb); v++) {
                uint8_t bt[8];
                for (int i = 0; i < nb; i++) bt[i] = (uint8_t)(v >> (nb - 1 - i) & 1);
                float l[8];
                demapQam(mapQam(bt, nb), 0.01f, nb, l);
                for (int i = 0; i < nb; i++) if ((l[i] > 0) != (bt[i] == 1)) ok = false;
            }
            CHECK(ok, "demapper vs mapper, %d bits", nb);
        }
    }
    // puncturing (17.3.5.7, 19.3.11.7): from A1 B1 A2 B2 A3 B3 ..., 3/4 keeps A1 B1 A2 B3; 2/3 keeps A1 B1 A2; 5/6 keeps A1 B1 A2 B3 A4 B5
    {
        std::vector<uint8_t> c(60);
        for (size_t i = 0; i < c.size(); i++) c[i] = (uint8_t)(i % 2 == 0 ? 0 : 1) ^ (uint8_t)((i / 2) & 1);   // an arbitrary pattern
        const auto p34 = puncture(c, 3, 4);
        CHECK(p34.size() == 40 && p34[0] == c[0] && p34[1] == c[1] && p34[2] == c[2] && p34[3] == c[5] && p34[4] == c[6], "3/4");
        const auto p23 = puncture(c, 2, 3);
        CHECK(p23.size() == 45 && p23[2] == c[2] && p23[3] == c[4], "2/3");
        const auto p56 = puncture(c, 5, 6);
        CHECK(p56.size() == 36 && p56[3] == c[5] && p56[4] == c[6] && p56[5] == c[9], "5/6");
        std::vector<float> l(p34.begin(), p34.end());
        for (auto& v : l) v = v > 0 ? 1.f : -1.f;
        const auto back = depuncture(l, 3, 4, c.size());
        int er = 0;
        for (size_t i = 0; i < c.size(); i++) if (back[i] == 0.f) er++;
        CHECK(back.size() == c.size() && er == 20, "depuncture erasures %d", er);
        // every rate decodes through the Viterbi decoder
        std::mt19937 rng(11);
        for (auto [num, den] : {std::pair<int, int>{1, 2}, {2, 3}, {3, 4}, {5, 6}}) {
            std::vector<uint8_t> bits(300);
            for (auto& b : bits) b = rng() & 1;
            for (int i = 0; i < 6; i++) bits[bits.size() - 1 - (size_t)i] = 0;
            const auto co = puncture(convEncode(bits), num, den);
            std::vector<float> ll(co.size());
            for (size_t i = 0; i < co.size(); i++) ll[i] = co[i] ? 1.f : -1.f;
            CHECK(viterbi(depuncture(ll, num, den, bits.size() * 2)) == bits, "BCC %d/%d round trip", num, den);
        }
    }
    // the 127-bit scrambler sequence of the 802.11 standard (17.3.5.5) for the all-ones state, as bytes of 8 bits
    {
        const auto s = scramblerSeq(0x7F, 127);
        const char* ref = "00001110111100101100100100000010001001100010111010110110000011001101010011100111101101000010101011111010010100011011100011111111";
        std::string got;
        for (uint8_t b : s) got += (char)('0' + b);
        CHECK(got == std::string(ref).substr(0, 127), "scrambler sequence\n  got %s\n  ref %s", got.c_str(), ref);
    }
    // SIG fields of every width and preamble: every field survives a round trip, a flipped bit is caught
    {
        std::mt19937 rng(21);
        int n = 0;
        for (int w = 0; w < kNumWidths; w++) for (int pre : (w == W1 ? std::vector<int>{Pre1M} : std::vector<int>{PreShort, PreLong})) for (int rep = 0; rep < 40; rep++) {
            Sig s;
            s.w = w; s.pre = pre;
            do { s.mcs = (int)(rng() % 11); s.nsts = (int)(rng() % 4); s.stbc = (rng() & 3) == 0 && (s.nsts & 1); } while (!mcsValid(w, s.mcs, s.nss()));
            s.len = 1 + (int)(rng() % 511); s.sgi = rng() & 1; s.agg = rng() & 1; s.ldpc = (rng() & 3) == 0; s.ldpcExtra = rng() & 1; s.id = (int)(rng() % 512);
            s.ul = rng() & 1; s.resp = (int)(rng() % 4); s.travelPilots = rng() & 1; s.ndp = (rng() & 7) == 0; s.beamformed = pre == PreLong && (rng() & 1);
            if (w == W1) { s.id = 0; s.ul = false; s.beamformed = false; }
            std::vector<uint8_t> b = sigBits(s);
            Sig r;
            const bool ok = parseSig(b.data(), w, r, pre);
            CHECK(ok && r.w == w && r.pre == pre && r.mcs == s.mcs && r.len == s.len && r.nsts == s.nsts && r.stbc == s.stbc && r.sgi == s.sgi && r.agg == s.agg && r.ldpc == s.ldpc &&
                  r.ldpcExtra == s.ldpcExtra && r.resp == s.resp && r.ndp == s.ndp && r.travelPilots == s.travelPilots, "SIG roundtrip %d MHz pre %d", 1 << w, pre);
            if (w != W1) CHECK(r.id == s.id && r.ul == s.ul && r.beamformed == s.beamformed, "SIG id / uplink / beamformed %d MHz", 1 << w);
            const size_t bit = rng() % (size_t)(w == W1 ? 30 : 46);
            b[bit] ^= 1;
            CHECK(!parseSig(b.data(), w, r, pre), "SIG with bit %zu flipped passed (%d MHz)", bit, 1 << w);
            n++;
        }
        printf("  %d SIG fields round-tripped\n", n);
        // a SIG heard at the wrong width is refused by its bandwidth field
        for (int w = W2; w < kNumWidths; w++) {
            Sig s; s.w = w; s.len = 50;
            const auto b = sigBits(s);
            for (int w2 = W2; w2 < kNumWidths; w2++) { Sig r; CHECK(parseSig(b.data(), w2, r, PreShort) == (w2 == w), "width field %d heard as %d", 1 << w, 1 << w2); }
        }
    }
    // PPDU lengths agree with the builder for every width, preamble and MCS (with short GI, with more streams)
    for (int w = 0; w < kNumWidths; w++) for (int pre : (w == W1 ? std::vector<int>{Pre1M} : std::vector<int>{PreShort, PreLong})) for (int mcs = 0; mcs <= (w == W1 ? 10 : 9); mcs++) for (int sgi = 0; sgi < 2; sgi++) {
        PpduSpec s;
        s.w = w; s.pre = pre; s.mcs = mcs; s.sgi = sgi; s.psdu.assign(57, 0xA5);
        if (!mcsValid(w, mcs, 1)) continue;
        CHECK(buildPpdu(s).size() == ppduSamples(s), "PPDU length %d MHz pre %d MCS %d sgi %d", 1 << w, pre, mcs, sgi);
    }
    {
        PpduSpec s;
        s.w = W4; s.mcs = 3; s.nss = 3; s.psdu.assign(80, 1);
        CHECK(buildPpdu(s).size() == ppduSamples(s), "multi stream PPDU length");
        s.nss = 2; s.stbc = true;
        CHECK(buildPpdu(s).size() == ppduSamples(s), "STBC PPDU");
    }
    // timing of the preambles in microseconds (clause 23.3.8): S1G_1M 160 + 160 + 240; S1G_SHORT 80 + 80 + 80; the data symbols 40 us (36 with the short GI)
    for (int w = 0; w < kNumWidths; w++) {
        const double fs = gridRate(w);
        PpduSpec s;
        s.w = w; s.mcs = 0; s.psdu.assign(1, 0);
        const double pre = w == W1 ? 560.0 : 240.0;
        const double total = (double)ppduSamples(s) / fs * 1e6;
        const double nd = dataSymbols(w, 0, 1);
        CHECK(nearBy(total, pre + nd * 40.0, 0.01), "%d MHz: %.2f us, preamble %.0f + %g symbols", 1 << w, total, pre, nd);
    }
    return testSummary("halow phy");
}
