// DTMB test kit: runs the test signal through a channel of impairments into the receiver and counts the numbered test packets that come out.
// Used by the tests and by dtmbtool. Header only; not part of the receiver.
#pragma once
#include "dtmb_gen.h"
#include "dtmb_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

namespace dect2::dtmb::kit {

struct Scenario {
    SignalConfig sc;
    double seconds = 2.0;
    int chunkMode = 0;           // 0: 16384 samples, 1: a mix of 1, 7, 4096, 65536 and other odd sizes
    bool quantise = true;        // 8 bits like the radio
    double levelDb = 0;          // gain in front of the quantiser (applies to the signal, the carrier wave and the impulses alike)
    cf32 dc{0, 0};               // DC offset added before quantisation (full scale = 1)
    double iqGainDb = 0, iqPhaseDeg = 0;
    double gapAt = -1, gapSec = 0; bool gapRemove = false;   // a dropout: zeros, or samples that are simply missing
    double resetAt = -1;
    double stepAt = -1, stepDb = 0;                          // the level changes by stepDb at stepAt (AGC)
    double toneDb = -100, toneHz = 0;                        // a carrier wave at toneDb relative to the signal power, at toneHz from the centre
    double burstsPerSec = 0, burstUs = 20, burstDb = 10;     // impulse noise: bursts of Gaussian noise, burstDb above the signal power
    int threads = 2;             // decoder threads, 0 = decode inside feed() (no drops, deterministic)
    uint32_t seed = 1;           // seed of the numbered packets
    uint64_t rngSeed = 12;
    double rxBwMhz = 0;          // the channel width the receiver is set to; 0 = the signal's (sc.symbolRate)
};

struct Result {
    uint64_t good = 0, wrong = 0, missing = 0, backwards = 0;   // numbered test packets
    double firstGoodSec = -1, lastGoodSec = -1;
    uint64_t goodAfterReset = 0;
    DtmbTelemetry tel;
    uint64_t seqBack = 0;
    double rxCpu = 0, signalSec = 0;                            // CPU seconds of the thread that calls feed() (flush included)
    std::vector<std::string> log;
    double goodFraction(double lockSec) const {                 // share of the packets a clean stream would have delivered after `lockSec`
        const double perSec = lastRate / 1504.0;
        const double expect = perSec * std::max(0.0, signalSec - lockSec);
        return expect > 0 ? (double)good / expect : 0;
    }
    double lastRate = 0;
};

inline double threadCpuSeconds() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

inline SignalConfig makeSignal(Header h, Mapping m, Rate rt, bool mode2, double rate, double snr) {
    SignalConfig sc;
    sc.rate = rate; sc.snrDb = snr;
    sc.tx.header = h; sc.tx.profile.map = m; sc.tx.profile.rate = rt; sc.tx.profile.mode2 = mode2;
    sc.tx.phaseRotate = h != Header::Pn595;
    return sc;
}

inline Result run(const Scenario& o, bool verbose = false) {
    Result r;
    Signal sig(o.sc, testPacketSource(o.seed));
    DtmbReceiver rx;
    rx.configure(o.sc.rate, o.rxBwMhz > 0 ? o.rxBwMhz : (o.sc.symbolRate < 7e6 ? 6.0 : 8.0));
    rx.setDecoderThreads(o.threads);
    uint32_t expect = 0xFFFFFFFFu;
    double now = 0;
    bool afterReset = false;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            uint32_t num = 0xFFFFFFFFu;
            if (checkTestPacket(p + i * 188, o.seed, &num)) {
                r.good++;
                if (afterReset) r.goodAfterReset++;
                if (r.firstGoodSec < 0) r.firstGoodSec = now;
                r.lastGoodSec = now;
                if (expect != 0xFFFFFFFFu) { if (num < expect) r.backwards++; else r.missing += num - expect; }
                expect = num + 1;
            } else r.wrong++;
        }
    });
    rx.setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    const size_t total = (size_t)(o.seconds * o.sc.rate);
    std::vector<cf32> buf;
    std::mt19937 rng(o.rngSeed);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float gI = (float)std::pow(10.0, o.iqGainDb / 40.0), gQ = 1.f / gI;
    const float sp = (float)std::sin(o.iqPhaseDeg * 3.14159265 / 180.0), cp = (float)std::cos(o.iqPhaseDeg * 3.14159265 / 180.0);
    const double sigPower = (double)o.sc.rms * o.sc.rms;
    const float toneAmp = o.toneDb > -90 ? (float)std::sqrt(sigPower * std::pow(10.0, o.toneDb / 10.0)) : 0.f;
    const double toneStep = 2.0 * 3.14159265358979 * o.toneHz / o.sc.rate;
    const float burstSigma = (float)std::sqrt(sigPower * std::pow(10.0, o.burstDb / 10.0) / 2.0);
    const size_t burstLen = (size_t)std::max(1.0, o.burstUs * 1e-6 * o.sc.rate);
    const double burstProb = o.burstsPerSec / o.sc.rate;
    size_t burstLeft = 0;
    uint64_t last = 0, produced = 0;
    static const size_t odd[] = {1, 7, 4096, 65536, 3, 1000, 12345, 65535, 17, 8191};
    size_t oddIdx = 0;
    double cpu = 0, tonePhase = 0;
    std::uniform_real_distribution<double> ur(0.0, 1.0);
    const float lvl = (float)std::pow(10.0, o.levelDb / 20.0), stepGain = (float)std::pow(10.0, o.stepDb / 20.0);
    r.lastRate = netBitrate(o.sc.tx.header, o.sc.tx.profile, o.sc.symbolRate);
    while (produced < total) {
        size_t n = o.chunkMode == 0 ? 16384 : (produced < 3000 ? 1 : produced < 6000 ? 7 : odd[oddIdx++ % 10]);
        n = std::min(n, total - (size_t)produced);
        buf.resize(n);
        sig.generate(buf.data(), n);
        now = (double)produced / o.sc.rate;
        const float g = lvl * (o.stepAt >= 0 && now >= o.stepAt ? stepGain : 1.f);
        for (size_t i = 0; i < n; i++) {
            cf32 v = buf[i];
            if (toneAmp > 0) { v += toneAmp * cf32((float)std::cos(tonePhase), (float)std::sin(tonePhase)); tonePhase += toneStep; if (tonePhase > 6.283185307) tonePhase -= 6.283185307; }
            if (o.burstsPerSec > 0) {
                if (burstLeft == 0 && ur(rng) < burstProb) burstLeft = burstLen;
                if (burstLeft) { v += cf32(nd(rng), nd(rng)) * burstSigma; burstLeft--; }
            }
            v *= g;   // level steps and the gain in front of the converter act on everything that arrives, the interference included
            float re = v.real(), im = v.imag();
            if (o.iqGainDb != 0 || o.iqPhaseDeg != 0) { const float a = re * gI, b = im * gQ; re = a; im = b * cp + a * sp; }
            re += o.dc.real(); im += o.dc.imag();
            if (o.quantise) { re = std::round(std::min(127.f, std::max(-128.f, re * 128.f))) / 128.f; im = std::round(std::min(127.f, std::max(-128.f, im * 128.f))) / 128.f; }
            buf[i] = cf32(re, im);
        }
        const bool inGap = o.gapAt >= 0 && now >= o.gapAt && now < o.gapAt + o.gapSec;
        if (o.resetAt >= 0 && !afterReset && now >= o.resetAt) { rx.reset(); afterReset = true; }
        const double c0 = threadCpuSeconds();
        if (inGap && o.gapRemove) { /* the samples never arrive */ }
        else {
            if (inGap) for (auto& v : buf) v = cf32(0, 0);
            rx.feed(buf.data(), n);
        }
        cpu += threadCpuSeconds() - c0;
        produced += n;
        DtmbTelemetry t;
        if (rx.telemetry(t, last)) {
            if (t.seq < last) r.seqBack++;
            last = t.seq; r.tel = t;
            if (verbose) printf("      t=%.2f state %d frames %llu loss %.0f%% ok %llu bad %llu dropped %llu skipped %llu packets %llu good %llu missing %llu\n", now, t.state, (unsigned long long)t.frames, t.frameLossPct,
                                (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.cwDropped, (unsigned long long)t.cwSkipped, (unsigned long long)t.packets,
                                (unsigned long long)r.good, (unsigned long long)r.missing);
        }
    }
    const double c0 = threadCpuSeconds();
    rx.flush();
    cpu += threadCpuSeconds() - c0;
    rx.telemetry(r.tel, 0);
    r.rxCpu = cpu; r.signalSec = o.seconds;
    return r;
}

} // namespace dect2::dtmb::kit
