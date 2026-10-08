// Helpers for the radiosonde tests and tools: run the test signal through the receiver with impairments and read the result.
#pragma once
#include "sonde_gen.h"
#include "sonde_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace dect2 {
namespace sondetest {

// the sanitizer builds run several times slower: speed checks are skipped there
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

struct Impair {
    bool quant8 = false;           // round to 8 bits like a HackRF
    float dcI = 0, dcQ = 0;        // constant offset added before quantisation
    double gapAtS = -1, gapLenS = 0;     // zeros for a while
    double resetAtS = -1;          // call reset() once at this time
    size_t chunk = 65536;
    double centerMhz = 403.0;
    std::function<void(cf32*, size_t, uint64_t)> inject;     // adds something to a block of samples (block, count, index of its first sample)
};

struct Result {
    SondeTelemetry tel;
    double rtf = 0;                // seconds of signal per second of processing time
    double cpuS = 0;
    uint64_t dropped = 0;
    double signalS = 0;
};

inline const SondeInfo* find(const SondeTelemetry& t, const std::string& serial) {
    for (const auto& s : t.sondes) if (s.serial == serial) return &s;
    return nullptr;
}
inline const SondeInfo* findType(const SondeTelemetry& t, const std::string& type) {
    for (const auto& s : t.sondes) if (s.type == type) return &s;
    return nullptr;
}

// Plays `secs` of the test signal into a receiver (synchronous mode) and returns its last report
inline Result run(const SynthConfig& cfg, double rate, double secs, const Impair& im = Impair()) {
    Result r;
    auto syn = makeSondeSynth(cfg, rate);
    SondeReceiver rx;
    rx.setSynchronous(true);
    rx.configure(rate);
    rx.setCenterMhz(im.centerMhz);
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    uint64_t last = 0;
    bool didReset = false;
    size_t sinceTel = 0;
    for (size_t done = 0; done < total; ) {
        const size_t m = std::min(buf.size(), total - done);
        syn->generate(buf.data(), m);
        if (im.inject) im.inject(buf.data(), m, done);
        for (size_t i = 0; i < m; i++) {
            float re = buf[i].real() + im.dcI, imv = buf[i].imag() + im.dcQ;
            if (im.quant8) { re = std::round(re * 127.f) / 127.f; imv = std::round(imv * 127.f) / 127.f; }
            const double t = (double)(done + i) / rate;
            if (im.gapAtS >= 0 && t >= im.gapAtS && t < im.gapAtS + im.gapLenS) { re = 0; imv = 0; }
            buf[i] = cf32(re, imv);
        }
        if (im.resetAtS >= 0 && !didReset && (double)done / rate >= im.resetAtS) { rx.reset(); didReset = true; }
        for (size_t o = 0; o < m; o += im.chunk) {
            const size_t k = std::min(im.chunk, m - o);
            rx.feed(buf.data() + o, k);
            sinceTel += k;
            if (sinceTel >= 4096) {
                sinceTel = 0;
                SondeTelemetry t;
                if (rx.telemetry(t, last)) { last = t.seq; r.tel = std::move(t); }
            }
        }
        done += m;
    }
    { SondeTelemetry t; if (rx.telemetry(t, last)) { last = t.seq; r.tel = std::move(t); } }
    r.cpuS = rx.cpuSeconds();
    r.rtf = secs / std::max(1e-9, r.cpuS);
    r.dropped = rx.droppedSamples();
    r.signalS = secs;
    return r;
}

} // namespace sondetest
} // namespace dect2
