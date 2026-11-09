// Auto for the WSJT sub-modes and periods (JT65, JT4, Q65, FST4, MSK144): the trial bank must decode a station it was not told the setting of. Simulated signals only.
#include "../hfdig/hfdig_wsjt_testutil.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2;

namespace {
struct Out { bool decoded; bool found; int sub; double period; float ms; };
Out run(int mode, int sub, double period, double snr, uint64_t seed) {
    Imp f; f.sub = sub; f.period = period;
    const auto a = audio(mode, {{"CQ K1ABC FN42", 1500, snr, 0.2}}, 3, f, seed);
    auto d = makeFtxDecoder();
    for (int o = 0; o < kFtxModes; o++) d->setEnabled(o, o == mode);
    d->setStartTime(kT0);
    d->setMirrored(false);
    d->setAutoOption(mode, true);
    d->reset();
    d->setStartTime(kT0);
    for (size_t i = 0; i < a.size(); i += 4000) d->feedAudio(&a[i], std::min<size_t>(4000, a.size() - i));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    HfdigFtxTelemetry t;
    d->flushSlots();
    d->feedAudio(a.data(), 1);
    d->waitIdle();
    // the children decode on their own threads: give them time
    for (int k = 0; k < 40; k++) { d->telemetry(t); if (t.autoFound[mode]) break; std::this_thread::sleep_for(std::chrono::milliseconds(200)); d->feedAudio(a.data(), 16000); }
    d->telemetry(t);
    Out o;
    o.decoded = t.perMode[mode] > 0; o.found = t.autoFound[mode]; o.sub = t.autoSub[mode]; o.period = t.autoPeriod[mode]; o.ms = t.decodeMs[mode];
    return o;
}
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0, n = 0;
    auto one = [&](const char* name, int m, int s, double p, double snr, uint64_t seed) {
        const Out o = run(m, s, p, snr, seed);
        n++;
        const bool hit = o.found && o.sub == s && (p == 0 || std::fabs(o.period - p) < 0.5);
        ok += hit;
        printf("  %-10s sub %d period %-5g at %5.0f dB: found %d, sub %d, period %g, decode time %.0f ms\n", name, s, p, snr, (int)o.found, o.sub, o.period, o.ms);
    };
    for (int s = 0; s < 3; s++) one("JT65", ftx::kJt65, s, 60, -10, (uint64_t)(100 + s));
    for (int s = 0; s < 7; s += 3) one("JT4", ftx::kJt4, s, 0, -8, (uint64_t)(200 + s));
    for (double p : {15.0, 30.0, 60.0}) for (int s = 0; s < 5; s += 2) one("Q65", ftx::kQ65, s, p, -10, (uint64_t)(300 + s + (int)p));
    for (double p : {15.0, 30.0, 60.0}) one("FST4", ftx::kFst4, 0, p, -8, (uint64_t)(400 + (int)p));
    one("MSK144", ftx::kMsk144, 0, 15, -2, 501);
    printf("trial bank: %d of %d found the right setting, SNR in 2500 Hz as WSJT-X\n", ok, n);
    CHECK(ok * 100 >= n * 80, "trial bank %d of %d", ok, n);
    // noise alone
    {
        Imp f;
        for (int m : {ftx::kQ65, ftx::kJt65}) {
            auto a = audio(m, {}, 3, f, 77);
            auto d = makeFtxDecoder();
            for (int o = 0; o < kFtxModes; o++) d->setEnabled(o, o == m);
            d->setStartTime(kT0);
            d->setAutoOption(m, true);
            d->reset();
            d->setStartTime(kT0);
            for (size_t i = 0; i < a.size(); i += 4000) d->feedAudio(&a[i], std::min<size_t>(4000, a.size() - i));
            d->feedAudio(a.data(), 1);
            d->waitIdle();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            HfdigFtxTelemetry t; d->telemetry(t);
            printf("noise alone (%s): decoded %llu\n", ftxModeName(m), (unsigned long long)t.perMode[m]);
            CHECK(t.perMode[m] == 0, "noise decode");
        }
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autoparam_ftx");
}
