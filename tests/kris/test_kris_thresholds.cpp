// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FER-versus-Es/N0 measurement of the Kris profiles on an AWGN channel (KAVB n = 1920, KAPP n = 2304, KAFT n = 7680). Only runs with KRIS_MEASURE=1 in the environment
// (ctest -R kris_thresholds, slow); without it the test passes at once. For every (n, rate, modulation) it sweeps Es/N0 in 0.5 dB steps from 2 dB below the
// "Shannon + 1.5 dB" estimate (Shannon: Es/N0 = 2^(rate x bits per symbol) - 1) upwards, 300 codewords per point (a point is abandoned at the 4th frame error), and prints
//   KRIS_THRESH n=1920 rate=1/2 mod=QPSK esn0_fer1e-2=3.50
// the first Es/N0 where at most 3 of 300 codewords (1%) fail, or "none" when that is not reached up to the estimate + 5 dB. Es = 1, N0 = 10^(-Es/N0 dB / 10),
// max-log demapper, 50 layered min-sum iterations, random information bits, no interleaver.
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace dect2::kris;

struct Profile { int n; LdpcRate r; Mod m; };

static const char* rateName(LdpcRate r) {
    static const char* names[] = {"1/4", "1/3", "1/2", "2/3", "3/4", "5/6", "9/10"};
    return names[static_cast<int>(r)];
}
static double rateVal(LdpcRate r) {
    static const double v[] = {0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75, 5.0 / 6, 0.9};
    return v[static_cast<int>(r)];
}
static const char* modName(Mod m) {
    switch (m) {
        case Mod::Bpsk: return "BPSK";
        case Mod::Qpsk: return "QPSK";
        case Mod::Qam16: return "16QAM";
        case Mod::Qam64: return "64QAM";
        case Mod::Qam256: return "256QAM";
        default: return "1024QAM";
    }
}

static XorShift64 rng{0x5EEDF00Dull};
static double uni() { return (double((rng.next() >> 11) + 1)) / 9007199254740993.0; }

// Number of failed codewords among `frames` at Es/N0 `esn0` dB; stops counting once `limit` is exceeded.
static int failures(const Profile& p, double esn0, int frames, int limit) {
    const int n = p.n, k = ldpcK(n, p.r), bps = static_cast<int>(p.m);
    const double n0 = std::pow(10.0, -esn0 / 10.0), sd = std::sqrt(n0 / 2.0);
    std::vector<uint8_t> info(k), cw(n), out(k);
    std::vector<float> llr(((n + bps - 1) / bps) * bps);
    int bad = 0;
    for (int f = 0; f < frames && bad <= limit; f++) {
        for (auto& b : info) b = (rng.next() >> 33) & 1;
        ldpcEncode(n, p.r, info.data(), cw.data());
        auto sym = qamMap(cw.data(), cw.size(), p.m);
        for (auto& s : sym) {
            const double a = std::sqrt(-2.0 * std::log(uni())), t = 6.283185307179586 * uni();
            s += std::complex<float>(float(sd * a * std::cos(t)), float(sd * a * std::sin(t)));
        }
        qamDemapSoft(sym.data(), sym.size(), p.m, float(n0), llr.data());
        const int it = ldpcDecode(n, p.r, llr.data(), out.data(), 50);
        if (it < 0 || out != info) bad++;
    }
    return bad;
}

static void measure(const Profile& p) {
    const double eta = rateVal(p.r) * static_cast<int>(p.m);
    const double est = 10.0 * std::log10(std::pow(2.0, eta) - 1.0) + 1.5;
    double found = 1e9;
    for (double e = est - 2.0; e <= est + 5.0 + 1e-9; e += 0.5) {
        if (failures(p, e, 300, 3) <= 3) { found = e; break; }
    }
    if (found < 1e8) printf("KRIS_THRESH n=%d rate=%s mod=%s esn0_fer1e-2=%.2f\n", p.n, rateName(p.r), modName(p.m), found);
    else printf("KRIS_THRESH n=%d rate=%s mod=%s esn0_fer1e-2=none\n", p.n, rateName(p.r), modName(p.m));
    fflush(stdout);
}

int main() {
    const char* e = std::getenv("KRIS_MEASURE");
    if (!e || std::string(e) != "1") { printf("test_kris_thresholds: skipped (set KRIS_MEASURE=1 to measure)\n"); return 0; }
    std::vector<Profile> list;
    for (LdpcRate r : {LdpcRate::R1_4, LdpcRate::R1_2, LdpcRate::R2_3, LdpcRate::R3_4, LdpcRate::R5_6})   // KAVB
        for (Mod m : {Mod::Qpsk, Mod::Qam16, Mod::Qam64}) list.push_back({1920, r, m});
    list.push_back({2304, LdpcRate::R1_4, Mod::Bpsk});   // KAPP
    for (LdpcRate r : {LdpcRate::R1_4, LdpcRate::R1_2, LdpcRate::R3_4}) list.push_back({2304, r, Mod::Qpsk});
    for (LdpcRate r : {LdpcRate::R1_2, LdpcRate::R3_4}) list.push_back({2304, r, Mod::Qam16});
    list.push_back({2304, LdpcRate::R2_3, Mod::Qam64});
    list.push_back({2304, LdpcRate::R3_4, Mod::Qam256});
    list.push_back({2304, LdpcRate::R5_6, Mod::Qam1024});
    list.push_back({7680, LdpcRate::R1_4, Mod::Bpsk});   // KAFT
    for (LdpcRate r : {LdpcRate::R1_4, LdpcRate::R1_2, LdpcRate::R3_4}) list.push_back({7680, r, Mod::Qpsk});
    list.push_back({7680, LdpcRate::R3_4, Mod::Qam16});
    list.push_back({7680, LdpcRate::R3_4, Mod::Qam64});
    list.push_back({7680, LdpcRate::R3_4, Mod::Qam256});
    list.push_back({7680, LdpcRate::R5_6, Mod::Qam256});
    list.push_back({7680, LdpcRate::R5_6, Mod::Qam1024});
    list.push_back({7680, LdpcRate::R9_10, Mod::Qam1024});
    for (const auto& p : list) measure(p);
    return 0;
}
