// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-SH TDM (SH-B, EN 302 583 clause 5.6): the tables (frame width, symbol rate: one TDM SH frame lasts exactly one OFDM SH frame), the
// signalling field (fields, CRCs, the rate 1/5 turbo code and its bit interleaver through noise), the constellations and the PL scrambling,
// and generator -> receiver round trips with impairments: unknown start, carrier offset, symbol clock offset, AWGN, the automatic symbol
// rate search over table 5.13, QPSK / 8PSK / 16APSK, a blockage under the early/late time interleaver, and a signal below threshold.
// Simulated signals only. ONAIR_TEST_LONG=1 adds a sweep over the modulations, code rates and channel widths.
#include "dect2/dvbsh/dvbsh_tdm.h"
#include "dect2/dvbs/dvbs_s2.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::dvbsh;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static TdmGenerator::PacketSource seqSource(unsigned pid) {
    return [pid, n = 0u](uint8_t* p) mutable {
        p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (n & 15));
        p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
        uint32_t h = n * 2654435761u + pid;
        for (int i = 8; i < 188; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; p[i] = (uint8_t)h; }
        n++;
    };
}
static bool packetGood(const uint8_t* p, unsigned pid, uint32_t* seq) {
    if (p[0] != 0x47 || (p[1] & 0x80) || (((p[1] & 0x1F) << 8) | p[2]) != (int)pid) return false;
    const uint32_t n = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    uint32_t h = n * 2654435761u + pid;
    for (int i = 8; i < 188; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; if (p[i] != (uint8_t)h) return false; }
    *seq = n;
    return true;
}

static void testTables() {
    // a TDM SH frame lasts as long as the OFDM SH frame it is aligned to: slots * 2176 / Rs = frames per SH frame * 68 * (N + G) / fs (2K)
    int bad = 0, n = 0;
    const double bws[5] = {8, 7, 6, 5, 1.7};
    for (double bw : bws)
        for (int g = 0; g < 4; g++)
            for (int r = 0; r < 3; r++)
                for (int q = 0; q < 2; q++)
                    for (int m = 0; m < 3; m++) {
                        Params op; op.mode = bw == 1.7 ? k1K : k2K; op.guard = g; op.mod = q ? k16Qam : kQpsk; op.bwMhz = bw;
                        const double tOfdm = framesPerShFrame(op) * 68.0 * (fftN(op.mode) + guardSamples(op.mode, g)) / sampleRate(bw);
                        const int cu = tdmFrameCu(m, g, r, q != 0);
                        const double tTdm = (double)cu / tdmCuPerSlot(m) * kSlotSymbols / tdmSymbolRate(bw, g, r);
                        n++;
                        // the 1K mode carries a quarter of 4K per symbol: 1.7 MHz scales like the others
                        if (cu % tdmCuPerSlot(m) != 0 || std::fabs(tTdm / tOfdm - 1) > 1e-9) { if (bad < 4) std::printf("  frame time %g MHz GI %d roll %d %s %s: TDM %.6f s, OFDM %.6f s\n", bw, g, r, q ? "16QAM" : "QPSK", tdmModName(m), tTdm, tOfdm); bad++; }
                    }
    CHECK(bad == 0, "%d of %d table 5.12 / 5.13 combinations do not give the OFDM SH frame length", bad, n);
    CHECK(tdmFrameCu(kTdmQpsk, 3, 0, false) == 952 && tdmFrameCu(kTdm16apsk, 0, 2, true) == 672 && std::fabs(tdmSymbolRate(5, 3, 0) - 4.25e6) < 1, "table values");
    CHECK(tdmCodewords(952, 4) == 52 && tdmDataCu(952, 4) == 936, "QPSK 1/3 at 952 CU: 52 code words");
}

static void testSignalling() {
    TdmSignalling s;
    s.version = 1; s.frameWidthCu = 1428; s.pat = 9; s.ti.commonMultiplier = 2; s.ti.lateTaps = 24; s.ti.slices = 2; s.ti.sliceDistance = 1; s.ti.nonLateIncrement = 1;
    s.llPresent = true; s.llPat = 4; s.bursts = 2; s.burst[0] = 0x8000 | 300; s.burst[1] = 100;
    uint8_t b[kSigInput];
    packSignalling(s, b);
    TdmSignalling r;
    CHECK(unpackSignalling(b, r) && r == s, "signalling field pack / unpack");
    b[20] ^= 1;
    CHECK(!unpackSignalling(b, r), "a damaged field is refused (CRC-16)");
    std::vector<uint8_t> enc;
    encodeSignalling(s, enc);
    CHECK(enc.size() == (size_t)kSfBits && kSofBits + kSfBits == kHeaderCu * kCuBits, "5 760 coded bits, SOF + SF = 3 CU");
    // BPSK through noise at Es/N0 -2 dB (rate 1/5: about 5 dB of Eb/N0)
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (double snr : {-2.0, -8.0}) {
        const float sig = (float)std::sqrt(std::pow(10.0, -snr / 10) / 2);
        std::vector<float> llr(kSfBits);
        for (int i = 0; i < kSfBits; i++) { const float y = (enc[(size_t)i] ? -1.f : 1.f) + sig * nd(rng); llr[(size_t)i] = 2 * y / (sig * sig); }
        TdmSignalling d;
        int it = 0;
        const bool ok = decodeSignalling(llr.data(), d, &it);
        std::printf("  signalling field at Es/N0 %.0f dB: %s (%d iterations)\n", snr, ok && d == s ? "decoded" : "not decoded", it);
        if (snr > -5) CHECK(ok && d == s, "signalling field through noise");
        else CHECK(!ok || d == s, "a wrong field is never accepted");
    }
    const auto& sof = sofBits();
    CHECK(sof[0] == 1 && sof[4] == 0 && sof[287] == 1 && sof[284] == 1, "SOF bits F3484536 ... 9E4F");
}

static void testConstellations() {
    for (int m = 0; m < 3; m++) {
        const cf32* c = tdmConstellation(m);
        double p = 0;
        for (int i = 0; i < (1 << tdmBitsPerSymbol(m)); i++) p += std::norm(c[i]);
        CHECK(std::fabs(p / (1 << tdmBitsPerSymbol(m)) - 1) < 1e-4, "%s unit energy", tdmModName(m));
    }
    const cf32* a = tdmConstellation(kTdm16apsk);
    CHECK(std::fabs(std::abs(a[0xC]) - 1 / std::sqrt(7.f)) < 1e-5 && std::fabs(std::abs(a[0x0]) - 3 / std::sqrt(7.f)) < 1e-5, "16APSK radii 1/sqrt(7), 3/sqrt(7)");
    const cf32* q = tdmConstellation(kTdmQpsk);
    CHECK(q[0].real() > 0 && q[0].imag() > 0 && q[2].real() < 0 && q[1].imag() < 0, "QPSK: I = MSB, 0 positive (figure 5.17)");
    const auto& C = tdmScrambling(0);
    const auto& R = dvbs::s2ScramblingRn(0);
    int same = 0;
    for (int i = 0; i < kSlotSymbols; i++) same += C[(size_t)i] == std::polar(1.f, (float)(R[(size_t)i] * M_PI / 2)) || std::abs(C[(size_t)i] - std::polar(1.f, (float)(R[(size_t)i] * M_PI / 2))) < 1e-6f;
    CHECK(same == kSlotSymbols, "PL scrambling = the DVB-S2 Gold sequence n = 0");
}

struct Run {
    TdmStatus st;
    long good = 0, bad = 0, gaps = 0;
};

static Run run(const TdmParams& p, double fs, double snr, double cfo, double ppm, double seconds, bool autoRate, double blockStart = -1, double blockLen = 0) {
    TdmSignalConfig c;
    c.p = p; c.sampleRate = fs; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm; c.seed = 5;
    TdmSignal sig(c, seqSource(0x123));
    TdmReceiver rx;
    rx.configure(fs, p.bwMhz);
    if (!autoRate) rx.setSymbolRate(p.symbolRate());
    Run r;
    int64_t last = -1;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uint32_t s;
            if (packetGood(pk + 188 * i, 0x123, &s)) { r.good++; if (last >= 0 && (int64_t)s != last + 1) r.gaps++; last = s; }
            else r.bad += (pk[188 * i + 1] & 0x80) ? 0 : 1;   // flagged packets are not errors; a wrong packet passed as good is
        }
    });
    // a random start: drop some samples first
    std::mt19937 rng(11);
    std::vector<cf32> buf(65536);
    sig.generate(buf.data(), 1000 + rng() % 30000);
    double t = 0;
    std::normal_distribution<float> nd(0.f, 1.f);
    while (t < seconds) {
        sig.generate(buf.data(), buf.size());
        if (blockStart >= 0)
            for (size_t i = 0; i < buf.size(); i++) { const double tt = t + i / fs; if (tt >= blockStart && tt < blockStart + blockLen) buf[i] = cf32(0.02f * nd(rng), 0.02f * nd(rng)); }
        rx.feed(buf.data(), buf.size());
        t += buf.size() / fs;
    }
    r.st = rx.status();
    return r;
}

static void report(const char* name, const Run& r) {
    std::printf("  %-52s level %d %s Rs %.4f MHz CFO %+.0f Hz clock %+.1f ppm Es/N0 %.1f dB, frames %ld SF %ld/%ld, code words %ld/%ld, packets good %ld (gaps %ld) wrong %ld, iterations %.1f\n",
                name, r.st.level, r.st.mod >= 0 ? tdmModName(r.st.mod) : "-", r.st.symbolRate / 1e6, r.st.cfoHz, r.st.clockPpm, r.st.snrDb, r.st.frames, r.st.sfOk, r.st.sfBad,
                r.st.codewordsOk, r.st.codewordsBad, r.good, r.gaps, r.bad, r.st.avgIterations);
}

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr;
    testTables();
    testSignalling();
    testConstellations();
    {
        TdmParams p; p.mod = kTdmQpsk; p.pat = 4; p.bwMhz = 5; p.ofdmGuard = 3; p.rollIdx = 0;
        const Run r = run(p, 8e6, 3.0, 12300, 20, 1.2, true);
        report("QPSK 1/3, 5 MHz, 3 dB, +12.3 kHz, +20 ppm, rate auto", r);
        CHECK(r.st.level == 4 && r.good > 1200 && r.bad == 0 && r.gaps == 0 && r.st.codewordsBad == 0, "QPSK round trip");
        CHECK(std::fabs(r.st.clockPpm - 20) < 15, "QPSK: the symbol clock offset");
        CHECK(std::fabs(r.st.cfoHz - 12300) < 300 && r.st.sf.frameWidthCu == 952 && r.st.mod == kTdmQpsk, "QPSK: carrier offset, frame width, modulation");
    }
    {
        TdmParams p; p.mod = kTdm8psk; p.pat = 8; p.bwMhz = 8; p.ofdmGuard = 2; p.rollIdx = 1;
        const Run r = run(p, 10e6, 10.0, -7000, -15, 1.0, true);
        report("8PSK 1/2, 8 MHz, 10 dB, -7 kHz, -15 ppm, rate auto", r);
        CHECK(r.st.level == 4 && r.good > 1500 && r.bad == 0 && r.gaps == 0 && r.st.codewordsBad == 0 && r.st.mod == kTdm8psk, "8PSK round trip");
    }
    {
        TdmParams p; p.mod = kTdm16apsk; p.pat = 10; p.bwMhz = 5; p.ofdmGuard = 0; p.rollIdx = 2; p.ofdm16qam = true;
        const Run r = run(p, 6e6, 14.0, 2500, 8, 1.2, false);
        report("16APSK 2/3, 5 MHz (OFDM 16-QAM frame), 14 dB, +2.5 kHz", r);
        CHECK(r.st.level == 4 && r.good > 1500 && r.bad == 0 && r.gaps == 0 && r.st.codewordsBad == 0 && r.st.mod == kTdm16apsk, "16APSK round trip");
    }
    {
        // early/late interleaver (2 x 24 taps): a 50 ms blockage is repaired; without the long interleaver it is not
        TdmParams p; p.mod = kTdmQpsk; p.pat = 4; p.bwMhz = 5; p.ofdmGuard = 3; p.rollIdx = 0;
        p.ti.commonMultiplier = 2; p.ti.lateTaps = 24; p.ti.slices = 2; p.ti.sliceDistance = 1; p.ti.nonLateIncrement = 1;
        const Run r = run(p, 8e6, 6.0, 900, 5, 1.8, false, 1.0, 0.05);
        report("QPSK 1/3, early/late interleaver, 50 ms blockage", r);
        CHECK(r.st.level == 4 && r.good > 1500 && r.bad == 0 && r.st.codewordsBad == 0 && r.st.tiLmax > 0, "blockage repaired by the time interleaver");
        TdmParams u = p; u.ti = TiConfig{};
        const Run s = run(u, 8e6, 6.0, 900, 5, 1.4, false, 1.0, 0.05);
        report("QPSK 1/3, short interleaver, the same blockage", s);
        CHECK(s.st.codewordsBad > 0 && s.bad == 0, "the short interleaver loses code words, none passed as good");
    }
    {
        TdmParams p; p.mod = kTdm8psk; p.pat = 11; p.bwMhz = 5; p.ofdmGuard = 3; p.rollIdx = 0;
        const Run r = run(p, 8e6, 2.0, 0, 0, 0.8, false);
        report("8PSK 2/3c at 2 dB (below threshold)", r);
        CHECK(r.bad == 0, "below threshold no wrong packet is passed as good");
    }
    if (longRun) {
        const double bws[3] = {5, 7, 1.7};
        int ok = 0, total = 0;
        for (int m = 0; m < 3; m++)
            for (int pat : {0, 3, 7, 11})
                for (double bw : bws) {
                    TdmParams p; p.mod = m; p.pat = pat; p.bwMhz = bw; p.ofdmGuard = (pat + m) % 4; p.rollIdx = (int)(bw * 10) % 3;
                    const double snr = m == 0 ? 8 : m == 1 ? 13 : 17;
                    const double fs = p.symbolRate() * 1.6;
                    const Run r = run(p, fs, snr, 1500, 6, 1.0, true);
                    char nm[96]; snprintf(nm, sizeof nm, "sweep %s %s %.1f MHz", tdmModName(m), dvbsh::rateName(pat), bw);
                    report(nm, r);
                    total++;
                    if (r.st.level == 4 && r.good >= 40 && r.bad == 0 && r.gaps == 0 && r.st.codewordsBad == 0) ok++;
                }
        std::printf("  sweep: %d of %d decode with no bad packets\n", ok, total);
        CHECK(ok == total, "sweep");
    }
    std::printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
