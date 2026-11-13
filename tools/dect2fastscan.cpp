// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fast scan harness, receive only:
//   dect2fastscan --from MHz --to MHz [--mode fw|retune|both] [--passes n] [--lna dB] [--vga dB] [--amp] [--step kHz] [--dwell ms] [--thr dB] [--top n]
// Runs the fast scanner on the first radio. fw: the HackRF firmware sweep (libhackrf hackrf_init_sweep); retune: the ordinary retune loop. Prints
// the measured seconds per pass and MHz/s, then the confirmed channels (strongest first) so known signals (FM broadcast, GSM / LTE downlinks,
// ADS-B at 1090 MHz) can be checked against the list.
#include "dect2/engine/engine.h"
#include "dect2/fastscan/fwsweep.h"
#include "dect2/fastscan/profile.h"
#include "dect2/sweep/sweep.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2;
using namespace dect2::fastscan;

static void report(FastScanner& sc, const char* tag, double wallSec, int top) {
    const ScanStats st = sc.stats();
    printf("[%s] %llu passes, last pass %.2f s (%.0f MHz/s), mean pass %.2f s over %.1f s wall, %d hops/pass\n", tag, (unsigned long long)st.passes, st.passSec, st.mhzPerSec,
           st.passes ? wallSec / (double)st.passes : 0.0, wallSec, (int)sc.plan().hops.size());
    std::vector<ChannelInfo> ch = sc.channels(true);
    std::sort(ch.begin(), ch.end(), [](const ChannelInfo& a, const ChannelInfo& b) { return a.hit.peakDb > b.hit.peakDb; });
    printf("[%s] %zu confirmed channels (%zu raw); strongest:\n", tag, ch.size(), sc.channels(false).size());
    for (int i = 0; i < (int)ch.size() && i < top; i++)
        printf("   %10.4f MHz  bw %6.1f kHz  peak %6.1f dBFS  snr %5.1f dB  %s\n", ch[i].hit.freqHz / 1e6, ch[i].hit.bwHz / 1e3, ch[i].hit.peakDb, ch[i].hit.snrDb, ch[i].hint.c_str());
}

int main(int argc, char** argv) {
    double from = 88, to = 108, stepK = -1, dwell = -1, thr = -1;
    std::string mode = "fw";
    int passes = 3, top = 25, lna = 32, vga = 20;
    bool amp = true;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--from") from = nx(); else if (a == "--to") to = nx(); else if (a == "--passes") passes = (int)nx(); else if (a == "--top") top = (int)nx();
        else if (a == "--lna") lna = (int)nx(); else if (a == "--vga") vga = (int)nx(); else if (a == "--amp") amp = true; else if (a == "--noamp") amp = false;
        else if (a == "--step") stepK = nx(); else if (a == "--dwell") dwell = nx(); else if (a == "--thr") thr = nx();
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    }
    std::string err;
    auto list = listRadios(err);
    if (list.empty()) { fprintf(stderr, "no radio found\n"); return 1; }
    const DeviceInfo dev = list[0];
    const RadioProfile radio = profileForBoard(dev.board + " " + dev.name, dev.maxRateHz);
    printf("radio: %s %s (profile %s)\n", dev.board.c_str(), dev.name.c_str(), radio.id.c_str());
    ScanProfile p = wholeSpanProfile(radio);
    char b[64]; snprintf(b, sizeof b, "%g-%g", from, to);
    p.bands = b;
    const AutoScan a = autoScan(radio, (to - from) * 1e6);
    p.stepKhz = stepK > 0 ? stepK : a.stepKhz; p.dwellMs = dwell > 0 ? dwell : a.dwellMs; p.thresholdDb = thr > 0 ? thr : a.thresholdDb;
    p.minDurSec = 0; p.hangSec = 3;
    const bool both = mode == "both";
    int rc = 0;
    if ((mode == "fw" || both) && radio.id == "hackrf") {
        FastScanConfig cfg = toConfig(p);
        const FwSweepPlan fp = planFirmwareSweep(cfg.plan, radio);
        cfg.plan = fp.plan;
        RadioProfile r20 = radio; r20.rateHz = 20e6;
        FastScanner sc;
        HackrfFwSweep src;
        if (!sc.configure(cfg, r20, err)) { fprintf(stderr, "configure: %s\n", err.c_str()); return 1; }
        if (!src.open(fp, lna, vga, amp, "", err)) { fprintf(stderr, "firmware sweep unavailable: %s\n", err.c_str()); rc = 2; }
        else {
            RunHooks hk; hk.maxPasses = passes;
            const auto t0 = std::chrono::steady_clock::now();
            sc.runBlocks(src, nullptr, hk, err);
            const double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            src.close();
            if (!err.empty()) fprintf(stderr, "run: %s\n", err.c_str());
            printf("[fw] sweep %u-%u MHz, %llu blocks dropped\n", fp.loMhz, fp.hiMhz, (unsigned long long)src.dropped());
            report(sc, "fw", w, top);
        }
    }
    if (mode == "retune" || both || rc == 2) {
        FastScanConfig cfg = toConfig(p);
        FastScanner sc;
        RadioProfile r = radio;
        TuneSettings t;
        t.sampleRate = r.rateHz; t.bandwidthMhz = r.rateHz / 1e6; t.centerHz = from * 1e6;
        if (dev.isGeneric()) t.gainDb = 30; else { t.lnaDb = lna; t.vgaDb = vga; t.ampOn = amp; }
        sweep::IqSweepSource src(dev, t);
        if (!src.open(err)) { fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
        const double st = sweep::calibrateSettle(src, from * 1e6, to * 1e6, err);
        if (st > 0) { cfg.plan.settleSec = st; printf("[retune] settle measured %.1f ms\n", st * 1e3); }
        if (!sc.configure(cfg, r, err)) { fprintf(stderr, "configure: %s\n", err.c_str()); return 1; }
        RunHooks hk; hk.maxPasses = passes;
        const auto t0 = std::chrono::steady_clock::now();
        sc.run(src, nullptr, hk, err);
        const double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        src.close();
        if (!err.empty()) fprintf(stderr, "run: %s\n", err.c_str());
        report(sc, "retune", w, top);
    }
    return rc == 2 ? 0 : rc;
}
