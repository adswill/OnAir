// The interface keeps the number of the last receiver report it took for the whole session (Engine::latestRx). After a run of another mode
// that number is high, while each receiver counts its own reports from 1: the analog TV reports must still come through at once.
#include "dect2/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    Engine e;
    DeviceInfo dev;
    FileOptions fo;
    uint64_t seq = 0;   // as App::rxSeq: never reset
    auto runFor = [&](int std, TuneSettings t, double secs, int& reports, int& firstStd) {
        e.setStandard(std);
        CHECK(e.start(dev, t, fo), "start %d", std);
        reports = 0; firstStd = -1;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
            RxTelemetry rx;
            if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == std - 1) { reports++; if (firstStd < 0) firstStd = (int)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000); } }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        e.stop();
    };
    TuneSettings g; g.centerHz = 1575.42e6; g.bandwidthMhz = 2.046; g.sampleRate = 4e6; g.synth.mode = 14;
    TuneSettings a; a.centerHz = 600e6; a.bandwidthMhz = 8; a.sampleRate = 10e6; a.synth.mode = 10;
    int n1, f1, n2, f2, n3, f3;
    runFor(14, g, 6.0, n1, f1);
    runFor(10, a, 3.0, n2, f2);
    runFor(10, a, 3.0, n3, f3);   // the same mode again: its receiver starts counting again too
    printf("GNSS %d reports; analog TV %d reports, the first after %d ms; again %d reports, the first after %d ms\n", n1, n2, f2, n3, f3);
    CHECK(n1 >= 15, "GNSS reports %d", n1);
    CHECK(n2 >= 8 && f2 >= 0 && f2 < 1000, "analog TV after GNSS: %d reports, first after %d ms", n2, f2);
    CHECK(n3 >= 8 && f3 >= 0 && f3 < 1000, "analog TV again: %d reports, first after %d ms", n3, f3);
    printf(fails ? "atv switch: %d FAILED\n" : "atv switch: all passed\n", fails);
    return fails ? 1 : 0;
}
