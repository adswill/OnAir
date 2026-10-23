// DVB family cost per stage: each TV standard's built-in test signal, at the radio rate a user runs it at, straight through its receiver
// (no Engine, no pacing), with the per-stage CPU accounting of dect2/common/stage_prof.h switched on.
//
//   bench_dvb [--case id[,id..]] [--seconds N] [--pipelined] [--list] [--json path]
//
// For every case it prints CPU seconds per second of signal: the whole process (all threads), the thread that calls feed() (the one that
// must keep up with the radio: above 1.0 the ring overflows), and every stage the receiver names, with its share. The signal is generated
// first (the synthetic source, as the app's "test signal" radio; resampled to the radio rate when the generator runs at the channel's native
// rate) and kept in memory as 8-bit samples, so generation is never in the figures. A digest of the delivered transport stream (FNV-1a over
// every packet) checks that an optimisation changed nothing: it must stay the same from one build to the next (DECT2_THREADS does not
// change it either). Not part of ctest: a full run takes minutes. docs/PERFORMANCE-DVB.md has the figures and how to read them.
#include "dect2/atsc/atsc_rx.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/common/ring.h"
#include "dect2/common/stage_prof.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/source/source.h"
#include "dect2/t2/t2rx.h"
#include "dect2/tv/bbunpack.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
using namespace dect2;
using Clock = std::chrono::steady_clock;

namespace {

bool gPipelined = false;   // --pipelined: the receivers' live-radio threading (DvbtReceiver / T2Receiver::setPipelined)

struct Case {
    std::string id, what;
    int stdMode = 0;
    double rate = 10e6, bwMhz = 8;
    std::function<void(TuneSettings&)> setup;
};

int stdOf(const char* id) {
    for (int s = 8; s < 200; s++) if (const ModeTuning* m = modeTuning(s)) if (!strcmp(m->id, id)) return m->stdMode;
    return -1;
}

std::vector<Case> cases() {
    std::vector<Case> v;
    v.push_back({"dvbt", "DVB-T 8 MHz, 8K, GI 1/8, 64-QAM 2/3, radio at 10 Msps", 2, 10e6, 8, [](TuneSettings& t) {
        t.synth.dvbt = true; t.synth.dvbtMode = 1; t.synth.dvbtGuard = 2; t.synth.dvbtMod = 2; t.synth.dvbtRate = 1; }});
    v.push_back({"dvbt-native", "DVB-T 8 MHz, 8K, GI 1/8, 64-QAM 2/3, at the native 9.14 Msps", 2, 64e6 / 7, 8, [](TuneSettings& t) {
        t.synth.dvbt = true; t.synth.dvbtMode = 1; t.synth.dvbtGuard = 2; t.synth.dvbtMod = 2; t.synth.dvbtRate = 1; }});
    v.push_back({"dvbt-2k", "DVB-T 8 MHz, 2K, GI 1/4, 16-QAM 3/4, radio at 10 Msps", 2, 10e6, 8, [](TuneSettings& t) {
        t.synth.dvbt = true; t.synth.dvbtMode = 0; t.synth.dvbtGuard = 3; t.synth.dvbtMod = 1; t.synth.dvbtRate = 2; }});
    v.push_back({"t2", "DVB-T2 8 MHz, 32K ext, GI 1/128, PP7, 256-QAM 2/3 (64800), radio at 10 Msps", 1, 10e6, 8, [](TuneSettings& t) {
        auto& x = t.synth.tx; x.s2field1 = 5; x.giIdx = 4; x.ext = true; x.pp = 6; x.payload = true; x.plpMod = 3; x.plpCod = 2; x.plpShort = false; }});
    v.push_back({"t2-16qam", "DVB-T2 8 MHz, 8K, GI 1/8, PP2, 16-QAM 3/4 (64800), radio at 10 Msps", 1, 10e6, 8, [](TuneSettings& t) {
        auto& x = t.synth.tx; x.s2field1 = 1; x.giIdx = 2; x.ext = false; x.pp = 1; x.payload = true; x.plpMod = 1; x.plpCod = 3; x.plpShort = false; }});
    v.push_back({"dvbs2-8psk", "DVB-S2 8PSK 3/4, 10 MBd, roll-off 0.20, pilots, radio at 20 Msps", stdOf("dvbs"), 20e6, 0, [](TuneSettings& t) {
        t.synth.modeOpt[0] = 0; t.synth.modeOpt[1] = 1; t.synth.modeOpt[2] = 7; t.synth.modeOpt[3] = 2; t.synth.modeOpt[5] = 1; t.synth.modeVal[0] = 10e6; }});
    v.push_back({"dvbs2-qpsk", "DVB-S2 QPSK 2/3, 5 MBd (the test default), radio at 10 Msps", stdOf("dvbs"), 10e6, 0, [](TuneSettings&) {}});
    v.push_back({"dvbs2-16apsk", "DVB-S2 16APSK 3/4, 8 MBd, roll-off 0.20, pilots, radio at 20 Msps", stdOf("dvbs"), 20e6, 0, [](TuneSettings& t) {
        t.synth.modeOpt[0] = 0; t.synth.modeOpt[1] = 2; t.synth.modeOpt[2] = 7; t.synth.modeOpt[3] = 2; t.synth.modeOpt[5] = 1; t.synth.modeVal[0] = 8e6; }});
    v.push_back({"dvbs2-32apsk", "DVB-S2 32APSK 4/5, 8 MBd, roll-off 0.20, pilots, radio at 20 Msps", stdOf("dvbs"), 20e6, 0, [](TuneSettings& t) {
        t.synth.modeOpt[0] = 0; t.synth.modeOpt[1] = 3; t.synth.modeOpt[2] = 8; t.synth.modeOpt[3] = 2; t.synth.modeOpt[5] = 1; t.synth.modeVal[0] = 8e6; }});
    v.push_back({"dvbs", "DVB-S QPSK 3/4, 10 MBd, radio at 20 Msps", stdOf("dvbs"), 20e6, 0, [](TuneSettings& t) {
        t.synth.modeOpt[0] = 1; t.synth.modeOpt[2] = 3; t.synth.modeVal[0] = 10e6; }});
    v.push_back({"dvbc", "DVB-C 256-QAM, 6.952 MBd, radio at 10 Msps", stdOf("dvbc"), 10e6, 8, [](TuneSettings& t) { t.synth.modeOpt[0] = 4; }});
    v.push_back({"atsc", "ATSC 1.0 8-VSB, radio at 8 Msps", 3, 8e6, 6, [](TuneSettings& t) { t.synth.atsc = true; }});
    v.push_back({"atsc3", "ATSC 3.0 (the test signal), radio at 10 Msps", 5, 10e6, 6, [](TuneSettings&) {}});
    v.push_back({"isdbt", "ISDB-T 6 MHz (the test signal), radio at 8 Msps", 6, 8e6, 6, [](TuneSettings& t) { t.isdbtBwMhz = 6; t.synth.isdbtBwMhz = 6; }});
    v.push_back({"dtmb", "DTMB 8 MHz (the test signal), radio at 10 Msps", stdOf("dtmb"), 10e6, 8, [](TuneSettings&) {}});
    return v;
}

double processCpu() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}

// CPU seconds of every thread of the process, by a stable thread id
std::map<uint64_t, double> threadCpu() {
    std::map<uint64_t, double> m;
#ifdef __APPLE__
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return m;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        thread_identifier_info_data_t ii; mach_msg_type_number_t ci = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
            thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ci) == KERN_SUCCESS)
            m[ii.thread_id] = bi.user_time.seconds + bi.system_time.seconds + (bi.user_time.microseconds + bi.system_time.microseconds) * 1e-6;
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
#else
    const long tck = sysconf(_SC_CLK_TCK);
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[64]; snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            if (FILE* f = fopen(path, "r")) {
                char buf[1024]; const size_t k = fread(buf, 1, sizeof buf - 1, f); buf[k] = 0; fclose(f);
                const char* p = strrchr(buf, ')');
                unsigned long ut = 0, st = 0;
                if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) m[(uint64_t)atoll(e->d_name)] = (double)(ut + st) / tck;
            }
        }
        closedir(d);
    }
#endif
    return m;
}

// the test signal at `rate`, `secs` long, as interleaved 8-bit I/Q (scaled by 128 like a CS8 file)
bool generate(const Case& c, double secs, std::vector<int8_t>& out) {
    setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    t.bandwidthMhz = c.bwMhz > 0 ? c.bwMhz : 8;
    t.sampleRate = c.rate;
    t.centerHz = 600e6;
    if (const ModeTuning* m = modeTuning(c.stdMode)) { t.centerHz = m->defMhz * 1e6; if (c.bwMhz <= 0) t.bandwidthMhz = m->bandwidthMhz; }
    t.synth.mode = c.stdMode >= 4 ? c.stdMode : 0;
    t.synth.snrDb = 30;
    if (c.setup) c.setup(t);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    std::string err;
    if (!src || !src->start(t, ring, err)) { fprintf(stderr, "%s: synthetic source: %s\n", c.id.c_str(), err.c_str()); return false; }
    const double r = src->sampleRate();
    ExactResampler rs;
    const bool resample = std::fabs(r - c.rate) > 1;
    if (resample) rs.configure(r, c.rate, 24);
    const uint64_t total = (uint64_t)(c.rate * secs);
    out.clear(); out.reserve((size_t)total * 2);
    std::vector<cf32> buf(1 << 16), conv;
    auto last = Clock::now();
    while (out.size() < total * 2) {
        const size_t n = ring.read(buf.data(), buf.size());
        if (!n) {
            if (Clock::now() - last > std::chrono::seconds(60)) { fprintf(stderr, "%s: no samples for 60 s\n", c.id.c_str()); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last = Clock::now();
        const cf32* p = buf.data();
        size_t m = n;
        if (resample) { conv.clear(); rs.process(buf.data(), n, conv); p = conv.data(); m = conv.size(); }
        for (size_t i = 0; i < m && out.size() < total * 2; i++) {
            out.push_back((int8_t)std::lround(std::min(127.f, std::max(-128.f, p[i].real() * 128.f))));
            out.push_back((int8_t)std::lround(std::min(127.f, std::max(-128.f, p[i].imag() * 128.f))));
        }
    }
    src->stop();
    return true;
}

struct Result {
    double secs = 0, cpuAll = 0, cpuFeed = 0, cpuBusiest = 0;   // busiest: the thread that used the most (the one that must keep up)
    uint64_t packets = 0, hash = 1469598103934665603ull;
    std::vector<std::pair<std::string, std::pair<double, uint64_t>>> stages;
};

// one receiver behind a uniform face
struct Rx {
    std::function<void(const cf32*, size_t)> feed;
    std::function<void()> finish;
    std::function<void()> destroy;
};

Rx makeRx(const Case& c, std::function<void(const uint8_t*, size_t)> ts) {
    Rx r;
    const double rate = c.rate;
    auto tsCb = [ts](const uint8_t* p, size_t n, double) { DECT2_STAGE("ts.out"); ts(p, n); };
    switch (c.stdMode) {
    case 1: {
        auto rx = std::make_shared<T2Receiver>();
        auto bb = std::make_shared<BbUnpacker>();
        auto blocks = std::make_shared<std::pair<long, long>>(0, 0);
        bb->setSink([ts](const uint8_t* pkt) { ts(pkt, 1); });
        rx->configure(rate, c.bwMhz);
        rx->setComputeMode(0);   // the CPU decoder (the GPU one is measured by tests/dsp)
        if (gPipelined) rx->setPipelined(true);
        rx->setPlpCallback([bb, blocks](const PlpResult& p) {
            DECT2_STAGE("ts.out");
            blocks->first += p.blocks; blocks->second += p.blocksOk;
            for (const auto& f : p.frames) { if (f.bits.empty()) bb->lost(); else bb->push(f.bits, f.header); }
        });
        // the decoded PLP frames reach the callback when the telemetry is read (the engine reads it all the time): read it every 8 blocks
        auto seq = std::make_shared<uint64_t>(0);
        auto nfeed = std::make_shared<int>(0);
        r.feed = [rx, seq, nfeed](const cf32* x, size_t n) {
            rx->feed(x, n);
            if (++*nfeed % 8) return;
            DECT2_STAGE("t2.telemetry (read)");   // as the engine does, about every 50 ms
            RxTelemetry t;
            if (rx->telemetry(t, *seq)) *seq = t.seq;
        };
        r.finish = [rx] { for (int i = 0; i < 20; i++) { RxTelemetry t; rx->telemetry(t, 0); std::this_thread::sleep_for(std::chrono::milliseconds(20)); } };
        r.destroy = [rx, bb, blocks]() mutable { RxTelemetry t; rx->telemetry(t, 0); printf("   T2 FEC blocks %ld, decoded %ld (state %d, fft %d, guard %d, L1 %d/%d, frames %llu, dropped %llu)\n", blocks->first, blocks->second, t.state, t.fftN, t.guard, (int)t.l1preOk, (int)t.l1postOk, (unsigned long long)t.plpFrames, (unsigned long long)t.plpFramesDropped); rx.reset(); bb.reset(); };
        break;
    }
    case 2: {
        auto rx = std::make_shared<DvbtReceiver>();
        rx->configure(rate, c.bwMhz);
        if (gPipelined) rx->setPipelined(true, true);
        rx->setPacketCallback(tsCb);
        r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
        r.finish = [rx] { rx->flush(); };
        r.destroy = [rx]() mutable { rx.reset(); };
        break;
    }
    case 3: {
        auto rx = std::make_shared<AtscReceiver>();
        rx->configure(rate); rx->setBlocking(true); rx->setPacketCallback(tsCb);
        r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
        r.finish = [rx] { rx->flush(); };
        r.destroy = [rx]() mutable { rx.reset(); };
        break;
    }
    case 5: {
        auto rx = std::make_shared<Atsc3Rx>();
        rx->setPaced(false); rx->configure(rate); rx->setBlocking(true); rx->setPacketCallback(tsCb);
        r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
        r.destroy = [rx]() mutable { rx->stop(); rx.reset(); };
        break;
    }
    case 6: {
        auto rx = std::make_shared<IsdbtReceiver>();
        rx->configure(rate, 6); rx->setPacketCallback(tsCb);
        r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
        r.destroy = [rx]() mutable { rx.reset(); };
        break;
    }
    default:
        if (c.id.rfind("dvbs", 0) == 0) {
            auto rx = std::make_shared<DvbsReceiver>();
            rx->configure(rate); rx->setBlocking(true); rx->setPacketCallback(tsCb);
            r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
            r.finish = [rx] { rx->flush(); };
            r.destroy = [rx]() mutable { rx.reset(); };
        } else if (c.id == "dvbc") {
            auto rx = std::make_shared<DvbcReceiver>();
            rx->configure(rate); rx->setBlocking(true); rx->setPacketCallback(tsCb);
            r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
            r.finish = [rx] { rx->flush(); };
            r.destroy = [rx]() mutable { rx.reset(); };
        } else if (c.id == "dtmb") {
            auto rx = std::make_shared<DtmbReceiver>();
            rx->configure(rate, c.bwMhz); rx->setPacketCallback(tsCb);
            r.feed = [rx](const cf32* x, size_t n) { rx->feed(x, n); };
            r.finish = [rx] { rx->flush(); };
            r.destroy = [rx]() mutable { rx.reset(); };
        }
    }
    return r;
}

Result run(const Case& c, const std::vector<int8_t>& sig) {
    Result res;
    res.secs = (double)sig.size() / 2 / c.rate;
    uint64_t pk = 0, h = 1469598103934665603ull;
    Rx rx = makeRx(c, [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n * 188; i++) h = (h ^ p[i]) * 1099511628211ull;
        pk += n;
    });
    if (!rx.feed) { fprintf(stderr, "%s: no receiver\n", c.id.c_str()); return res; }
    prof::stageProfReset();
    const size_t B = 1 << 16;   // a radio's block, as the engine hands them on
    std::vector<cf32> blk(B);
    const std::map<uint64_t, double> th0 = threadCpu();
    const double c0 = processCpu();
    const uint64_t f0 = prof::threadCpuNs();
    uint64_t convNs = 0;
    for (size_t i = 0; i < sig.size() / 2; i += B) {
        const size_t n = std::min(B, sig.size() / 2 - i);
        const uint64_t a = prof::threadCpuNs();
        for (size_t k = 0; k < n; k++) blk[k] = cf32(sig[2 * (i + k)] * (1.f / 128), sig[2 * (i + k) + 1] * (1.f / 128));
        convNs += prof::threadCpuNs() - a;
        rx.feed(blk.data(), n);
    }
    if (rx.finish) rx.finish();
    res.cpuFeed = (double)(prof::threadCpuNs() - f0 - convNs) * 1e-9;
    // worker threads may still be busy: wait until the process is idle
    double prev = processCpu();
    for (int i = 0; i < 200; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double now = processCpu();
        if (now - prev < 0.005) break;
        prev = now;
    }
    res.cpuAll = processCpu() - c0 - (double)convNs * 1e-9;
    for (const auto& t : threadCpu()) {
        const auto it = th0.find(t.first);
        res.cpuBusiest = std::max(res.cpuBusiest, t.second - (it == th0.end() ? 0.0 : it->second));
    }
    res.stages = prof::stageProfSnapshot();
    rx.destroy();
    res.packets = pk; res.hash = h;
    return res;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> want;
    double secs = 6;
    const char* jsonPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--case") && i + 1 < argc) {
            std::string s = argv[++i];
            for (size_t p = 0, q; p <= s.size(); p = q + 1) { q = s.find(',', p); if (q == std::string::npos) q = s.size(); if (q > p) want.push_back(s.substr(p, q - p)); }
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
        else if (!strcmp(argv[i], "--pipelined")) gPipelined = true;
        else if (!strcmp(argv[i], "--list")) { for (auto& c : cases()) printf("%-14s %s\n", c.id.c_str(), c.what.c_str()); return 0; }
        else { fprintf(stderr, "usage: bench_dvb [--case id[,id..]] [--seconds N] [--pipelined] [--list] [--json path]\n"); return 2; }
    }
    prof::stageProfEnable(true);
    FILE* js = jsonPath ? fopen(jsonPath, "w") : nullptr;
    if (js) fprintf(js, "[\n");
    bool first = true;
    for (const Case& c : cases()) {
        if (!want.empty() && std::find(want.begin(), want.end(), c.id) == want.end()) continue;
        if (c.stdMode < 0) { printf("%s: mode not built in\n", c.id.c_str()); continue; }
        std::vector<int8_t> sig;
        if (!generate(c, secs, sig)) continue;
        fprintf(stderr, "running %s\n", c.id.c_str());   // (profilers wait for this line: the signal is generated)
        const Result r = run(c, sig);
        printf("\n== %s: %s\n", c.id.c_str(), c.what.c_str());
        printf("   %.2f s of signal, %llu TS packets, digest %016llx\n", r.secs, (unsigned long long)r.packets, (unsigned long long)r.hash);
        printf("   CPU s per s of signal: all threads %.3f, feed thread %.3f, busiest thread %.3f%s\n", r.cpuAll / r.secs, r.cpuFeed / r.secs, r.cpuBusiest / r.secs,
               r.cpuFeed / r.secs > 1 ? "  (TOO SLOW: the radio's ring would overflow)" : r.cpuBusiest / r.secs > 1 ? "  (TOO SLOW: a decoder thread falls behind)" : "");
        auto st = r.stages;
        st.erase(std::remove_if(st.begin(), st.end(), [](const auto& e) { return e.second.second == 0; }), st.end());
        std::sort(st.begin(), st.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        double named = 0;
        for (auto& e : st) named += e.second.first;
        for (auto& e : st) printf("     %-24s %7.4f  %5.1f%%  (%llu calls)\n", e.first.c_str(), e.second.first / r.secs, 100 * e.second.first / std::max(1e-9, r.cpuAll), (unsigned long long)e.second.second);
        if (!st.empty()) printf("     %-24s %7.4f  %5.1f%%\n", "(not in a stage)", std::max(0.0, r.cpuAll - named) / r.secs, 100 * std::max(0.0, r.cpuAll - named) / std::max(1e-9, r.cpuAll));
        fflush(stdout);
        if (js) {
            fprintf(js, "%s{\"id\":\"%s\",\"secs\":%.3f,\"cpu_all\":%.5f,\"cpu_feed\":%.5f,\"cpu_busiest\":%.5f,\"packets\":%llu,\"digest\":\"%016llx\",\"stages\":{", first ? "" : ",\n", c.id.c_str(), r.secs,
                    r.cpuAll / r.secs, r.cpuFeed / r.secs, r.cpuBusiest / r.secs, (unsigned long long)r.packets, (unsigned long long)r.hash);
            for (size_t i = 0; i < st.size(); i++) fprintf(js, "%s\"%s\":%.5f", i ? "," : "", st[i].first.c_str(), st[i].second.first / r.secs);
            fprintf(js, "}}");
            first = false;
        }
    }
    if (js) { fprintf(js, "\n]\n"); fclose(js); }
    return 0;
}
