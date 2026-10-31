// Inmarsat all-carriers mode on a live Airspy, for the C-channel (voice) work: the radio is driven through libairspy (opened at run time,
// nothing linked), its samples go to the manager, the voice frames to the voice mixer with the user's aeroambe plug-in, and every 10 s a
// status line says what decodes. Does nothing (and passes) unless DECT2_LIVE_SECONDS is set: this is a user test with a real radio.
//   DECT2_LIVE_SECONDS=900 DECT2_LIVE_MHZ=1545.85 DECT2_LIVE_RATE=10e6 DECT2_LIVE_GAIN=14 DECT2_LIVE_BIAS=1
//   DECT2_LIVE_CODEC=/path/onair-codec-aeroambe.so DECT2_LIVE_DIR=~/inmsat_live DECT2_LIVE_SNIP=60 (seconds of raw cf32 kept from the first C sync)
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/inmsat/inmsat_voice.h"
#include "dect2/codec/speech_codec.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::inmsat;

#include "../common/test_util.h"
#ifdef _WIN32
#include <windows.h>
static void* libOpen(const char* n) { return (void*)LoadLibraryA(n); }
static void* libSym(void* l, const char* n) { return (void*)GetProcAddress((HMODULE)l, n); }
static const char* libErr() { return "LoadLibrary failed"; }
static const char* const kAirspyNames[] = {"libairspy.dll", "airspy.dll", nullptr};
#else
#include <dlfcn.h>
static void* libOpen(const char* n) { return dlopen(n, RTLD_NOW); }
static void* libSym(void* l, const char* n) { return dlsym(l, n); }
static const char* libErr() { const char* e = dlerror(); return e ? e : "?"; }
#ifdef __APPLE__
static const char* const kAirspyNames[] = {"libairspy.dylib", "libairspy.0.dylib", nullptr};
#else
static const char* const kAirspyNames[] = {"libairspy.so.0", "libairspy.so", nullptr};
#endif
#endif

namespace {
struct airspy_device;
struct Transfer { airspy_device* device; void* ctx; void* samples; int sample_count; uint64_t dropped_samples; int sample_type; };
typedef int (*cb_fn)(Transfer*);
struct Airspy {
    void* lib = nullptr; airspy_device* dev = nullptr;
    int (*open)(airspy_device**); int (*close)(airspy_device*); int (*setRate)(airspy_device*, uint32_t); int (*setType)(airspy_device*, int);
    int (*setFreq)(airspy_device*, uint32_t); int (*setLin)(airspy_device*, uint8_t); int (*setBias)(airspy_device*, uint8_t); int (*setPacking)(airspy_device*, uint8_t);
    int (*startRx)(airspy_device*, cb_fn, void*); int (*stopRx)(airspy_device*);
    bool load() {
        for (const char* const* n = kAirspyNames; *n && !lib; n++) lib = libOpen(*n);
        if (!lib) { printf("libairspy not found: %s\n", libErr()); return false; }
        auto g = [&](const char* n) { void* p = libSym(lib, n); if (!p) printf("missing %s\n", n); return p; };
        *(void**)&open = g("airspy_open"); *(void**)&close = g("airspy_close"); *(void**)&setRate = g("airspy_set_samplerate");
        *(void**)&setType = g("airspy_set_sample_type"); *(void**)&setFreq = g("airspy_set_freq"); *(void**)&setLin = g("airspy_set_linearity_gain");
        *(void**)&setBias = g("airspy_set_rf_bias"); *(void**)&setPacking = g("airspy_set_packing"); *(void**)&startRx = g("airspy_start_rx"); *(void**)&stopRx = g("airspy_stop_rx");
        return open && close && setRate && setType && setFreq && setLin && setBias && startRx && stopRx;
    }
};

struct Queue {
    std::mutex mu; std::condition_variable cv;
    std::deque<std::vector<cf32>> q;
    size_t queued = 0, limit = 0; uint64_t dropped = 0, pushed = 0;
    void push(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        pushed += n;
        if (queued + n > limit) { dropped += n; return; }
        q.emplace_back(x, x + n); queued += n;
        cv.notify_one();
    }
    bool pop(std::vector<cf32>& out, int ms) {
        std::unique_lock<std::mutex> lk(mu);
        if (!cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !q.empty(); })) return false;
        out = std::move(q.front()); q.pop_front(); queued -= out.size();
        return true;
    }
};

double env(const char* n, double d) { const char* v = getenv(n); return v ? atof(v) : d; }
std::string envs(const char* n, const char* d) { const char* v = getenv(n); return v ? v : d; }
std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }     // SIGINT / SIGTERM: stop the radio cleanly (an Airspy killed mid-stream delivers a broken stream until it is replugged)
}

int main() {
    const double secs = env("DECT2_LIVE_SECONDS", 0);
    if (secs <= 0) { printf("inmsat_live: no DECT2_LIVE_SECONDS, nothing to do (a user test with a real radio)\n"); return testSummary("inmsat_live"); }
    const double rate = env("DECT2_LIVE_RATE", 10e6), mhz = env("DECT2_LIVE_MHZ", 1545.85);
    const int gain = (int)env("DECT2_LIVE_GAIN", 14), bias = (int)env("DECT2_LIVE_BIAS", 1);
    const double snip = env("DECT2_LIVE_SNIP", 0);
    // the C-channel pool to watch (MHz): every wide carrier in it is listed and the raw capture starts on the first one (not only on a sync)
    const double poolLo = env("DECT2_LIVE_POOL_LO", 1546.09), poolHi = env("DECT2_LIVE_POOL_HI", 1546.33);
    std::string dir = envs("DECT2_LIVE_DIR", "");
    if (dir.empty()) dir = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/inmsat_live";
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    const std::string codecPath = envs("DECT2_LIVE_CODEC", "");

    Airspy r;
    if (!r.load()) return 1;
    if (r.open(&r.dev) != 0) { printf("airspy_open failed: is the radio attached to WSL (usbipd attach --wsl --busid 2-1) and free?\n"); return 1; }
    r.setPacking(r.dev, 0);
    r.setType(r.dev, 0);                                     // float32 I/Q
    if (r.setRate(r.dev, (uint32_t)rate) != 0) printf("airspy_set_samplerate %.0f failed\n", rate);
    r.setFreq(r.dev, (uint32_t)(mhz * 1e6));
    r.setLin(r.dev, (uint8_t)gain);
    r.setBias(r.dev, (uint8_t)(bias ? 1 : 0));
    printf("Airspy: %.1f Msps, %.4f MHz, linearity gain %d, bias-tee %s\n", rate / 1e6, mhz, gain, bias ? "on" : "off");

    Manager m;
    ManagerConfig c; c.rate = rate; c.centerHz = mhz * 1e6; c.budget = (double)env("DECT2_LIVE_BUDGET", 24);
    m.configure(c);
    m.setLocation(true, env("DECT2_LIVE_LAT", 48.85), env("DECT2_LIVE_LON", 2.35));   // the geometry evidence of the satellite identification
    std::vector<std::string> logs; std::mutex logMu;
    m.setLogCallback([&](const std::string& s) { std::lock_guard<std::mutex> lk(logMu); logs.push_back(s); });
    VoiceMixer voice;
    voice.setRecording(dir);
    if (!codecPath.empty()) {
        const std::string folder = codecPath.substr(0, codecPath.find_last_of('/'));
        voice.setDecoderFactory([folder]() {
            std::unique_ptr<codec::SpeechCodec> dec;
            codec::CodecLookup lk = codec::loadSpeechCodec("aeroambe", folder, true, dec);
            if (!dec) printf("codec: %s\n", lk.message.c_str());
            return dec;
        });
    }
    std::map<int, uint64_t> sinkFrames;
    std::mutex sinkMu;
    m.setVoiceSink([&](int id, double f, const std::vector<uint8_t>& fr, double t) {
        { std::lock_guard<std::mutex> lk(sinkMu); sinkFrames[id]++; }
        voice.pushFrame(id, f, fr, t);
    });

    Queue q; q.limit = (size_t)(3 * rate);                   // three seconds of samples at most in flight
    std::atomic<bool> run{true};
    std::atomic<uint64_t> usbDropped{0};
    auto cb = [](Transfer* t) -> int {
        Queue* qq = (Queue*)t->ctx;
        qq->push((const cf32*)t->samples, (size_t)t->sample_count);
        return 0;
    };
    // the worker: feed the manager, keep the first seconds after the first C sync as a raw capture (DECT2_LIVE_SNIP)
    FILE* snipFile = nullptr; double snipLeft = 0; bool snipDone = false;
    std::thread worker([&] {
        std::vector<cf32> buf;
        while (run) {
            if (!q.pop(buf, 200)) continue;
            m.feed(buf.data(), buf.size());
            if (snipFile) {
                fwrite(buf.data(), sizeof(cf32), buf.size(), snipFile);
                snipLeft -= (double)buf.size() / rate;
                if (snipLeft <= 0) { fclose(snipFile); snipFile = nullptr; snipDone = true; printf("raw capture closed\n"); }
            }
        }
    });
    if (r.startRx(r.dev, cb, &q) != 0) { printf("airspy_start_rx failed\n"); run = false; worker.join(); return 1; }

    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
    const auto t0 = std::chrono::steady_clock::now();
    double next = 10;
    uint64_t lastSeq = 0;
    int cSyncSeen = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Snapshot s;
        if (m.snapshot(s, lastSeq)) {
            lastSeq = s.seq;
            voice.endIdle(s.timeSec);
            int cs = 0;
            for (const auto& row : s.carriers) cs += row.type == CarrierType::AeroC && row.state >= 2;
            if (cs && !cSyncSeen) { cSyncSeen = 1; printf("[%6.1f s] first C-channel sync\n", el); }
            if (cs && snip > 0 && !snipFile && !snipDone) {
                const std::string base = dir + "/live_" + std::to_string((long)(mhz * 1e3)) + "kHz_" + std::to_string((long)el) + "s";
                snipFile = fopen((base + ".cf32").c_str(), "wb"); snipLeft = snip;
                FILE* meta = fopen((base + ".sigmf-meta").c_str(), "w");
                if (meta) { fprintf(meta, "{\"global\":{\"core:datatype\":\"cf32_le\",\"core:sample_rate\":%.0f,\"core:version\":\"1.0.0\",\"core:dataset\":\"%s.cf32\",\"core:description\":\"OnAir inmsat live test\"},\"captures\":[{\"core:sample_start\":0,\"core:frequency\":%.0f}],\"annotations\":[]}\n", rate, base.substr(base.find_last_of('/') + 1).c_str(), mhz * 1e6); fclose(meta); }
                printf("[%6.1f s] raw capture of %.0f s started: %s.cf32\n", el, snip, base.c_str());
            }
            if (el >= next) {
                next += 10;
                const VoiceStatus vs = voice.status();
                printf("[%6.1f s] %zu carriers, %zu windows, cost %.1f/%.0f, msgs %llu, SUs %llu/%llu, floor %.0f dB, sat %s | voice: %d active, codec %s | queue drop %.1f s\n",
                       el, s.carriers.size(), s.windows.size(), s.cpuCost, s.cpuBudget, (unsigned long long)s.messagesTotal, (unsigned long long)s.susOk,
                       (unsigned long long)s.susBad, s.floorDb, s.sat.valid ? (s.sat.name + " " + std::to_string((int)(100 * s.sat.confidence)) + "%" + (s.sat.ambiguous ? " (ambiguous)" : "")).c_str() : "?",
                       vs.activeCalls, vs.codecReady ? "ready" : "none", q.dropped / rate);
                for (const auto& row : s.carriers) {
                    const bool inPool = row.cls == CarrierClass::Wide && row.freqHz >= poolLo * 1e6 && row.freqHz <= poolHi * 1e6 && row.snrDb > 8;
                    if (row.type != CarrierType::AeroC && !inPool && !(row.voiceCandidate && row.snrDb > 20)) continue;
                    printf("    %.4f MHz %5.1f kHz %4.1f dB  %-14s state %d  Eb/N0 %4.1f  frames %5llu  voice %6llu%s\n", row.freqHz / 1e6, row.widthHz / 1e3, row.snrDb,
                           row.type == CarrierType::AeroC ? "Aero C (voice)" : "wide, no sync", row.state, row.ebn0Db, (unsigned long long)row.frames,
                           (unsigned long long)row.voiceFrames, row.parked ? "  parked" : "");
                }
                for (const auto& v : vs.channels)
                    if (v.active) printf("    call on carrier %d at %.4f MHz: %llu frames, %llu decoded, level %.2f, %s\n", v.id, v.freqHz / 1e6, (unsigned long long)v.frames,
                                         (unsigned long long)v.decoded, v.level, v.recording.empty() ? v.rawFile.c_str() : v.recording.c_str());
                std::lock_guard<std::mutex> lk(logMu);
                for (const auto& l : logs) printf("    log: %s\n", l.c_str());
                logs.clear();
                fflush(stdout);
            }
        }
        if (el >= secs || gStop) break;
    }
    if (gStop) printf("stopped by a signal at %.0f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    r.stopRx(r.dev);
    run = false; worker.join();
    if (snipFile) fclose(snipFile);
    r.close(r.dev);
    const VoiceStatus vs = voice.status();
    printf("done: %.0f s; voice channels seen %zu; samples pushed %.1f s, dropped %.1f s\n", secs, vs.channels.size(), q.pushed / rate, q.dropped / rate);
    for (const auto& v : vs.channels) printf("  carrier %d at %.4f MHz: %llu frames, %llu decoded\n", v.id, v.freqHz / 1e6, (unsigned long long)v.frames, (unsigned long long)v.decoded);
    return testSummary("inmsat_live");
}
