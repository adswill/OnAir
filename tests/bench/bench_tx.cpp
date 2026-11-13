// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-mode cost benchmark of the TRANSMIT generators (makeTxGenerator): CPU seconds (user + system of this process) per second of signal made, and a hash
// of the first samples so that a speed-up can be shown to leave the signal bit-identical.
//
//   bench_tx [--list] [--mode id[,id..]] [--seconds N] [--hash-seconds N] [--rate Msps] [--session]
//
// One line per mode: id, rate (Msps), cost (CPU s per signal s), real-time factor, FNV-1a hash of the raw float bits of the first --hash-seconds
// of output (default 0.5). Nothing is transmitted; no radio is opened. Run one mode per process (--mode) when a generator may crash.
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <set>
#include <thread>
#include <string>
#include <vector>
using namespace dect2;

static double cpuSeconds() {
    timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
    double seconds = 2, hashSeconds = 0.5, rateMsps = 0;
    std::set<std::string> only;
    bool list = false, session = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--hash-seconds" && i + 1 < argc) hashSeconds = atof(argv[++i]);
        else if (a == "--session") session = true;
        else if (a == "--rate" && i + 1 < argc) rateMsps = atof(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t p = 0;
            while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); only.insert(s.substr(p, q - p)); p = q + 1; }
        }
    }
    for (const auto& m : txModes()) {
        if (list) { printf("%s\n", m.id); continue; }
        if (!only.empty() && !only.count(m.id)) continue;
        TxRequest r;
        r.modeId = m.id;
        r.synth.mode = m.stdMode;
        std::string err;
        if (session) {
            TxSession s;
            const double c0s = cpuSeconds();
            if (!s.start(makeNullTxSink(), r, TxRateLimits{}, err)) { printf("%-10s no session: %s\n", m.id, err.c_str()); continue; }
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            const auto st = s.stats();
            s.stop();
            printf("%-10s %6.3f  sent %.1f s  gaps %llu  cpu %.2f s per s\n", m.id, st.rateHz / 1e6, st.seconds, (unsigned long long)st.underruns, (cpuSeconds() - c0s) / seconds);
            continue;
        }
        const double rate = rateMsps > 0 ? rateMsps * 1e6 : txRequestRate(m, r, TxRateLimits{});
        auto g = makeTxGenerator(m, r, rate, err);
        if (!g) { printf("%-10s %6.3f  no generator: %s\n", m.id, rate / 1e6, err.c_str()); continue; }
        const size_t block = 1 << 15;
        std::vector<cf32> buf(block);
        uint64_t h = 1469598103934665603ull;
        uint64_t made = 0;
        const uint64_t hashN = (uint64_t)(hashSeconds * rate), total = (uint64_t)(seconds * rate);
        const double c0 = cpuSeconds();
        double worstMs = 0;
        while (made < std::max(total, hashN)) {
            const auto w0 = std::chrono::steady_clock::now();
            g->generate(buf.data(), block);
            worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
            if (made < hashN) {
                const size_t k = (size_t)std::min<uint64_t>(block, hashN - made);
                const unsigned char* p = reinterpret_cast<const unsigned char*>(buf.data());
                for (size_t i = 0; i < k * sizeof(cf32); i++) { h ^= p[i]; h *= 1099511628211ull; }
            }
            made += block;
        }
        const double cpu = cpuSeconds() - c0, sig = (double)made / g->sampleRate();
        printf("%-10s %6.3f  cost %8.4f  rt %7.1fx  worst block %6.1f ms (budget %5.1f)  hash %016llx\n", m.id, g->sampleRate() / 1e6, cpu / sig, sig / cpu, worstMs, (double)block / g->sampleRate() * 1e3, (unsigned long long)h);
        fflush(stdout);
    }
    return 0;
}
