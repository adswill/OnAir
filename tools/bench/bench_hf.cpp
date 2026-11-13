// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CPU cost and decode count of the HF weak-signal slot decoders (FT8 FT4 FT2 WSPR JT65 JT9 Q65 JT4 FST4 MSK144) on synthetic slots with N stations.
//   bench_hf <mode> [stations] [slots] [snr_lo] [snr_hi]    prints: CPU s, s of signal, cost (CPU s per s of signal), decodes, digest of the decoded set, peak RSS
// Built by tools/bench/bench_hf.sh (needs the unit-test helpers in tests/hfdig).
#include "../../tests/hfdig/hfdig_wsjt_testutil.h"
#include <sys/resource.h>
#include <cmath>
#include <cstdlib>
#include <set>
using namespace dect2;

static double cpuSec() {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}
static std::string callOf(int i) {
    static const char* pre[] = {"K", "W", "N", "DL", "G", "JA", "VK", "F", "OH", "SM"};
    char b[16];
    snprintf(b, sizeof b, "%s%d%c%c", pre[i % 10], (i / 10) % 10, 'A' + (i / 100) % 26, 'A' + (i * 7) % 26);
    return b;
}
// BENCH_WAV=<dir>: every slot of sens / the main bench is also written as 16-bit 8 kHz WAV, with the sent messages in <name>.txt (for running
// other decoders on the same audio)
static void writeWav(const std::string& base, const std::vector<float>& a, const std::vector<wsjttest::St>& st) {
    FILE* f = fopen((base + ".wav").c_str(), "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + 2 * (uint32_t)a.size()); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16); fwrite("data", 1, 4, f); u32(2 * (uint32_t)a.size());
    for (float v : a) { const int x = (int)std::lround(std::max(-32767.f, std::min(32767.f, v * 20000.f))); u16((uint16_t)(int16_t)x); }
    fclose(f);
    FILE* t = fopen((base + ".txt").c_str(), "w");
    if (t) { for (const auto& s : st) fprintf(t, "%s\n", s.msg.c_str()); fclose(t); }
}
static const char* kNames[] = {"ft8", "ft4", "ft2", "wspr", "jt65", "jt9", "q65", "fst4", "msk144", "fst4w", "jt4"};
static int modeOf(const char* s) {
    for (int i = 0; i < 11; i++) if (!strcmp(s, kNames[i])) return i;
    return -1;
}
// sensitivity: K stations per slot, all at one SNR, spread over the band with random DT; success rate per SNR and false decodes
//   bench_hf sens <mode> [snrLo] [snrHi] [step=1] [slots=4] [K=8] [drift Hz/s=0] [dtMax=1.0] [dialHz=0]
static int sens(int argc, char** argv) {
    const int mode = modeOf(argv[2]);
    if (mode < 0) return 1;
    const double lo = argc > 3 ? atof(argv[3]) : -26, hi = argc > 4 ? atof(argv[4]) : -14, step = argc > 5 ? atof(argv[5]) : 1;
    const int slots = argc > 6 ? atoi(argv[6]) : 4, K = argc > 7 ? atoi(argv[7]) : 8;
    const double drift = argc > 8 ? atof(argv[8]) : 0, dtMax = argc > 9 ? atof(argv[9]) : 1.0, dial = argc > 10 ? atof(argv[10]) : 0;
    const bool wspr = mode == 3 || mode == 9;
    const double f0 = wspr ? 1410 : 300, bw = wspr ? 170 : 2400;
    const int k = wspr ? std::min(K, 4) : K;
    for (double snr = lo; snr <= hi + 1e-9; snr += step) {
        int ok = 0, all = 0, fake = 0;
        for (int t = 0; t < slots; t++) {
            ftxtest::Gauss g(1000 + t * 31 + (int)(snr * 7 + 500));
            std::vector<wsjttest::St> st;
            for (int i = 0; i < k; i++) {
                std::string a = callOf(i + 13 * t), b = callOf(i + 13 * t + 37);
                std::string msg = wspr ? a + " FN42 37" : (i % 2 ? "CQ " + a + " FN42" : a + " " + b + " -" + std::to_string(10 + i % 9));
                st.push_back({msg, f0 + bw * (i + 0.2 + 0.6 * g.u()) / k, snr, dtMax * (2 * g.u() - 1)});
            }
            wsjttest::Imp imp; imp.drift = drift; imp.dialHz = dial;
            auto a = wsjttest::audio(mode, st, 1, imp, 5000 + t * 13 + (int)(snr * 3 + 300));
            if (const char* w = getenv("BENCH_WAV")) { char nm[256]; snprintf(nm, sizeof nm, "%s/%s_%+.1f_%d", w, argv[2], snr, t); writeWav(nm, a, st); }
            auto d = wsjttest::decode(a, mode, imp);
            for (const auto& s : st) {
                all++;
                bool hit = false;
                for (const auto& x : d) if (x.msg == s.msg) { hit = true; break; }
                ok += hit;
                if (!hit && getenv("BENCH_V")) printf("  missed %.1f Hz dt %.2f \"%s\"\n", s.hz, s.dt, s.msg.c_str());
            }
            for (const auto& x : d) { bool sent = false; for (const auto& s : st) sent |= x.msg == s.msg; if (!sent) { fake++; printf("  FALSE \"%s\"\n", x.msg.c_str()); } }
        }
        printf("%-6s snr %6.1f  ok %3d/%d  false %d\n", argv[2], snr, ok, all, fake);
        fflush(stdout);
    }
    return 0;
}
// false decodes on pure noise: bench_hf noise <mode> [slots=20]
static int noise(int argc, char** argv) {
    const int mode = modeOf(argv[2]);
    const int slots = argc > 3 ? atoi(argv[3]) : 20;
    int fake = 0;
    for (int t = 0; t < slots; t++) {
        wsjttest::Imp imp;
        auto a = wsjttest::audio(mode, {}, 1, imp, 777 + 7919 * t);
        for (const auto& x : wsjttest::decode(a, mode, imp)) { fake++; printf("  FALSE \"%s\" %.0f Hz\n", x.msg.c_str(), x.hz); }
    }
    printf("%-6s noise slots %d  false %d\n", argv[2], slots, fake);
    return 0;
}
int main(int argc, char** argv) {
    if (argc < 2) { puts("bench_hf <ft8|ft4|ft2|wspr|jt65|jt9|q65|fst4|msk144|jt4> [stations=20] [slots=2] [snrLo=-18] [snrHi=-4]\nbench_hf sens <mode> [lo hi step slots K drift dtMax dialHz]\nbench_hf noise <mode> [slots]"); return 1; }
    if (!strcmp(argv[1], "sens") && argc > 2) return sens(argc, argv);
    if (!strcmp(argv[1], "noise") && argc > 2) return noise(argc, argv);
    const char** names = kNames;
    int mode = modeOf(argv[1]);
    if (mode < 0) return 1;
    const int N = argc > 2 ? atoi(argv[2]) : 20, slots = argc > 3 ? atoi(argv[3]) : 2;
    const double lo = argc > 4 ? atof(argv[4]) : -18, hi = argc > 5 ? atof(argv[5]) : -4;
    const double bw = mode <= 2 ? 2600 : mode == 3 ? 180 : 2400;
    const double f0 = mode == 3 ? 1420 : 300;
    std::vector<wsjttest::St> st;
    ftxtest::Gauss g(7);
    for (int i = 0; i < N; i++) {
        std::string a = callOf(i), b = callOf(i + 37);
        std::string msg;
        if (mode == 3) msg = a + " FN42 37";
        else switch (i % 3) { case 0: msg = "CQ " + a + " FN42"; break; case 1: msg = a + " " + b + " -" + (5 + i % 15 < 10 ? "0" : "") + std::to_string(5 + i % 15); break; default: msg = a + " " + b + " RR73"; }
        double hz = f0 + (N > 1 ? bw * i / N : 0) + (mode == 3 ? 0 : 3.0 * (i % 4));
        st.push_back({msg, hz, lo + (hi - lo) * g.u(), -0.3 + 0.6 * g.u()});
    }
    wsjttest::Imp imp;
    auto a = wsjttest::audio(mode, st, slots, imp);
    if (const char* w = getenv("BENCH_WAV")) { char nm[256]; snprintf(nm, sizeof nm, "%s/%s_busy%d", w, argv[1], N); writeWav(nm, a, st); }
    const double sig = (double)a.size() / 8000.0 - 4.0;
    const double c0 = cpuSec();
    auto d = wsjttest::decode(a, mode, imp);
    const double c1 = cpuSec();
    std::set<std::string> uniq;
    uint64_t h = 1469598103934665603ull;
    int right = 0;
    for (const auto& x : d) {
        char b[256]; snprintf(b, sizeof b, "%s|%.1f|%.2f|%.0f", x.msg.c_str(), x.hz, x.dt, x.snrDb);
        uniq.insert(b);
    }
    for (const auto& s : uniq) for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    for (const auto& s : st) for (const auto& x : d) if (x.msg == s.msg) { right++; break; }
    if (getenv("BENCH_LIST")) for (const auto& x : d) printf("  decoded \"%s\"\n", x.msg.c_str());
    if (getenv("BENCH_V"))
        for (const auto& x : d) { bool sent = false; for (const auto& s : st) sent |= x.msg == s.msg; if (!sent) printf("  not sent: \"%s\" %.0f Hz\n", x.msg.c_str(), x.hz); }
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    printf("%-6s N=%-3d slots=%d  cpu %.3f s  signal %.1f s  cost %.4f  decodes %zu (sent msgs decoded %d/%d)  digest %016llx  rss %.0f MB\n", names[mode], N, slots, c1 - c0, sig,
           (c1 - c0) / sig, d.size(), right, N, (unsigned long long)h, r.ru_maxrss / 1048576.0);
    return 0;
}
