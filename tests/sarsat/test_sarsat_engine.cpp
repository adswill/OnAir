// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT through the engine at 0.5 Msps (an RTL-SDR / RSPduo style start): which receivers log at start, how much CPU the engine uses next to the
// bare receiver, and that the generated beacons are decoded with the tuner 20 kHz off the dial.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include <ctime>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/cpu_time.h"

#ifdef _WIN32
static double cpuSec() { return processCpuSecs(); }   // std::clock() is the wall clock on Windows: a busy runner read 1.000 s/s
#else
static double cpuSec() { rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6; }
#endif

int main() {
    const ModeTuning* mt = modeTuningById("sarsat");
    CHECK(mt, "tuning");
    if (!mt) return 1;
    const double rate = 500000;
    // bare receiver, free running: CPU seconds per second of signal
    {
        SynthConfig cfg; cfg.snrDb = 30;
        SarsatReceiver rx; rx.configure(rate); rx.setSignalOffset(-mt->tuneOffsetHz);
        auto syn = makeSarsatSynth(cfg, rate);
        std::vector<cf32> buf(32768);
        const double c0 = cpuSec(); double sig = 0;
        while (sig < 20) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); sig += buf.size() / rate; }
        printf("RX-ONLY cpu %.3f s per s of signal\n", (cpuSec() - c0) / sig);
    }
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    TuneSettings tune;
    tune.centerHz = mt->defMhz * 1e6;
    tune.sampleRate = rate;
    tune.bandwidthMhz = mt->bandwidthMhz;
    tune.basebandFilterHz = mt->basebandHz;
    tune.synth.mode = mt->stdMode;
    tune.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(mt->stdMode);
    const double c0 = cpuSec();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(e.start(dev, tune, fo), "engine started");
    RxTelemetry t, last; uint64_t seq = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (e.latestRx(t, seq)) { seq = t.seq; last = t; }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = cpuSec() - c0;
    e.stop();
    size_t total = 0;
    printf("ENGINE LOG:\n");
    for (const auto& l : e.logSnapshot(total)) printf("  | %s\n", l.c_str());
    printf("ENGINE cpu %.3f s per s of wall (%.1f s wall), beacons ok %llu, bursts %llu\n", cpu / wall, wall, (unsigned long long)last.sarsat.blocksOk, (unsigned long long)last.sarsat.bursts);
    CHECK(last.sarsat.blocksOk >= 5, "the generated beacons are decoded at 0.5 Msps with the tuner 20 kHz off the dial (%llu)", (unsigned long long)last.sarsat.blocksOk);
    CHECK(cpu / wall < 0.30, "the engine's CPU in SARSAT mode at 0.5 Msps stays under 0.30 s/s (was %.3f; the receiver alone is 0.07 on the CI runner)", cpu / wall);

    // a band full of carriers: steady tones that are not beacons fire the burst detector all the time; each used to run the whole search
    for (int guarded = 0; guarded < 2; guarded++) {
        SarsatReceiver rx; rx.configure(rate); rx.setSignalOffset(-mt->tuneOffsetHz);
        rx.setFalseBurstBudget(guarded ? 1.0 : 1e9);
        std::vector<cf32> buf(32768);
        unsigned q = 99; double ph = 0; uint64_t k = 0;
        const double c1 = cpuSec();
        const double secs = 30;
        for (double done = 0; done < secs * rate; done += (double)buf.size()) {
            for (auto& v : buf) {
                q = q * 1664525u + 1013904223u; const float a = (((q >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.05f;
                q = q * 1664525u + 1013904223u; const float b = (((q >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.05f;
                const double t = (double)(k++) / rate, cyc = std::fmod(t, 0.9);
                cf32 sig(0, 0);
                if (cyc < 0.45) { ph += 2 * 3.14159265358979 * (20000.0 + 3000.0 * std::floor(t / 0.9 / 3.0)) / rate; sig = cf32((float)std::cos(ph) * 0.05f, (float)std::sin(ph) * 0.05f); }
                v = cf32(a, b) + sig;
            }
            rx.feed(buf.data(), buf.size());
        }
        printf("FALSE-BURST STORM %s: cpu %.3f s per s of signal, skipped %llu\n", guarded ? "with the guard" : "no guard", (cpuSec() - c1) / secs, (unsigned long long)rx.falseBurstsSkipped());
        if (guarded) CHECK((cpuSec() - c1) / secs < 0.25, "a storm of steady tones stays under 0.25 s of CPU per s of signal");
    }

    // every restart after the first used to print the start-up line of every other mode (Balise, nRF24, APRS, Sensors, ...) into the log
    {
        std::vector<std::string> names;
        for (int sm = 1; sm < 1200; sm++) if (const ModeTuning* m = modeTuning(sm)) if (m->name && *m->name) names.push_back(m->name);
        const char* ids[] = {"sarsat", "tones", "packet", "sensors", "hfbeacon", "iono", "roshf", "ssdv", "ebicab", "nrf24", "specx", "astro", "dmr", "adsb", "cw", "psk"};
        int checked = 0;
        for (const char* id : ids) {
            const ModeTuning* m = modeTuningById(id);
            if (!m) continue;
            Engine en;
            DeviceInfo d; d.kind = DeviceInfo::Synthetic; d.name = "synthetic";
            TuneSettings ts;
            ts.centerHz = m->defMhz * 1e6; ts.sampleRate = std::max(m->sampleRate, 500000.0); ts.bandwidthMhz = m->bandwidthMhz; ts.basebandFilterHz = m->basebandHz;
            ts.synth.mode = 99;
            en.setStandard(m->stdMode);
            for (int run = 0; run < 3; run++) {
                en.start(d, ts, fo);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                en.stop();
            }
            size_t tot = 0;
            std::string bad;
            for (const auto& l : en.logSnapshot(tot)) {
                const size_t at = l.find("s ");
                const std::string body = at == std::string::npos ? l : l.substr(at + 2);
                for (const std::string& nm : names)
                    if (nm != m->name && body.compare(0, nm.size() + 1, nm + ":") == 0) { bad += "  | " + l + "\n"; break; }
            }
            CHECK(bad.empty(), "%s: lines of other modes in the log after restarts:\n%s", id, bad.c_str());
            checked++;
        }
        printf("RESTART LOG checked %d modes\n", checked);
        CHECK(checked >= 8, "the restart check covered %d modes", checked);
    }
    return fails ? 1 : 0;
}
