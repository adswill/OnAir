// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Every mode through the whole engine under malformed input, meant for DECT2_SANITIZE=ON (ASan + UBSan): each mode runs in a child
// process of its own (a crash, a sanitizer report or a hang is pinned on that mode, and the next mode still runs) and gets, from a
// source in memory (setTestSourceFactory):
//   random IQ, impulsive noise, its own test signal clipped, with a DC offset, cut into pieces (dropped and repeated spans), with bit
//   flips in the 8-bit samples, zeros, full-scale squares and chirps, and huge, tiny and denormal floats (no NaN);
// then a lifecycle: retunes, a sample-rate change while it runs (the engine restarts), mode switches A -> B while running to other modes,
// and start / stop loops. The heap in use (ASan's allocator) is compared over repeated start / stop cycles to find leaks.
// The parent prints one line per mode and the first sanitizer lines of each failure. Exit code 1 when a mode failed.
// Environment: ONAIR_FUZZ_ONLY=id,id  only these modes; ONAIR_FUZZ_NEW=1  also the 3.9 block (engine standards 602 and up, reported only,
// never failing); ONAIR_TEST_LONG=1  longer signals and every other mode as a switch partner; ONAIR_FUZZ_PARTNERS=n  switch partners per
// mode (default 3); ONAIR_FUZZ_JOBS=n  children at once (default 1); ONAIR_FUZZ_SEED.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_registry.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ONAIR_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define ONAIR_ASAN 1
#endif
#ifdef ONAIR_ASAN
#include <sanitizer/allocator_interface.h>
#endif
extern char** environ;
using namespace dect2;
using Clock = std::chrono::steady_clock;

namespace {

struct Entry {
    std::string id;
    int stdMode = 0;
    const SubMode* sub = nullptr;
    double mhz = 100, bw = 1, rate = 2e6, offset = 0;
    int kind = 0;   // 0 table, 1 t2, 2 dvbt, 3 atsc, 4 dab, 5 atsc3, 6 isdbt, 7 fm
    bool isNew() const { return stdMode >= 602 && stdMode < kPluginStdBase; }
};

std::vector<Entry> entries() {
    std::vector<Entry> v;
    auto core = [&](const char* id, int s, int kind, double bw, double rate, double mhz) {
        Entry e; e.id = id; e.stdMode = s; e.kind = kind; e.bw = bw; e.rate = rate > 0 ? rate : nativeRateHz(bw); e.mhz = mhz; v.push_back(e);
    };
    core("auto", 0, 1, 8, 0, 522);
    core("t2", 1, 1, 8, 0, 522);
    core("dvbt", 2, 2, 8, 0, 522);
    core("atsc", 3, 3, 6, 8e6, 600);
    core("dab", 4, 4, 8, 10e6, 225);
    core("atsc3", 5, 5, 8, 10e6, 600);
    core("isdbt", 6, 6, 6, 8e6, 600);
    core("fm", 7, 7, 0.25, 4e6, 93.9);
    std::vector<int> seen;
    for (const ModeTuning& m : modeTunings()) {
        if (m.stdMode < 8 || std::find(seen.begin(), seen.end(), m.stdMode) != seen.end()) continue;
        seen.push_back(m.stdMode);
        Entry e; e.id = m.id; e.stdMode = m.stdMode; e.mhz = m.defMhz; e.bw = m.bandwidthMhz; e.rate = m.sampleRate; e.offset = m.tuneOffsetHz;
        v.push_back(e);
    }
    int n = 0;
    const SubMode* sm = subModes(n);
    for (int i = 0; i < n; i++) {
        const ModeTuning* m = modeTuning(sm[i].stdMode);
        if (!m) continue;
        Entry e; e.id = sm[i].id; e.stdMode = sm[i].stdMode; e.sub = &sm[i]; e.mhz = sm[i].defMhz; e.bw = m->bandwidthMhz; e.rate = m->sampleRate; e.offset = m->tuneOffsetHz;
        v.push_back(e);
    }
    return v;
}

void tuneFor(const Entry& en, double rate, TuneSettings& t) {
    t.bandwidthMhz = en.bw;
    t.sampleRate = rate;
    t.centerHz = en.mhz * 1e6 + en.offset;
    t.synth.mode = en.stdMode >= 4 ? en.stdMode : 0;
    t.synth.snrDb = 30;
    if (en.kind == 2) t.synth.dvbt = true;
    if (en.kind == 3) t.synth.atsc = true;
    if (en.kind == 4) t.synth.dab = true;
    if (en.kind == 6) { t.isdbtBwMhz = 6; t.synth.isdbtBwMhz = 6; }
    if (en.sub) applySubModeSynth(t.synth, *en.sub);
}

void quiet(Engine& e) {
    e.setComputeMode(0);
    e.fm().setSilent(true); e.vor().setSilent(true); e.m17().setSilent(true); e.cw().setSilent(true);
    e.psk().setListen(false); e.js8().setListen(false); e.ale().setListen(false); e.hfdig().setListen(false);
    e.specx().setSilent(true); e.uplink().setSilent(true); e.specan().setSilent(true); e.astro().setSilent(true); e.iono().setSilent(true);
    e.qo100().setSilent(true); e.general().setSilent(true); e.hfbeacon().setSilent(true); e.same().setSilent(true);
    e.dstar().setSilent(true); e.nxdn().setSilent(true); e.dpmr().setSilent(true); e.p25().setSilent(true); e.mac().setSilent(true);
    e.setWideMuted(true);
    e.setVfoSound(false);
}

// ------------------------------------------------------------------ the source in memory

struct Feed {
    std::mutex mu;
    std::vector<cf32> data;        // what the next start() plays, once (then silence is not sent: the source just idles)
    std::atomic<uint64_t> sent{0};
    std::atomic<bool> endless{false};   // lifecycle phase: loop `data` for ever
};
Feed gFeed;

class MemSource : public IqSource {
public:
    ~MemSource() override { stop(); }
    bool start(const TuneSettings& s, IqRing& ring, std::string&) override {
        rate_ = s.sampleRate > 0 ? s.sampleRate : 2e6;
        ring_ = &ring;
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() override { run_ = false; if (th_.joinable()) th_.join(); }
    bool retune(const TuneSettings&, std::string&) override { return true; }
    double sampleRate() const override { return rate_; }

private:
    void loop() {
        std::vector<cf32> d;
        { std::lock_guard<std::mutex> lk(gFeed.mu); d = gFeed.data; }
        size_t pos = 0;
        while (run_ && !d.empty()) {
            if (pos >= d.size()) { if (!gFeed.endless) break; pos = 0; }
            const size_t n = std::min<size_t>(1 << 14, d.size() - pos);
            while (run_ && ring_->capacity() - ring_->available() < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!run_) break;
            ring_->write(d.data() + pos, n);
            pos += n;
            gFeed.sent += n;
        }
    }
    double rate_ = 2e6;
    IqRing* ring_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread th_;
};

// the mode's own test signal, from the real synthetic source
std::vector<cf32> testSignal(const Entry& en, double rate, size_t n) {
    TuneSettings t;
    tuneFor(en, rate, t);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    setTestSourceFactory({});
    auto src = makeSource(dev);
    std::vector<cf32> out;
    IqRing ring(1u << 22);
    std::string err;
    if (!src || !src->start(t, ring, err)) return out;
    out.resize(n);
    size_t got = 0;
    const auto t0 = Clock::now();
    while (got < n && Clock::now() - t0 < std::chrono::seconds(60)) {
        const size_t k = ring.read(out.data() + got, n - got);
        if (!k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        got += k;
    }
    src->stop();
    out.resize(got);
    return out;
}

// ------------------------------------------------------------------ inputs

struct Seg { const char* name; std::vector<cf32> x; };

std::vector<Seg> inputs(const std::vector<cf32>& valid, size_t L, std::mt19937& g) {
    std::vector<Seg> s;
    std::normal_distribution<float> nd(0.f, 1.f);
    auto clampf = [](float v) { return std::max(-1.f, std::min(1.f, v)); };
    auto q8 = [](float v) { return (int8_t)std::lround(std::max(-128.f, std::min(127.f, v * 128.f))); };
    const size_t V = valid.size();
    { Seg a{"random IQ", std::vector<cf32>(L)}; for (auto& x : a.x) x = cf32(nd(g), nd(g)) * 0.3f; s.push_back(std::move(a)); }
    {
        Seg a{"impulsive noise", std::vector<cf32>(L)};
        for (size_t i = 0; i < L; i++) a.x[i] = cf32(nd(g), nd(g)) * 0.02f + (V ? valid[i % V] * 0.5f : cf32());
        for (size_t i = 0; i < L; i += 200 + g() % 4000) {
            const size_t w = 1 + g() % 64;
            for (size_t k = i; k < std::min(L, i + w); k++) a.x[k] = cf32(g() % 2 ? 1.f : -1.f, g() % 2 ? 1.f : -1.f);
        }
        s.push_back(std::move(a));
    }
    if (V) {
        Seg c{"clipped", valid}; for (auto& x : c.x) x = cf32(clampf(x.real() * 12), clampf(x.imag() * 12)); s.push_back(std::move(c));
        Seg d{"DC offset", valid}; for (auto& x : d.x) x += cf32(0.45f, -0.35f); s.push_back(std::move(d));
        s.push_back({"valid", valid});
        // pieces: spans dropped, repeated, and the signal cut off mid-symbol into silence and back
        Seg t{"truncated", {}};
        for (size_t i = 0; i < V;) {
            const size_t keep = 1 + g() % 20000, drop = g() % 3 ? 1 + g() % 4096 : 0;
            t.x.insert(t.x.end(), valid.begin() + (long)i, valid.begin() + (long)std::min(V, i + keep));
            if (g() % 5 == 0 && !t.x.empty()) { const size_t r = std::min(t.x.size(), (size_t)(1 + g() % 3000)); std::vector<cf32> rep(t.x.end() - (long)r, t.x.end()); t.x.insert(t.x.end(), rep.begin(), rep.end()); }
            if (g() % 7 == 0) t.x.insert(t.x.end(), 1 + g() % 5000, cf32());
            i += keep + drop;
        }
        s.push_back(std::move(t));
        Seg b{"bit flips", valid};
        for (size_t i = 0; i < V; i++) {
            int8_t re = q8(valid[i].real()), im = q8(valid[i].imag());
            if (g() % 300 == 0) re = (int8_t)(re ^ (1 << (g() % 8)));
            if (g() % 300 == 0) im = (int8_t)(im ^ (1 << (g() % 8)));
            b.x[i] = cf32(re / 128.f, im / 128.f);
        }
        for (int k = 0; k < 20; k++) { const size_t i = g() % V; for (size_t j = i; j < std::min(V, i + 64); j++) b.x[j] = cf32((int8_t)g() / 128.f, (int8_t)g() / 128.f); }
        s.push_back(std::move(b));
    }
    s.push_back({"zeros", std::vector<cf32>(L)});
    {
        Seg e{"full scale", std::vector<cf32>(L)};
        for (size_t i = 0; i < L; i++) {
            const size_t ph = i * 4 / L;
            if (ph == 0) e.x[i] = (i & 1) ? cf32(1, 1) : cf32(-1, -1);                      // Nyquist square
            else if (ph == 1) e.x[i] = cf32(1, -1);                                         // constant at the rail
            else { const double f = (double)i / L; e.x[i] = std::polar(1.f, (float)(M_PI * f * i * 0.25)); }   // full-scale chirp
        }
        s.push_back(std::move(e));
    }
    {
        Seg f{"extreme floats", std::vector<cf32>(L / 4 + 1)};
        const float vals[] = {1e30f, -1e30f, 3.4e38f, -3.4e38f, 1e-40f, -1e-42f, 0.f, -0.f, 65504.f, 1e9f, -2.1e9f, 4.3e9f};
        for (auto& x : f.x) x = cf32(vals[g() % 12], vals[g() % 12]);
        s.push_back(std::move(f));
    }
    if (V) s.push_back({"valid after", std::vector<cf32>(valid.begin(), valid.begin() + (long)std::min(V, L))});
    return s;
}

// ------------------------------------------------------------------ child: one mode

std::atomic<int64_t> gBeat{0};   // the child's main loop is alive (steady-clock ms); a poll that blocks behind a hung receiver stops it
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
#define STAGE(name) do { gBeat = nowMs(); printf("    stage: %s\n", name); fflush(stdout); } while (0)

// a thread that never touches the engine: when the main loop has not moved for 60 s it prints HANG with the stacks (macOS sample) and exits
void startWatchdog(const std::string& id) {
    gBeat = nowMs();
    std::thread([id] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (nowMs() - gBeat.load() < 60000) continue;
            printf("    HANG in %s: the engine did not return for 60 s\n", id.c_str());
            fflush(stdout);
#ifdef __APPLE__
            const std::string cmd = "/usr/bin/sample " + std::to_string(getpid()) + " 1 2>/dev/null | grep -E 'dect2::' | head -40";
            if (system(cmd.c_str()) != 0) {}
#endif
            fflush(stdout);
            _exit(4);
        }
    }).detach();
}

size_t heapInUse() {
#ifdef ONAIR_ASAN
    return __sanitizer_get_current_allocated_bytes();
#else
    return 0;
#endif
}

// runs the engine until it has taken every sample of gFeed, polling the reports (the GUI's poll path) meanwhile; false on a stall
bool playThrough(Engine& e, const Entry& en, double rate, size_t total, const char* what) {
    RxTelemetry rx;
    uint64_t seq = 0;
    const double secs = total / rate;
    auto lastMove = Clock::now();
    uint64_t lastSent = 0;
    double lastSig = -1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        gBeat = nowMs();
        if (e.latestRx(rx, seq)) { seq = rx.seq; (void)modeSummary(rx); }
        const uint64_t sent = gFeed.sent.load();
        double rxs = 0, sig = 0;
        sscanf(e.loadProfile().c_str(), "analysis thread: receiver %lf s for %lf s of signal", &rxs, &sig);
        if (sent != lastSent || sig != lastSig) { lastSent = sent; lastSig = sig; lastMove = Clock::now(); }
        if (sent >= total && Clock::now() - lastMove > std::chrono::milliseconds(300)) return true;
        if (Clock::now() - lastMove > std::chrono::seconds(30)) {
            printf("    HANG in %s: %s: no progress for 30 s (%llu of %zu samples taken, %.2f of %.2f s analysed)\n", en.id.c_str(), what,
                   (unsigned long long)sent, total, sig, secs);
            fflush(stdout);
            return false;
        }
    }
}

int child(const Entry& en, const std::vector<Entry>& partners, uint32_t seed, bool lng) {
    std::mt19937 g(seed);
    const double rate = en.rate;
    const size_t L = (size_t)std::min(rate * (lng ? 0.2 : 0.03), (double)(lng ? 1 << 20 : 1 << 16));
    const size_t nValid = (size_t)std::min(rate * (lng ? 2.0 : 0.25), (double)(lng ? 1 << 23 : 1 << 20));
    startWatchdog(en.id);
    STAGE("test signal");
    const std::vector<cf32> valid = testSignal(en, rate, nValid);
    printf("    %zu samples of test signal at %.0f Hz\n", valid.size(), rate);
    std::vector<Seg> segs = inputs(valid, L, g);
    setTestSourceFactory([](const DeviceInfo&) { return std::unique_ptr<IqSource>(new MemSource); });
    Engine e;
    quiet(e);
    e.setStandard(en.stdMode);
    if (en.sub) applySubMode(e, en.sub);
    if (en.stdMode == 22) e.mesh().setTunedHz(en.mhz * 1e6);
    TuneSettings t;
    tuneFor(en, rate, t);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "fuzz";
    FileOptions fo;
    // 1. every input in one run (the receiver carries its state from one into the next, as on the air), then each on a fresh start
    {
        STAGE("inputs, one run");
        std::vector<cf32> all;
        for (auto& s : segs) all.insert(all.end(), s.x.begin(), s.x.end());
        { std::lock_guard<std::mutex> lk(gFeed.mu); gFeed.data = all; }
        gFeed.sent = 0;
        if (!e.start(dev, t, fo)) { printf("    start failed: %s\n", e.startError().c_str()); return 3; }
        if (!playThrough(e, en, rate, all.size(), "all inputs")) return 4;
        e.stop();
    }
    for (auto& s : segs) {
        STAGE(s.name);
        { std::lock_guard<std::mutex> lk(gFeed.mu); gFeed.data = s.x; }
        gFeed.sent = 0;
        if (!e.start(dev, t, fo)) { printf("    start failed (%s)\n", s.name); return 3; }
        if (!playThrough(e, en, rate, s.x.size(), s.name)) return 4;
        e.stop();
    }
    // 2. lifecycle on the endless test signal: retunes, a rate change while running, switches to other modes and back, start / stop loops
    std::vector<cf32> loopSig = valid.empty() ? segs[0].x : valid;
    { std::lock_guard<std::mutex> lk(gFeed.mu); gFeed.data = loopSig; }
    gFeed.endless = true;
    auto runFor = [&](int ms) {
        RxTelemetry rx; uint64_t seq = 0;
        const auto t1 = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < t1) { gBeat = nowMs(); if (e.latestRx(rx, seq)) { seq = rx.seq; (void)modeSummary(rx); } std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    };
    STAGE("retune");
    if (!e.start(dev, t, fo)) return 3;
    runFor(150);
    for (double d : {1e3, -25e3, 0.0}) { TuneSettings r = t; r.centerHz += d; e.retune(r); runFor(60); }
    { TuneSettings r = t; r.centerHz += 7e3; e.retuneReset(r); runFor(60); }
    STAGE("sample-rate change");
    for (double f : {2.0, 0.5, 1.0}) { TuneSettings r = t; r.sampleRate = rate * f; e.retune(r); runFor(120); }
    STAGE("mode switches");
    for (const Entry& p : partners) {
        printf("    switch %s -> %s -> %s\n", en.id.c_str(), p.id.c_str(), en.id.c_str());
        fflush(stdout);
        e.setStandard(p.stdMode);
        if (p.sub) applySubMode(e, p.sub);
        runFor(80);
        e.setStandard(en.stdMode);
        if (en.sub) applySubMode(e, en.sub);
        runFor(40);
    }
    e.stop();
    STAGE("start/stop loops");
    size_t heap[6] = {};
    for (int c = 0; c < 6; c++) {
        if (!e.start(dev, t, fo)) return 3;
        runFor(c % 2 ? 30 : 120);
        e.stop();
        heap[c] = heapInUse();
    }
    gFeed.endless = false;
    int rc = 0;
#ifdef ONAIR_ASAN
    // a leak grows the heap at every cycle; caches and lists fill once. 2 MB per cycle over the last four cycles is a finding.
    const double perCycle = ((double)heap[5] - (double)heap[1]) / 4.0;
    printf("    heap after start/stop cycles: %.1f %.1f %.1f %.1f %.1f %.1f MB (%.0f kB per cycle)\n", heap[0] / 1e6, heap[1] / 1e6, heap[2] / 1e6, heap[3] / 1e6,
           heap[4] / 1e6, heap[5] / 1e6, perCycle / 1e3);
    if (perCycle > 2e6 && heap[2] < heap[3] && heap[3] < heap[4] && heap[4] < heap[5]) { printf("    LEAK: %.1f MB per start/stop cycle\n", perCycle / 1e6); rc = 6; }
#endif
    printf("    done\n");
    return rc;
}

// ------------------------------------------------------------------ parent

std::string tail(const std::string& path, size_t maxLines) {
    std::ifstream f(path);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(f, l)) lines.push_back(l);
    // the sanitizer report: its first line, the frames of our code and the summary
    std::string out;
    size_t firstErr = lines.size();
    for (size_t i = 0; i < lines.size(); i++)
        if (lines[i].find("ERROR: AddressSanitizer") != std::string::npos || lines[i].find("runtime error:") != std::string::npos ||
            lines[i].find("HANG in") != std::string::npos || lines[i].find("LEAK:") != std::string::npos || lines[i].find("ERROR: LeakSanitizer") != std::string::npos) { firstErr = i; break; }
    if (firstErr == lines.size()) firstErr = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::string lastStage;
    for (size_t i = 0; i < firstErr && i < lines.size(); i++) if (lines[i].find("stage:") != std::string::npos || lines[i].find("switch ") != std::string::npos) lastStage = lines[i];
    if (!lastStage.empty()) out += lastStage + "\n";
    size_t shown = 0;
    for (size_t i = firstErr; i < lines.size() && shown < maxLines; i++) {
        const std::string& x = lines[i];
        if (x.find("    #") != std::string::npos && x.find("dect2::") == std::string::npos && x.find(".cpp:") == std::string::npos) continue;   // our frames only
        if (x.find("Shadow bytes") != std::string::npos) break;
        out += "      " + x + "\n";
        shown++;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    // the sound device is never opened and the source never waits for real time; both are read at start-up, so set them and start again
    if (!getenv("DECT2_MUTE") || !getenv("DECT2_NOPACE")) {
        setenv("DECT2_MUTE", "1", 1);
        setenv("DECT2_NOPACE", "1", 1);
        execv(argv[0], argv);
        perror("execv");
        return 2;
    }
    const char* li = getenv("ONAIR_TEST_LONG");
    const bool lng = li && *li == '1';
    const uint32_t seed = getenv("ONAIR_FUZZ_SEED") ? (uint32_t)atol(getenv("ONAIR_FUZZ_SEED")) : 20261110u;
    std::vector<Entry> all = entries();
    if (argc >= 3 && !strcmp(argv[1], "--child")) {
        const int idx = atoi(argv[2]);
        std::vector<Entry> partners;
        for (int i = 3; i < argc; i++) partners.push_back(all[(size_t)atoi(argv[i])]);
        return child(all[(size_t)idx], partners, seed + (uint32_t)idx, lng);
    }
    const char* only = getenv("ONAIR_FUZZ_ONLY");
    const bool withNew = getenv("ONAIR_FUZZ_NEW") && *getenv("ONAIR_FUZZ_NEW") == '1';
    const int nPartners = getenv("ONAIR_FUZZ_PARTNERS") ? atoi(getenv("ONAIR_FUZZ_PARTNERS")) : (lng ? 1000000 : 3);
    const int jobs = std::max(1, getenv("ONAIR_FUZZ_JOBS") ? atoi(getenv("ONAIR_FUZZ_JOBS")) : 1);
    std::vector<int> run;
    for (size_t i = 0; i < all.size(); i++) {
        if (only) { std::string o = std::string(",") + only + ","; if (o.find("," + all[i].id + ",") == std::string::npos) continue; }
        else if (all[i].isNew() && !withNew) continue;
        run.push_back((int)i);
    }
    std::vector<int> pool;   // switch partners: the built-in modes outside the 3.9 block
    for (size_t i = 0; i < all.size(); i++) if (!all[i].isNew()) pool.push_back((int)i);
    char dir[] = "/tmp/onair_fuzz_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    setenv("HOME", dir, 1);   // nothing a mode might save reaches the real home
    printf("fuzz_engine_modes: %zu modes, %d switch partners each, logs in %s\n", run.size(), std::min<int>(nPartners, (int)pool.size() - 1), dir);
    fflush(stdout);
    struct Job { pid_t pid; int idx; Clock::time_point t0; std::string log; };
    std::vector<Job> live;
    int failed = 0, newFailed = 0, done = 0;
    std::vector<std::string> failures;
    const double limitSecs = lng ? 3600 : 900;
    auto reap = [&](bool block) {
        for (size_t j = 0; j < live.size();) {
            int st = 0;
            pid_t r = waitpid(live[j].pid, &st, WNOHANG);
            const double el = std::chrono::duration<double>(Clock::now() - live[j].t0).count();
            if (r == 0 && el > limitSecs) {
#ifdef __APPLE__
                const std::string cmd = "/usr/bin/sample " + std::to_string(live[j].pid) + " 1 -mayDie > '" + live[j].log + ".sample' 2>/dev/null";
                if (system(cmd.c_str()) != 0) {}
#endif
                kill(live[j].pid, SIGKILL);
                waitpid(live[j].pid, &st, 0);
                r = live[j].pid;
                st = -1;
            }
            if (r == 0) { j++; continue; }
            const Entry& en = all[(size_t)live[j].idx];
            const bool ok = st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            done++;
            if (ok) printf("[%3d/%zu] %-14s ok (%.1f s)\n", done, run.size(), en.id.c_str(), el);
            else {
                char why[64];
                if (st == -1) snprintf(why, sizeof why, "TIMEOUT after %.0f s", el);
                else if (WIFSIGNALED(st)) snprintf(why, sizeof why, "signal %d", WTERMSIG(st));
                else snprintf(why, sizeof why, "exit %d", WEXITSTATUS(st));
                printf("[%3d/%zu] %-14s FAILED (%s, %.1f s)%s\n%s", done, run.size(), en.id.c_str(), why, el, en.isNew() ? " [3.9 block: reported only]" : "",
                       tail(live[j].log, 24).c_str());
                if (en.isNew()) newFailed++; else { failed++; failures.push_back(en.id); }
            }
            fflush(stdout);
            live.erase(live.begin() + (long)j);
        }
        if (block && !live.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    for (int idx : run) {
        while ((int)live.size() >= jobs) reap(true);
        std::vector<std::string> args = {argv[0], "--child", std::to_string(idx)};
        const size_t me = (size_t)(std::find(pool.begin(), pool.end(), idx) - pool.begin());
        for (int k = 1, got = 0; got < nPartners && k < (int)pool.size(); k++) {
            const size_t base = me < pool.size() ? me : (size_t)idx;   // a fixed spread over the list, a different set per mode (all of them when long)
            const int cand = pool[(nPartners >= (int)pool.size() - 1 ? base + (size_t)k : base * 7 + (size_t)k * 13) % pool.size()];
            if (cand == idx) continue;
            args.push_back(std::to_string(cand));
            got++;
        }
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        const std::string log = std::string(dir) + "/" + all[(size_t)idx].id + ".log";
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        pid_t pid = 0;
        if (posix_spawn(&pid, argv[0], &fa, nullptr, av.data(), environ) != 0) { perror("posix_spawn"); return 2; }
        posix_spawn_file_actions_destroy(&fa);
        live.push_back({pid, idx, Clock::now(), log});
    }
    while (!live.empty()) reap(true);
    printf("fuzz_engine_modes: %d of %zu modes failed", failed, run.size());
    for (auto& f : failures) printf(" %s", f.c_str());
    if (newFailed) printf(" (and %d in the 3.9 block, reported only)", newFailed);
    printf("\n");
    return failed ? 1 : 0;
}
