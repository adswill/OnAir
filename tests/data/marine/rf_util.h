// Helpers for the marine radio-level tests: play the test signal into the receiver the way the engine does and collect what it reports.
#pragma once
#include "dect2/marine_gen.h"
#include "dect2/marine_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MT_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MT_SANITIZED 1
#endif
#endif
#ifndef MT_SANITIZED
#define MT_SANITIZED 0
#endif

namespace mt {
using namespace dect2;

// under a sanitiser the long variants (rates of 8 Msps and more, chunks of 1 and 7 samples) are skipped: they take minutes
constexpr bool kSanitized = MT_SANITIZED != 0;

struct Scenario {
    int service = 1;               // generator: 1 NAVTEX, 2 DSC MF/HF, 3 fax, 4 DSC VHF
    int msg = 4;                   // NAVTEX message (4 = the short test message)
    double rate = 250000, secs = 30, snr = 30, cfo = 0, sro = 0, mist = 0, idle = 3;
    int fade = 0;
    size_t chunk = 65536;
    bool quant8 = false;           // round to 8 bits like a HackRF
    float dc = 0;                  // DC offset added before the rounding
    double gapAt = -1, gapMs = 20; // samples dropped from the stream
    double resetAt = -1;
    int setting = 0;               // MarineReceiver::setService (0 auto)
    double freqHz = 0;             // MarineReceiver::setFrequencyHz
    int lpm = 0, ioc = 0, lines = 0;
    double phasing = 0;
    int faxLpm = -1;               // receiver setting, -1 leave
    bool faxAuto = true;
    double faxSlant = 0;
};

struct Outcome {
    MarineTelemetry tel;
    FaxImage img;
    double feedSecs = 0;           // CPU time spent in feed()
    double rtf = 0;                // seconds of signal per second of processing
    uint64_t reports = 0;
    bool seqOk = true;
};

inline Outcome run(const Scenario& s) {
    SynthConfig cfg;
    cfg.mode = 17; cfg.snrDb = s.snr; cfg.cfoHz = s.cfo; cfg.sroPpm = s.sro;
    cfg.modeOpt[0] = s.service; cfg.modeOpt[1] = s.fade; cfg.modeOpt[2] = s.msg;
    cfg.modeOpt[3] = s.lpm; cfg.modeOpt[4] = s.ioc; cfg.modeOpt[5] = s.lines;
    cfg.modeVal[0] = s.mist; cfg.modeVal[1] = s.idle; cfg.modeVal[2] = s.phasing;
    auto syn = makeMarineSynth(cfg, s.rate);
    MarineReceiver rx;
    rx.configure(s.rate);
    rx.setSignalOffset(-marineTuning().tuneOffsetHz);
    rx.setService(s.setting);
    rx.setFrequencyHz(s.freqHz);
    if (s.faxLpm >= 0) rx.setFaxLpm(s.faxLpm);
    rx.setFaxAutoSlant(s.faxAuto);
    rx.setFaxSlantPpm(s.faxSlant);
    Outcome o;
    const size_t total = (size_t)(s.secs * s.rate);
    const size_t gen = 65536;
    std::vector<cf32> buf(gen);
    size_t done = 0;
    bool gapped = false, resetDone = false;
    uint64_t last = 0;
    double feed = 0;
    while (done < total) {
        const size_t n = std::min(gen, total - done);
        syn->generate(buf.data(), n);
        if (s.quant8 || s.dc != 0) {
            for (size_t i = 0; i < n; i++) {
                float a = buf[i].real() + s.dc, b = buf[i].imag() + s.dc;
                if (s.quant8) { a = std::lrintf(std::max(-1.f, std::min(1.f, a)) * 127.f) / 127.f; b = std::lrintf(std::max(-1.f, std::min(1.f, b)) * 127.f) / 127.f; }
                buf[i] = cf32(a, b);
            }
        }
        size_t start = 0, len = n;
        const double tNow = (double)done / s.rate;
        if (s.gapAt >= 0 && !gapped && tNow >= s.gapAt) { gapped = true; const size_t g = std::min(n, (size_t)(s.gapMs * 1e-3 * s.rate)); start = g; len = n - g; }
        if (s.resetAt >= 0 && !resetDone && tNow >= s.resetAt) { resetDone = true; rx.reset(); }
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t k = 0; k < len; k += s.chunk) rx.feed(buf.data() + start + k, std::min(s.chunk, len - k));
        feed += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        done += n;
        MarineTelemetry t;
        while (rx.telemetry(t, last)) { if (t.seq <= last) o.seqOk = false; last = t.seq; o.tel = t; o.reports++; }
    }
    o.feedSecs = feed; o.rtf = feed > 0 ? s.secs / feed : 0;
    uint64_t seq = 0;
    rx.latestImage(o.img, seq);
    return o;
}

inline uint32_t lev(const std::string& a, const std::string& b) {
    std::vector<uint32_t> p(b.size() + 1), q(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) p[j] = (uint32_t)j;
    for (size_t i = 1; i <= a.size(); i++) {
        q[0] = (uint32_t)i;
        for (size_t j = 1; j <= b.size(); j++) q[j] = std::min({p[j] + 1, q[j - 1] + 1, p[j - 1] + (a[i - 1] != b[j - 1])});
        p.swap(q);
    }
    return p[b.size()];
}

inline const char* kShortText() { return "010000 UTC JAN 26\nTEST MESSAGE FROM STATION A. 518 KHZ."; }

// messages with header AA09: how many are complete and exact, and the character edit distance over all of them
struct NavScore { int msgs = 0, exact = 0; uint32_t dist = 0, chars = 0; };
inline NavScore scoreNavtex(const MarineTelemetry& t) {
    NavScore r;
    const std::string want = kShortText();
    for (const auto& m : t.navtex) {
        if (m.header.size() >= 2 && m.header.compare(0, 2, "AA") != 0 && m.header.find('*') == std::string::npos) continue;
        r.msgs += m.repeats;
        if (m.complete && m.text == want) r.exact += m.repeats;
        r.dist += lev(m.text, want) * (uint32_t)m.repeats; r.chars += (uint32_t)want.size() * (uint32_t)m.repeats;
    }
    return r;
}

} // namespace mt
