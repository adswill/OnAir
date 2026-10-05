// Front-end load: how much of real time does the receive path need for a heavy DVB-T2 mode (16K extended, GI 1/4)?
//   bench_rx [seconds] [fft code 0..5] [ext 0|1] [gi idx] [pace] [hog threads]   (pace > 1 feeds the signal faster than real time: an overloaded receiver)
#include "dect2/engine.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>
using namespace dect2;
int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 15;
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.synth.snrDb = 30;
    t.synth.tx.s2field1 = argc > 2 ? atoi(argv[2]) : 4; // 16K
    t.synth.tx.ext = argc > 3 ? atoi(argv[3]) != 0 : true;
    t.synth.tx.giIdx = argc > 4 ? atoi(argv[4]) : 3;     // 1/4
    t.synth.pace = argc > 5 ? atof(argv[5]) : 1.0;
    FileOptions fo;
    std::atomic<bool> stopHogs{false};
    std::vector<std::thread> hogs;   // competing CPU load, to starve the receiver the way a busy machine does
    for (int i = 0, n = argc > 6 ? atoi(argv[6]) : 0; i < n; i++) hogs.emplace_back([&] { volatile double x = 1; while (!stopHogs) x = x * 1.0000001 + 1e-9; });
    e.start(dev, t, fo);
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
    printf("%s\n%s\n", e.loadProfile().c_str(), t2rxProfile().c_str());
    RxTelemetry rx; uint64_t seq = 0;
    e.latestRx(rx, seq);
    printf("L1-post decoded %llu / %llu, data frames %llu, dropped %llu\n", (unsigned long long)rx.l1postGood, (unsigned long long)(rx.l1postGood + rx.l1postBad), (unsigned long long)rx.dataFrames, (unsigned long long)e.droppedSamples());
    e.stop();
    stopHogs = true;
    for (auto& h : hogs) h.join();
    size_t tot = 0;
    for (auto& l : e.logSnapshot(tot)) if (l.find("CPU load") != std::string::npos) printf("%s\n", l.c_str());
}
