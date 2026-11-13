// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-channel TV, the core: the channeliser (mix, low-pass, resample) and the slot scan, and MultiTv fed with two and three DVB-T2 channels
// (and DVB-T) side by side in one wide capture. Every occupied channel must lock and show its own service list; an empty slot gets no receiver;
// the cap holds; the selected channel's packets reach the sink and switching works. CPU per instance is printed.
#include "dect2/engine/channelizer.h"
#include "dect2/engine/multitv.h"
#include "multitv_testkit.h"
#ifdef _WIN32
#include <windows.h>
#include "dect2/engine/crash_report.h"
#include <fstream>
#include <iostream>
#else
#include <sys/resource.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace multitvtest;
#include "../common/test_util.h"

static double cpuSecs() {
#ifdef _WIN32
    FILETIME c, e, k, u;   // 100 ns units
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto t = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) * 1e-7; };
    return t(k) + t(u);
#else
    rusage r; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec);
#endif
}

// a pure tone through the channeliser: shifted to baseband, and the neighbouring channel's tone rejected
static void testFilter() {
    const double rate = 20e6, out = 10e6;
    ChannelFilter f;
    CHECK(f.configure(rate, 4e6, out), "configure");
    const size_t n = 200000;
    std::vector<cf32> x(n), y;
    // tones at +4.5 MHz (inside the channel at +4 MHz, 0.5 MHz above its centre) and -4 MHz (the other channel)
    for (size_t i = 0; i < n; i++) {
        const double a = 2 * M_PI * 4.5e6 * (double)i / rate, b = 2 * M_PI * -4e6 * (double)i / rate;
        x[i] = cf32((float)std::cos(a), (float)std::sin(a)) + cf32((float)std::cos(b), (float)std::sin(b));
    }
    f.process(x.data(), n, y);
    CHECK(y.size() > n / 2 - 100 && y.size() < n / 2 + 100, "output count %zu", y.size());
    // the wanted tone is at +0.5 MHz now: measure with a correlation at +0.5 MHz and at -8 MHz (the other tone)
    auto corr = [&](double hz) {
        std::complex<double> s = 0;
        for (size_t i = 2000; i < y.size(); i++) { const double a = -2 * M_PI * hz * (double)i / out; s += std::complex<double>(y[i]) * std::complex<double>(std::cos(a), std::sin(a)); }
        return std::abs(s) / (double)(y.size() - 2000);
    };
    const double want = corr(0.5e6);
    CHECK(want > 0.97 && want < 1.03, "wanted tone amplitude %.3f", want);
    // the other tone, 8 MHz away, aliases to -8 + 10 = +2 MHz at most if it were not filtered
    const double alias = corr(2e6);
    CHECK(alias < 0.006, "the neighbour's tone leaks %.4f", alias);
    printf("  filter: tone %.3f, neighbour %.5f (%.0f dB down)\n", want, alias, 20 * std::log10(std::max(alias, 1e-9) / want));
    {   // the cost of one channel: mixing and resampling 28 Msps down to 10 Msps, per second of signal
        ChannelFilter g;
        g.configure(28e6, 8e6, 10e6);
        std::vector<cf32> in(1 << 20, cf32(0.1f, 0.05f)), o;
        const auto t0 = std::chrono::steady_clock::now();
        size_t done = 0;
        while (done < 28000000) { o.clear(); g.process(in.data(), in.size(), o); done += in.size(); }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 28e6 / (double)done;
        printf("  channeliser: %.2f s of one core per second of a 28 Msps capture (%.0f %%)\n", s, s * 100);
    }
}

// which slots the scan finds occupied
static void testScan() {
    const double rate = 28e6;
    Capture cap({{false, -8e6, 8, 1, "A", 0}, {false, 8e6, 8, 2, "B", -6}}, rate, 25);   // the middle slot empty
    SpanScanner sc;
    sc.configure(rate);
    std::vector<cf32> blk;
    for (int i = 0; i < 40; i++) { cap.pull(1 << 18, blk); sc.feed(blk.data(), blk.size()); }
    const auto slots = spanSlots(rate, 8e6, 0);
    CHECK(slots.size() == 3, "slots at 28 Msps: %zu", slots.size());
    const auto m = sc.measure(slots, 8e6);
    CHECK(m.size() == 3, "measured %zu", m.size());
    if (m.size() == 3) {
        printf("  scan: %+.0f MHz %.1f dB above floor, %+.0f MHz %.1f dB, %+.0f MHz %.1f dB\n", m[0].pos.offsetHz / 1e6, m[0].aboveDb, m[1].pos.offsetHz / 1e6, m[1].aboveDb, m[2].pos.offsetHz / 1e6, m[2].aboveDb);
        CHECK(m[0].occupied && !m[1].occupied && m[2].occupied, "occupied: %d %d %d", m[0].occupied, m[1].occupied, m[2].occupied);
    }
    // 20 Msps, channels at +-4 MHz: the grid between two channels has two slots, the grid on a channel has one
    CHECK(spanSlots(20e6, 8e6, 4e6).size() == 2 && spanSlots(20e6, 8e6, 0).size() == 1, "slots at 20 Msps");
    CHECK(spanSlots(20e6, 7e6, 0).size() == 1 && spanSlots(20e6, 6e6, 0).size() == 3, "slots at 20 Msps for 7 and 6 MHz: %zu %zu", spanSlots(20e6, 7e6, 0).size(), spanSlots(20e6, 6e6, 0).size());
}

struct Result { std::vector<TvChannelRow> rows; std::vector<MultiService> svc; double cpuPerInst = 0; double wall = 0; int running = 0; std::vector<int> slots; };

// plays `secs` of the capture into a MultiTv and waits for the receivers
static Result runCase(const char* title, const std::vector<ChanSpec>& ch, double rate, bool dvbt, int maxCh, bool autoGrid, double gridOff, double secs, int minServices,
                      std::atomic<int>* sinkSlots = nullptr, int switchTo = -1000) {
    printf("%s\n", title);
    Capture cap(ch, rate, 28);
    MultiTv mt;
    MultiTvOptions o; o.dvbt = dvbt; o.spanRate = rate; o.widthMhz = ch[0].bwMhz; o.maxChannels = maxCh; o.autoGrid = autoGrid; o.gridOffsetHz = gridOff; o.blocking = true;
    CHECK(mt.configure(o), "configure");
    if (sinkSlots) mt.setSink([sinkSlots](int slot, const uint8_t*, size_t n, double, const TsSnapshot&) { sinkSlots[(unsigned)(slot + 8) & 15] += (int)n; });
    const double c0 = cpuSecs();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<cf32> blk;
    const size_t total = (size_t)(rate * secs), step = 1 << 16;
    RxTelemetry t;
    for (size_t done = 0; done < total; done += step) {
        cap.pull(step, blk);
        mt.feed(blk.data(), blk.size());
        mt.poll(t);
        if (switchTo != -1000 && done >= total / 2 && mt.selected() != switchTo) mt.setSelected(switchTo);
    }
    // the T2 receivers decode on their own threads: wait until every service list is complete (or a limit)
    mt.wait();
    Result r;
    for (int i = 0; i < 400; i++) {
        mt.poll(t);
        r.svc = mt.services();
        if ((int)r.svc.size() >= minServices) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    r.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.rows = mt.channels();
    r.slots = mt.runningSlots();
    r.running = (int)r.slots.size();
    r.cpuPerInst = r.running ? (cpuSecs() - c0) / r.running : 0;
    for (const auto& row : r.rows)
        printf("  slot %+d (%+5.1f MHz): %s%s level %.1f dB (+%.1f) %s state %d, %d services, receiver thread %.0f %%, dropped %llu\n", row.slot, row.offsetHz / 1e6, row.occupied ? "occupied" : "empty", row.running ? ", running" : "",
               row.levelDb, row.aboveDb, row.selected ? "[playing]" : "         ", row.state, row.services, row.cpuPct, (unsigned long long)row.dropped);
    for (const auto& s : r.svc) printf("  service %d \"%s\" from slot %+d (%+.1f MHz)\n", s.svc.id, s.svc.name.c_str(), s.slot, s.offsetHz / 1e6);
    printf("  CPU: %.2f s per instance for %.1f s of signal (%.2f x real time, process total over %d receivers), wall %.1f s\n", r.cpuPerInst, secs, r.cpuPerInst / secs, r.running, r.wall);
    mt.reset();
    return r;
}

static bool hasService(const Result& r, int slotOffsetSign, const std::string& name) {
    for (const auto& s : r.svc) if (s.svc.name == name && (slotOffsetSign == 0 || (s.offsetHz > 0) == (slotOffsetSign > 0))) return true;
    return false;
}

#ifdef _WIN32
// A crash on a Windows CI runner leaves no trace in the ctest log: write the crash report (code, address, stack) and print it to standard error.
static LPTOP_LEVEL_EXCEPTION_FILTER gPrevFilter = nullptr;
static LONG WINAPI printCrash(EXCEPTION_POINTERS* ep) {
    const LONG r = gPrevFilter ? gPrevFilter(ep) : EXCEPTION_EXECUTE_HANDLER;
    std::ifstream f("multitv_crash.txt");
    std::cerr << "multitv crashed:\n" << f.rdbuf() << std::endl;
    return r;
}
#endif

int main() {
#ifdef _WIN32
    setvbuf(stdout, nullptr, _IONBF, 0);   // so the ctest log shows how far the run got
    dect2::crash::install("multitv_crash", ".");
    gPrevFilter = SetUnhandledExceptionFilter(printCrash);
#endif
    testFilter();
    testScan();
    {   // two DVB-T2 channels at 20 Msps, the capture centred between them (a radio tuned to the boundary)
        std::atomic<int> got[16] = {};
        Result r = runCase("two DVB-T2 channels at 20 Msps", {{false, -4e6, 8, 101, "Alpha TV", 0, 0, 0x1001, 1}, {false, 4e6, 8, 202, "Beta TV", -3, 3, 0x1002, 2}}, 20e6, false, 3, true, 0, 3.0, 2, got);
        CHECK(r.running == 2, "%d receivers", r.running);
        CHECK(hasService(r, -1, "Alpha TV") && hasService(r, +1, "Beta TV"), "both service lists (%zu services)", r.svc.size());
        int locked = 0;
        for (auto& row : r.rows) locked += row.locked;
        CHECK(locked == 2, "%d channels locked", locked);
        int fed = 0;
        for (auto& g : got) fed += g.load() > 0;
        CHECK(fed >= 1, "the sink got no packets");
    }
    {   // three DVB-T2 channels at 28 Msps on a raster centred on a channel; switching the playing channel half way
        std::atomic<int> got[16] = {};
        Result r = runCase("three DVB-T2 channels at 28 Msps (grid on a channel), playing switches to the +8 MHz channel", {{false, -8e6, 8, 11, "Uno", 0, 0, 0x1001, 1}, {false, 0, 8, 22, "Dos", -2, 2, 0x1002, 2}, {false, 8e6, 8, 33, "Tres", -4, 5, 0x1003, 3}},
                           28e6, false, 3, false, 0, 4.0, 3, got, +1);
        CHECK(r.running == 3, "%d receivers", r.running);
        CHECK(hasService(r, -1, "Uno") && hasService(r, 0, "Dos") && hasService(r, +1, "Tres"), "three service lists (%zu services)", r.svc.size());
        int locked = 0;
        for (auto& row : r.rows) locked += row.locked;
        CHECK(locked == 3, "%d channels locked", locked);
        CHECK(got[(1 + 8) & 15] > 0, "after the switch the sink gets the +8 MHz channel's packets");
    }
    {   // an empty slot in the middle
        Result r = runCase("DVB-T2 at -8 and +8 MHz, nothing in the middle (28 Msps)", {{false, -8e6, 8, 11, "Left", 0, 0, 0x1001, 1}, {false, 8e6, 8, 33, "Right", 0, 1, 0x1003, 3}}, 28e6, false, 3, false, 0, 3.0, 2);
        CHECK(r.running == 2, "%d receivers (the empty slot must get none)", r.running);
        bool midEmpty = false;
        for (auto& row : r.rows) if (row.slot == 0) midEmpty = !row.occupied && !row.running;
        CHECK(midEmpty, "the middle slot is reported empty");
        CHECK(hasService(r, -1, "Left") && hasService(r, +1, "Right"), "both service lists");
    }
    {   // the cap
        Result r = runCase("three channels, cap of 2", {{false, -8e6, 8, 11, "Uno", 0, 0, 0x1001, 1}, {false, 0, 8, 22, "Dos", -1, 2, 0x1002, 2}, {false, 8e6, 8, 33, "Tres", -2, 5, 0x1003, 3}}, 28e6, false, 2, false, 0, 2.0, 2);
        CHECK(r.running == 2, "cap 2: %d receivers", r.running);
        int occ = 0;
        for (auto& row : r.rows) occ += row.occupied;
        CHECK(occ == 3, "all three slots reported occupied (%d)", occ);
    }
    {   // DVB-T, two channels
        Result r = runCase("two DVB-T channels at 20 Msps", {{true, -4e6, 8, 5, "Terra One", 0, 0, 0x1001, 1}, {true, 4e6, 8, 6, "Terra Two", -2, 0, 0x1002, 2}}, 20e6, true, 2, true, 0, 3.0, 2);
        CHECK(r.running == 2, "%d receivers", r.running);
        CHECK(hasService(r, -1, "Terra One") && hasService(r, +1, "Terra Two"), "both DVB-T service lists (%zu)", r.svc.size());
    }
    printf(fails ? "multitv: %d FAILED\n" : "multitv: all passed\n", fails);
    return fails != 0;
}
