// ATSC receiver: generated 8-VSB signals through the whole receive chain, with carrier offset, clock offset, noise and echoes.
#include "dect2/atsc_gen.h"
#include "dect2/atsc_rx.h"
#include "dect2/dvbt_gen.h"
#include <chrono>
#include <random>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <cstdint>
#include <utility>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Result { size_t packets = 0, good = 0, errFlag = 0, wrong = 0; double lockSec = -1; AtscTelemetry tel; double cpuRt = 0; };

static Result run(const atsc::ChannelConfig& cc, double inRate, double seconds, unsigned seed = 1, bool verbose = false) {
    atsc::Generator gen(dvbt::testTsSource(), cc, inRate, seed);
    AtscReceiver rx;
    rx.configure(inRate);
    rx.setBlocking(true);
    std::vector<uint8_t> got;
    double sigSec = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double s) { got.insert(got.end(), p, p + n * 188); sigSec += s; });
    // the reference stream: the same source, packet by packet
    auto ref = dvbt::testTsSource();
    std::vector<cf32> blk;
    const size_t chunk = 1 << 15;
    size_t total = 0;
    Result r;
    double cpu = 0;
    while (total < (size_t)(seconds * inRate)) {
        blk.clear();
        gen.generate(chunk, blk);
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(blk.data(), blk.size());
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        total += blk.size();
        if (r.lockSec < 0 && rx.detectLevel() >= 4) r.lockSec = total / inRate;
    }
    rx.flush();
    uint64_t seq = 0;
    rx.telemetry(r.tel, seq);
    r.cpuRt = cpu / seconds;
    // every decoded packet that is not flagged must be a packet of the reference stream
    r.packets = got.size() / 188;
    std::set<std::string> refSet;
    for (int i = 0; i < 60000; i++) { uint8_t p[188]; ref(p); p[0] = 0x47; refSet.insert(std::string((char*)p, 188)); }
    for (size_t k = 0; k < r.packets; k++) {
        if (got[k * 188 + 1] & 0x80) { r.errFlag++; continue; }
        if (refSet.count(std::string((char*)&got[k * 188], 188))) r.good++; else r.wrong++;
    }
    if (verbose) {
        printf("  level %d  pilot %d seg %d field %d  cfo %.0f Hz sro %.1f ppm  SNR %.1f dB (data %.1f)  sync q %.2f  fields %llu  RS clean %llu corr %llu fail %llu\n",
               rx.detectLevel(), r.tel.pilot, r.tel.segSync, r.tel.fieldSync, r.tel.cfoHz, r.tel.sroPpm, r.tel.snrDb, r.tel.dataSnrDb, r.tel.syncQuality,
               (unsigned long long)r.tel.fields, (unsigned long long)r.tel.rsClean, (unsigned long long)r.tel.rsCorrected, (unsigned long long)r.tel.rsFailed);
    }
    return r;
}

struct Case { const char* name; atsc::ChannelConfig cc; double rate; double minGoodPct; double maxLock; };

int main(int argc, char** argv) {
    const bool verbose = argc > 1;
    std::vector<Case> cases;
    auto mk = [](double snr, double cfo, double sro, std::vector<atsc::Echo> e) { atsc::ChannelConfig c; c.snrDb = snr; c.cfoHz = cfo; c.sroPpm = sro; c.echoes = std::move(e); return c; };
    cases.push_back({"clean", mk(99, 0, 0, {}), 8e6, 97, 0.5});
    cases.push_back({"clean, 10 Msps", mk(99, 0, 0, {}), 10e6, 97, 0.5});
    cases.push_back({"carrier offset +18 kHz", mk(99, 18000, 0, {}), 8e6, 97, 0.5});
    cases.push_back({"carrier offset -25 kHz", mk(99, -25000, 0, {}), 8e6, 97, 0.5});
    cases.push_back({"clock offset +60 ppm", mk(99, 0, 60, {}), 8e6, 97, 0.7});
    cases.push_back({"clock offset -60 ppm", mk(99, 0, -60, {}), 8e6, 97, 0.7});
    cases.push_back({"SNR 19 dB", mk(19, 0, 0, {}), 8e6, 95, 0.5});
    cases.push_back({"SNR 16.5 dB", mk(16.5, 0, 0, {}), 8e6, 90, 0.7});
    cases.push_back({"post echo -8 dB at 2 us", mk(99, 0, 0, {{2.0, -8, 40}}), 8e6, 95, 0.7});
    cases.push_back({"pre echo -10 dB at 1.5 us", mk(99, 0, 0, {{-1.5, -10, 100}}), 8e6, 95, 0.7});
    cases.push_back({"post echo -6 dB at 5 us, SNR 24", mk(24, 0, 0, {{5.0, -6, 200}}), 8e6, 90, 0.9});
    cases.push_back({"post echo -3 dB at 5 us, SNR 28", mk(28, 0, 0, {{5.0, -3, 200}}), 8e6, 90, 0.9});
    cases.push_back({"post echo -1.5 dB at 8 us, SNR 30", mk(30, 0, 0, {{8.0, -1.5, 70}}), 8e6, 90, 0.9});
    cases.push_back({"three echoes, CFO 10 kHz, SRO 30 ppm, SNR 25", mk(25, 10000, 30, {{0.8, -9, 10}, {3.0, -12, 130}, {-1.0, -14, 250}}), 8e6, 90, 0.9});
    int shown = 0;
    for (auto& cs : cases) {
        if (verbose && argc > 2 && std::string(argv[2]) != std::to_string(shown)) { shown++; continue; }
        shown++;
        Result r = run(cs.cc, cs.rate, 1.6, 1, verbose);
        const double pct = r.packets ? 100.0 * r.good / (r.packets) : 0;
        printf("%-52s packets %5zu good %5zu flagged %4zu wrong %3zu  (%.1f%%)  locked at %.2f s  CPU %.1fx\n", cs.name, r.packets, r.good, r.errFlag, r.wrong, pct, r.lockSec, r.cpuRt);
        CHECK(r.lockSec >= 0 && r.lockSec <= cs.maxLock, "%s: lock took %.2f s", cs.name, r.lockSec);
        CHECK(pct >= cs.minGoodPct * (r.packets > 0 ? 1.0 : 100.0) * 0.93 && r.wrong <= 3, "%s: %.1f%% good, %zu wrong", cs.name, pct, r.wrong);
    }
    {   // noise only: no lock, no packets
        AtscReceiver rx;
        rx.configure(8e6);
        rx.setBlocking(true);
        size_t pk = 0;
        rx.setPacketCallback([&](const uint8_t*, size_t n, double) { pk += n; });
        std::mt19937 g(5);
        std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> blk(1 << 15);
        for (int i = 0; i < 250; i++) { for (auto& v : blk) v = cf32(nd(g), nd(g)); rx.feed(blk.data(), blk.size()); }
        rx.flush();
        printf("noise only: level %d, packets %zu\n", rx.detectLevel(), pk);
        CHECK(rx.detectLevel() < 3 && pk == 0, "noise produced a lock (level %d, %zu packets)", rx.detectLevel(), pk);
    }
    {   // signal, then a 0.4 s gap of noise, then the signal again: the receiver must find it again
        atsc::ChannelConfig cc; cc.snrDb = 25;
        atsc::Generator gen(dvbt::testTsSource(), cc, 8e6, 3);
        AtscReceiver rx;
        rx.configure(8e6);
        rx.setBlocking(true);
        size_t pk = 0;
        rx.setPacketCallback([&](const uint8_t*, size_t n, double) { pk += n; });
        std::mt19937 g(9);
        std::normal_distribution<float> nd(0.f, 3.f);
        std::vector<cf32> blk;
        auto feedSignal = [&](double sec) { for (size_t done = 0; done < (size_t)(sec * 8e6); done += 1 << 15) { blk.clear(); gen.generate(1 << 15, blk); rx.feed(blk.data(), blk.size()); } rx.flush(); };
        feedSignal(0.6);
        const size_t before = pk;
        for (int i = 0; i < 100; i++) { blk.assign(1 << 15, cf32(0, 0)); for (auto& v : blk) v = cf32(nd(g), nd(g)); rx.feed(blk.data(), blk.size()); }
        rx.flush();
        const size_t mid = pk;
        feedSignal(1.0);
        printf("dropout: %zu packets before the gap, %zu during, %zu after; level %d\n", before, mid - before, pk - mid, rx.detectLevel());
        CHECK(before > 2000 && pk - mid > 5000, "no recovery after a dropout (%zu before, %zu after)", before, pk - mid);
    }
    printf(fails ? "ATSC receiver tests FAILED\n" : "ATSC receiver tests passed\n");
    return fails ? 1 : 0;
}
