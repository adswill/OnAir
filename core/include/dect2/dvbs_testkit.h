// Helpers for the DVB-S/S2 tests and tools: a transport stream whose every packet can be checked (a counter and pseudo random bytes), and a runner that
// sends the generator's signal through the receiver in chunks and counts what comes out. Header only.
#pragma once
#include "dvbs_gen.h"
#include "dvbs_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace dvbs {

// Packet n of the test stream: PID 0x20, counter in bytes 4..7, pseudo random payload that depends on n
inline void testPacket(uint64_t n, uint8_t* p) {
    uint32_t st = (uint32_t)(n * 2654435761u) ^ 0x9e3779b9u;
    p[0] = 0x47; p[1] = 0x00; p[2] = 0x20; p[3] = (uint8_t)(0x10 | (n & 15));
    p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
    for (int i = 8; i < 188; i++) { st = st * 1664525u + 1013904223u; p[i] = (uint8_t)(st >> 24); }
}

struct RunConfig {
    DvbsSignalConfig sig;
    double secs = 4;
    size_t chunk = 65536;
    bool quantise = true;            // round to 8 bits like a HackRF
    double manualRs = 0;             // Hz, 0 = automatic
    int standardHint = 0;
    double rollOffHint = 0;
    double dropoutAt = -1, dropoutSecs = 0;     // the input is zero for this long (a gap in the samples, not a loss of signal time)
    double resetAt = -1;             // reset() at this time
    double scale = 1.0;              // level applied to the signal before quantisation (the receiver has its own AGC)
    bool testStream = true;          // the checkable packet stream; false: the demo programme of the generator
    std::vector<size_t> chunkPattern;   // when not empty, chunk sizes are taken from this list in turn
};

struct RunResult {
    uint64_t packets = 0, good = 0, bad = 0, gaps = 0, lost = 0, errorFlag = 0;   // `gaps`: jumps in the counter, `lost`: packets the jumps skipped
    double firstPacketSecs = -1;     // input time of the first packet
    double cpuSecs = 0;              // time spent in feed()
    double signalSecs = 0;
    uint64_t firstIndex = 0, lastIndex = 0;
    DvbsTelemetry tel;
    std::vector<std::string> log;
};

inline RunResult runCase(const RunConfig& rc) {
    RunResult r;
    DvbsSignalConfig c = rc.sig;
    uint64_t pn = 0;
    if (rc.testStream) c.ts = [&pn](uint8_t* p) { testPacket(pn++, p); };
    DvbsSignal sig(c);
    DvbsReceiver rx;
    rx.setBlocking(true);
    rx.configure(c.sampleRate);
    if (rc.manualRs > 0) rx.setSymbolRate(rc.manualRs);
    if (rc.standardHint) rx.setStandardHint(rc.standardHint);
    if (rc.rollOffHint > 0) rx.setRollOff(rc.rollOffHint);
    double inputSecs = 0;
    bool have = false;
    uint64_t expect = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++, p += 188) {
            r.packets++;
            if (p[1] & 0x80) { r.errorFlag++; r.bad++; continue; }
            const uint64_t idx = ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
            uint8_t ex[188];
            testPacket(idx, ex);
            if (memcmp(p, ex, 188) != 0) { r.bad++; continue; }
            if (!have) { have = true; r.firstIndex = idx; r.firstPacketSecs = inputSecs; }
            else if (idx != expect) { r.gaps++; r.lost += idx > expect ? idx - expect : 0; }
            r.good++;
            expect = idx + 1;
            r.lastIndex = idx;
        }
    });
    rx.setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    const size_t total = (size_t)(rc.secs * c.sampleRate);
    std::vector<cf32> buf(std::max<size_t>(rc.chunk, 65536));
    size_t done = 0, ci = 0;
    uint64_t seq = 0;
    bool resetDone = false;
    while (done < total) {
        size_t m = rc.chunk;
        if (!rc.chunkPattern.empty()) m = rc.chunkPattern[ci++ % rc.chunkPattern.size()];
        m = std::min(m, total - done);
        sig.generate(buf.data(), m);
        for (size_t i = 0; i < m; i++) {
            cf32 v = buf[i] * (float)rc.scale;
            if (rc.quantise) {
                auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
                v = cf32(q(v.real()), q(v.imag()));
            }
            const double t = (double)(done + i) / c.sampleRate;
            if (rc.dropoutAt >= 0 && t >= rc.dropoutAt && t < rc.dropoutAt + rc.dropoutSecs) v = cf32();
            buf[i] = v;
        }
        if (rc.resetAt >= 0 && !resetDone && (double)done / c.sampleRate >= rc.resetAt) { rx.reset(); resetDone = true; have = false; }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), m);
        r.cpuSecs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        done += m;
        inputSecs = (double)done / c.sampleRate;
        DvbsTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.tel = t; }
    }
    rx.flush();
    DvbsTelemetry t;
    if (rx.telemetry(t, 0)) r.tel = t;
    r.signalSecs = rc.secs;
    return r;
}

// A configuration with sensible defaults for a test: standard 1 (DVB-S) or 2 (DVB-S2), rate index, symbol rate in Hz, sample rate in Hz
inline RunConfig makeRun(int standard, int mod, int rate, double symbolRate, double sampleRate, double snrDb, double secs) {
    RunConfig rc;
    rc.sig.tx.standard = standard;
    rc.sig.tx.mod = mod;
    rc.sig.tx.rate = rate;
    rc.sig.tx.symbolRate = symbolRate;
    rc.sig.sampleRate = sampleRate;
    rc.sig.snrDb = snrDb;
    rc.secs = secs;
    return rc;
}

} // namespace dvbs
} // namespace dect2
