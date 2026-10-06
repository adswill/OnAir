// DVB-T receiver load: how much of real time does the receive path need for a given mode?
//   bench_dvbt [frames] [mode 0=2K 1=8K] [constellation 0..2] [gi 0..3]
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <vector>
#include <ctime>
using namespace dect2;
int main(int argc, char** argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 60;
    dvbt::Params p;
    p.mode = (argc > 2 ? atoi(argv[2]) : 1) ? dvbt::k8K : dvbt::k2K;
    p.mod = (argc > 3 ? atoi(argv[3]) : 2) == 0 ? dvbt::kQpsk : (atoi(argv[3]) == 1 ? dvbt::k16Qam : dvbt::k64Qam);
    const int gis[] = {dvbt::kGi32, dvbt::kGi16, dvbt::kGi8, dvbt::kGi4};
    p.guard = gis[argc > 4 ? atoi(argv[4]) & 3 : 2];
    p.crHp = p.crLp = dvbt::kR34;
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) { pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0; pkt[3] = 0x10; memcpy(pkt + 4, &counter, 4); for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7); counter++; });
    const double fn = nativeRateHz(8);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0, 1);
    const float sigma = (float)(std::pow(10.0, -30 / 20.0) / std::sqrt(2.0));
    std::vector<cf32> sig, sym;
    for (int i = 0; i < 20000; i++) sig.push_back(cf32(nd(rng), nd(rng)) * sigma);
    for (int s = 0; s < frames * 68; s++) {
        gen.nextSymbol(sym);
        for (auto v : sym) sig.push_back(v + cf32(nd(rng), nd(rng)) * sigma);
    }
    DvbtReceiver rx;
    rx.configure(fn, 8);
    size_t packets = 0;
    rx.setPacketCallback([&](const uint8_t*, size_t n, double) { packets += n; });
    auto cpu = [] { return (double)std::clock() / CLOCKS_PER_SEC; };   // CPU time of the whole process
    fprintf(stderr, "feeding\n");
    const double c0 = cpu();
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sig.size(); i += 1 << 14) rx.feed(sig.data() + i, std::min<size_t>(1 << 14, sig.size() - i));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpuS = cpu() - c0;
    printf("%zu packets, %.2f s of signal: %.1f%% of real time by the clock, %.1f%% of one core in CPU time\n", packets, sig.size() / fn, 100 * secs / (sig.size() / fn), 100 * cpuS / (sig.size() / fn));
}
