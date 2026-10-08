// Multipath detector: a clean synthetic channel must read "none"; a strong echo must be found, with the right delay.
#include "dect2/channel.h"
#include "dect2/engine.h"
#include "jobs.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdint>
#include <cstdlib>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// The signal runs at half speed, so a machine that decodes DVB-T2 at half of real time still passes: the result must not depend on CPU speed.
static const double kPace = 1.0;

static MultipathReport run(double echoDb, int echoDelay, double secs) {
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.synth.snrDb = 30;
    t.synth.tx.s2field1 = 1;   // 8K
    t.synth.tx.giIdx = 3;      // 1/8
    t.synth.echoDb = echoDb;
    t.synth.echoDelay = echoDelay;
    t.synth.pace = kPace;
    FileOptions fo;
    e.setStandard(1);   // DVB-T2 only: no time lost on the DVB-T search, so the result does not depend on how fast the machine is
    e.start(dev, t, fo);
    MultipathDetector md;
    RxTelemetry rx; uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs / kPace) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; md.update(rx); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.stop();
    return md.report();
}

static void print(const char* name, const MultipathReport& r) {
    printf("%s: %s (%s); guard %.1f us\n", name, multipathName(r.level), r.headline.c_str(), r.guardUs);
    for (auto& x : r.echoes) printf("   echo %.1f dB at %+.2f us %s\n", x.levelDb, x.delayUs, x.insideGuard ? "" : "(outside guard)");
    for (auto& s : r.reasons) printf("   - %s\n", s.c_str());
}

int main() {
    // the two engines are independent: run them side by side
    MultipathReport clean, echo;
    testjobs::Jobs jobs;
    jobs.add([&] { clean = run(0, 300, 12); });
    jobs.add([&] { echo = run(4, 100, 12); }); // echo 4 dB below the main path, 100 samples (~11 us) later, inside the 1/8 guard
    jobs.run(2);
    print("clean", clean);
    CHECK(clean.level == MultipathLevel::None || clean.level == MultipathLevel::Mild, "clean channel flagged as %s", multipathName(clean.level));
    CHECK(clean.echoes.empty() || clean.echoes[0].levelDb < -15, "clean channel shows a strong echo");

    print("echo -4 dB @ 100 samples", echo);
    CHECK(echo.level >= MultipathLevel::Likely, "strong echo not detected (%s)", multipathName(echo.level));
    CHECK(!echo.echoes.empty(), "no echo reported");
    if (!echo.echoes.empty()) {
        const double expectUs = 100 * 1e6 / (64e6 / 7.0);
        CHECK(std::abs(std::abs(echo.echoes[0].delayUs) - expectUs) < 0.6, "echo delay %.2f us, expected %.2f", echo.echoes[0].delayUs, expectUs);
        CHECK(echo.echoes[0].levelDb > -8 && echo.echoes[0].levelDb < -1, "echo level %.1f dB, expected about -4", echo.echoes[0].levelDb);
    }
    printf(fails ? "channel tests FAILED\n" : "channel tests passed\n");
    return fails ? 1 : 0;
}
