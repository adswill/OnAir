// Inmarsat Aero receiver at the radios' sample rates (0.25, 2.4, 8, 10 Msps), with all three channel rates in band (600 bit/s too),
// and fed in chunks of 1, 7 and 65536 samples: the channels are found, the rate is found, the frames are bit-exact.
#include "dect2/aero_gen.h"
#include "dect2/aero_rx.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(const char* name, double rate, int mask, double secs, size_t chunk, double ebn0 = 0) {
    SynthConfig cfg;
    cfg.mode = 20;
    cfg.modeOpt[0] = mask;
    cfg.modeOpt[1] = 5;
    cfg.modeVal[0] = ebn0;
    AeroSynth syn(cfg, rate);
    syn.record(true);
    AeroReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
    CHECK(rx.ready(), "%s: ready", name);
    std::set<std::vector<uint8_t>> got;
    size_t damaged = 0;
    rx.setFrameCallback([&](double, const AeroFrameEvent& e) { if (e.susBad == 0) got.insert(e.bytes); else damaged++; });
    std::vector<cf32> buf(65536);
    AeroTelemetry t;
    double feedSecs = 0;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        syn.generate(buf.data(), buf.size());
        const std::clock_t c0 = std::clock();          // processor time: the machine may be busy with other work
        for (size_t i = 0; i < buf.size(); i += chunk) rx.feed(buf.data() + i, std::min(chunk, buf.size() - i));
        feedSecs += (double)(std::clock() - c0) / CLOCKS_PER_SEC;
    }
    rx.telemetry(t, 0);
    const auto sent = syn.takeFrames();
    std::set<std::vector<uint8_t>> sentSet;
    for (const auto& f : sent) sentSet.insert(f.bytes);
    size_t exact = 0;
    for (const auto& g : got) exact += sentSet.count(g);
    printf("%-28s feed %.2f s of processor time for %.0f s of signal (%.0fx real time)\n", name, feedSecs, secs, secs / feedSecs);
    int found = 0;
    for (const auto& sc : syn.channels()) {
        for (const auto& c : t.channels) {
            if (c.bitRate != sc.bitRate || c.state != 3 || std::fabs(c.offsetHz - sc.offsetHz) > 4000) continue;
            found++;
            printf("    %5d bit/s %+9.1f Hz  Eb/N0 %4.1f dB  frames %3llu  SUs %4llu/%llu\n", c.bitRate, c.offsetHz, c.ebn0Db, (unsigned long long)c.frames, (unsigned long long)c.susOk,
                   (unsigned long long)c.susBad);
            CHECK(ebn0 ? c.susBad * 50 <= c.susOk : c.susBad == 0, "%s: rate %d bad SUs", name, c.bitRate);
        }
    }
    printf("    frames %zu exact %zu damaged %zu; carriers seen %zu\n", got.size(), exact, damaged, t.channels.size());
    for (const auto& c : t.channels) if (c.state < 3) printf("    (carrier %+9.1f Hz level %.1f dB state %d)\n", c.offsetHz, c.levelDb, c.state);
    CHECK(found == (int)syn.channels().size(), "%s: %d of %zu channels", name, found, syn.channels().size());
    CHECK(exact == got.size() && got.size() >= 6 && (ebn0 || damaged == 0), "%s: %zu of %zu exact, %zu damaged", name, exact, got.size(), damaged);
    CHECK(t.channels.size() == syn.channels().size(), "%s: %zu carriers for %zu channels", name, t.channels.size(), syn.channels().size());
}

int main(int argc, char** argv) {
    if (argc > 1) { run("one rate", std::atof(argv[1]) * 1e6, 6, argc > 2 ? std::atof(argv[2]) : 10, 65536); return fails ? 1 : 0; }   // for profiling
    run("0.25 Msps", 250e3, 6, 12, 4096);
    run("2.4 Msps", 2.4e6, 6, 12, 4096);
    run("8 Msps", 8e6, 6, 8, 65536);
    run("10 Msps", 10e6, 6, 8, 65536);
    run("2 Msps, 600+1200+10500 at 6 dB", 2e6, 7, 24, 4096, 6);
    run("2 Msps, chunks of 7", 2e6, 6, 10, 7);
    run("1 Msps, chunks of 1", 1e6, 6, 10, 1);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
