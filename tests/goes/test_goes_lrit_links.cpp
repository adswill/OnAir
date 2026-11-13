// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The other xRIT links through the GOES HRIT chain: the GOES-N / O / P LRIT broadcast (BPSK 293.883 ksym/s, 1691.0 MHz) and every Elektro-L HRIT / LRIT link
// (665.4 k, 1 M, 128 k, 64 k, 294 k BPSK and 1.15 Msym/s OQPSK) through ElektroReceiver::setLink, with carrier and clock offsets and noise. Every file sent
// must come back byte for byte. A receiver set to the wrong rate must not decode. Simulated signals only: no GOES-N LRIT or Elektro-L recording was found.
#include "dect2/goes/goes_gen.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/elektro/elektro_gen.h"
#include "dect2/elektro/elektro_rx.h"
#include "dect2/elektro/elektro_tel.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

struct Res { GoesTelemetry t; size_t files = 0, exact = 0; };

static Res runGoes(double rate, double symRate, double rxSymRate, double snr, double cfo, double ppm, double seconds, bool oqpsk = false) {
    GoesGenConfig g;
    g.rate = rate; g.symbolRate = symRate; g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.fillFrames = 4; g.seed = 9;
    g.qpsk = g.oqpsk = oqpsk;
    GoesGenerator gen(g);
    GoesReceiver rx;
    rx.setSymbolRate(rxSymRate);
    rx.setOqpsk(oqpsk);
    rx.configure(rate);
    Res r;
    const std::vector<GoesTestFile> tf = goesTestFiles();
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { r.files++; for (const auto& x : tf) if (f == x.file) { r.exact++; break; } });
    std::vector<cf32> buf(32768);
    uint64_t seq = 0;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(r.t, seq)) seq = r.t.seq;
    }
    return r;
}

// synth: the app's Elektro test signal (makeElektroSynth, which sends the files once and then fill frames); otherwise the files in a loop
static Res runElektro(int link, double rate, double snr, double cfo, double ppm, double seconds, bool synth = false) {
    SynthConfig sc;
    sc.snrDb = snr; sc.cfoHz = cfo; sc.sroPpm = ppm; sc.modeOpt[0] = link;
    std::unique_ptr<ModeSynth> syn;
    std::unique_ptr<GoesGenerator> gen;
    if (synth) syn = makeElektroSynth(sc, rate);
    else {
        GoesGenConfig g;
        g.rate = rate; g.symbolRate = elektroSymbolRate(link); g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.fillFrames = 4; g.seed = 9;
        g.qpsk = g.oqpsk = link == 6;
        gen = std::make_unique<GoesGenerator>(g);
    }
    ElektroReceiver rx;
    rx.setLink(link);
    rx.configure(rate);
    Res r;
    const std::vector<GoesTestFile> tf = goesTestFiles();
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { r.files++; for (const auto& x : tf) if (f == x.file) { r.exact++; break; } });
    std::vector<cf32> buf(32768);
    uint64_t seq = 0;
    ElektroTelemetry et;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (syn) syn->generate(buf.data(), buf.size()); else gen->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(et, seq)) { seq = et.hrit.seq; }
    }
    r.t = et.hrit;
    return r;
}

static void expectAll(const char* name, const Res& r) {
    const size_t want = goesTestFiles().size();
    printf("  %s: %s; Es/N0 %.1f dB, sym %.0f, frames %llu ok %llu bad, %zu files (%zu exact)\n", name, goesSummary(r.t).c_str(), r.t.snrDb, r.t.symbolRate,
           (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, r.files, r.exact);
    CHECK(r.t.state == 3, "%s: state %d", name, r.t.state);
    CHECK(r.files >= want && r.exact == r.files, "%s: %zu files, %zu exact, %zu sent", name, r.files, r.exact, want);
    CHECK(r.t.crcErrors == 0 && r.t.fileErrors == 0, "%s: CRC %llu file errors %llu", name, (unsigned long long)r.t.crcErrors, (unsigned long long)r.t.fileErrors);
}

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;   // the 2.4 Msps cases, the 64 ksym/s link and the noise sweep
    // ---- GOES-N / O / P LRIT: 293.883 ksym/s
    const double lrit = 293883.0;
    expectAll("GOES LRIT 1 Msps, Es/N0 6 dB, +9 kHz, +40 ppm", runGoes(1e6, lrit, lrit, 6, 9e3, 40, 5.5));
    if (longRun) {
        expectAll("GOES LRIT 2.4 Msps, Es/N0 5 dB, -20 kHz, -30 ppm", runGoes(2.4e6, lrit, lrit, 5, -20e3, -30, 6.0));
        expectAll("GOES LRIT 2.4 Msps, Es/N0 30 dB, on the dial", runGoes(2.4e6, lrit, lrit, 30, 0, 0, 6.0));
    }
    {   // the HRIT rate set while LRIT is on the air (and the other way round): nothing decodes
        const Res a = runGoes(1e6, lrit, 927e3, 20, 0, 0, 1.5);
        CHECK(a.t.blocksOk == 0 && a.files == 0, "LRIT signal, HRIT setting: %llu frames", (unsigned long long)a.t.blocksOk);
        const Res b = runGoes(2.4e6, 927e3, lrit, 20, 0, 0, 0.6);
        CHECK(b.t.blocksOk == 0 && b.files == 0, "HRIT signal, LRIT setting: %llu frames", (unsigned long long)b.t.blocksOk);
    }
    // ---- Elektro-L: every HRIT / LRIT link through ElektroReceiver
    struct Ek { int link; double rate, snr, cfo, ppm, secs; const char* name; };
    const Ek ek[] = {
        {0, 2.4e6, 7, 15e3, 25, 2.5, "Elektro HRIT 665.4 ksym/s"},
        {1, 4.0e6, 7, -30e3, -20, 2.0, "Elektro HRIT 1 Msym/s"},
        {2, 512e3, 7, 3e3, 40, 12.0, "Elektro LRIT 128 ksym/s"},
        {3, 256e3, 7, -2e3, -40, 22.0, "Elektro LRIT 64 ksym/s"},
        {5, 1.0e6, 6, 8e3, 30, 6.0, "Elektro LRIT 294 ksym/s"},
        {6, 4.0e6, 9, 20e3, 15, 2.0, "Elektro HRIT 1.15 Msym/s OQPSK"},
    };
    for (const auto& e : ek) {
        if (e.link == 3 && !longRun) continue;
        const Res r = runElektro(e.link, e.rate, e.snr, e.cfo, e.ppm, e.secs);
        expectAll(e.name, r);
        CHECK(std::fabs(r.t.symbolRate - elektroSymbolRate(e.link)) < 0.002 * elektroSymbolRate(e.link), "%s: symbol rate %.0f", e.name, r.t.symbolRate);
    }
    {   // the app's test signal follows the link: it locks and decodes frames at the link's rate
        const Res r = runElektro(5, 1.0e6, 10, 0, 0, 2.0, true);
        CHECK(r.t.state == 3 && r.t.blocksOk > 20 && std::fabs(r.t.symbolRate - 294000) < 600, "Elektro test signal on the 294 ksym/s link: state %d, %llu frames", r.t.state, (unsigned long long)r.t.blocksOk);
    }
    // ---- noise sweep on the GOES LRIT link: the lowest Es/N0 that still brings every file
    if (longRun) {
        double lowest = 99;
        for (double snr : {8.0, 6.0, 5.0, 4.0, 3.0}) {
            const Res r = runGoes(1e6, lrit, lrit, snr, 4e3, 20, 6.0);
            const bool ok = r.files >= goesTestFiles().size() && r.exact == r.files;
            printf("  GOES LRIT at Es/N0 %.0f dB: %s (%llu frames, %llu bad)\n", snr, ok ? "all files" : "incomplete", (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
            if (ok) lowest = snr;
            if (snr >= 5) CHECK(ok, "GOES LRIT lost at Es/N0 %.0f dB", snr);
        }
        printf("  GOES LRIT: every file down to Es/N0 %.0f dB\n", lowest);
    }
    return testSummary("goes lrit links");
}
