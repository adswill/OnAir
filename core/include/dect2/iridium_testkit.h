// Iridium test helpers (tests and the tool): random burst scenes, a run through the receiver with impairments, and matching what was
// received against what was sent. Header only.
#pragma once
#include "gen_util.h"
#include "iridium_gen.h"
#include "iridium_phy.h"
#include "iridium_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <vector>

namespace dect2 {
namespace iridiumtest {

struct SceneOpts {
    double rate = 10e6;
    double secs = 2;
    double esn0Db = 25;
    double perSec = 150;            // bursts a second
    double bandHz = 0;              // bursts within +-bandHz/2 of 0 Hz (0: 90 % of the rate)
    double cfoHz = 0;               // added to every burst
    double dopplerMax = 0;          // each burst gets a random Doppler in +-dopplerMax ...
    double dopplerRate = 0;         // ... and this rate (sign random)
    double uplinkFrac = 0;          // share of uplink bursts
    double simplexFrac = 0.3;       // share with a 64 symbol preamble
    bool longSimplex = false;       // those carry up to 400 symbols (only above 1626 MHz, where the receiver allows them)
    bool overlap = false;           // bursts may overlap in time (on channels at least 3 apart)
    uint32_t seed = 1;
};

inline std::vector<IridiumTestBurst> makeScene(const SceneOpts& o, double noiseSigma) {
    std::mt19937 r(o.seed);
    std::vector<IridiumTestBurst> v;
    const double band = o.bandHz > 0 ? o.bandHz : 0.9 * o.rate;
    const int nCh = std::max(1, (int)(band / iridium::kChannelHz) - 2);
    const double amp = noiseSigma * std::sqrt(std::pow(10.0, o.esn0Db / 10) * iridium::kSymbolRate * 2 / o.rate);
    double t = 0.01;
    const double gap = 1.0 / o.perSec;
    struct Busy { double t0, t1; int ch; };
    std::vector<Busy> busy;
    while (t < o.secs - 0.03) {
        IridiumTestBurst b;
        const bool simplex = (r() % 1000) < o.simplexFrac * 1000;
        b.downlink = (r() % 1000) >= o.uplinkFrac * 1000;
        b.preamble = simplex ? iridium::kPreambleLong : iridium::kPreambleShort;
        const int nsym = simplex && o.longSimplex ? 100 + (int)(r() % 300) : simplex ? 80 + (int)(r() % 100) : 120 + (int)(r() % 60);
        b.bits.resize(2 * nsym);
        for (auto& x : b.bits) x = (uint8_t)(r() & 1);
        int ch = (int)(r() % nCh);
        const double dur = (b.preamble + 12 + nsym + 8) / iridium::kSymbolRate;
        b.startSec = t;
        if (o.overlap) {
            // a burst may start while others are on: keep 3 channels away from them
            for (int tries = 0; tries < 50; tries++) {
                bool clash = false;
                for (const auto& q : busy) if (q.t1 > t - 0.001 && std::abs(q.ch - ch) < 3) clash = true;
                if (!clash) break;
                ch = (int)(r() % nCh);
            }
        }
        const double dop = o.dopplerMax > 0 ? ((r() % 20001) / 10000.0 - 1) * o.dopplerMax : 0;
        b.freqHz = (ch - nCh / 2.0 + 0.5) * iridium::kChannelHz + o.cfoHz + dop;
        b.dopplerRate = (r() & 1 ? 1 : -1) * o.dopplerRate;
        b.amplitude = (float)amp;
        b.phase = (r() % 6283) / 1000.0;
        v.push_back(b);
        busy.push_back(Busy{t, t + dur, ch});
        if (busy.size() > 64) busy.erase(busy.begin());
        t += o.overlap ? gap * (0.2 + 1.6 * (r() % 1000) / 1000.0) : std::max(gap, dur + 0.0015);
    }
    return v;
}

struct RunOpts {
    double sroPpm = 0;
    size_t chunk = 4096;
    bool quant8 = false;            // round to 8 bits like a HackRF (full scale 1.0)
    cf32 dc = cf32(0, 0);           // DC offset added to the input
    double gapAt = -1, gapSecs = 0.02;   // samples lost: the stream jumps
    double resetAt = -1;            // reset() at this time
    double noiseSigma = 0.05;       // per component
    double centerMhz = 1622;
    bool offline = true;
    uint32_t noiseSeed = 7;
};

struct Got {
    IridiumBurstBits bits;
    IridiumType type = IridiumType::Unknown;
};

struct RunResult {
    std::vector<Got> got;
    IridiumTelemetry tel;
    double cpuSecs = 0;             // time spent in feed() and flush()
    double signalSecs = 0;
};

// Plays the bursts with noise into a receiver. got: every burst with a unique word (times relative to the start of the stream;
// after a reset the receiver's clock restarts: those times are shifted back to the stream's).
inline RunResult runScene(const std::vector<IridiumTestBurst>& bursts, double rate, double secs, const RunOpts& ro, IridiumReceiver* rxIn = nullptr) {
    RunResult res;
    IridiumReceiver own;
    IridiumReceiver& rx = rxIn ? *rxIn : own;
    rx.configure(rate);
    rx.setCenterMhz(ro.centerMhz);
    rx.setOffline(ro.offline);
    std::mutex m;
    double timeBase = 0;       // stream time of the receiver's time 0
    rx.setBurstCallback([&](const IridiumBurstBits& b, const IridiumFrame& f) {
        std::lock_guard<std::mutex> lk(m);
        Got g; g.bits = b; g.bits.timeSec += timeBase; g.type = f.type;
        res.got.push_back(g);
    });
    IridiumBurstPlayer pl(rate, ro.sroPpm);
    for (const auto& b : bursts) pl.add(b);
    genutil::NoiseSource ns(ro.noiseSeed);
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(ro.chunk);
    size_t done = 0;
    bool gapDone = false, resetDone = false;
    double cpu = 0;
    while (done < total) {
        const size_t n = std::min(ro.chunk, total - done);
        std::fill(buf.begin(), buf.begin() + n, cf32(0, 0));
        pl.render(buf.data(), n);
        ns.add(buf.data(), n, (float)ro.noiseSigma);
        for (size_t i = 0; i < n; i++) {
            cf32 v = buf[i] + ro.dc;
            if (ro.quant8) v = cf32(std::round(std::clamp(v.real(), -1.f, 0.992f) * 128) / 128, std::round(std::clamp(v.imag(), -1.f, 0.992f) * 128) / 128);
            buf[i] = v;
        }
        const double tNow = done / rate;
        bool feedIt = true;
        if (ro.gapAt >= 0 && !gapDone && tNow >= ro.gapAt) {
            // drop gapSecs of samples: the receiver's clock now runs behind the stream
            if (tNow < ro.gapAt + ro.gapSecs) feedIt = false;
            else gapDone = true;
        }
        if (ro.resetAt >= 0 && !resetDone && tNow >= ro.resetAt) {
            rx.flush();
            rx.reset();
            std::lock_guard<std::mutex> lk(m);
            timeBase = tNow;
            resetDone = true;
        }
        if (feedIt) {
            const auto c0 = std::chrono::steady_clock::now();
            rx.feed(buf.data(), n);
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        } else if (ro.gapAt >= 0) {
            std::lock_guard<std::mutex> lk(m);
            timeBase += n / rate;
        }
        done += n;
    }
    const auto c0 = std::chrono::steady_clock::now();
    rx.flush();
    cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    rx.telemetry(res.tel, 0);
    rx.setBurstCallback(nullptr);
    res.cpuSecs = cpu;
    res.signalSecs = secs;
    return res;
}

struct Match {
    int sent = 0, found = 0, exact = 0, bitErrors = 0, wrongDir = 0, extra = 0;
    double maxFreqErr = 0, maxTimeErr = 0;
};

// For every sent burst the received one at the same time (+-60 us) and frequency (+-3 kHz); exact = every bit right.
inline Match match(const std::vector<IridiumTestBurst>& sent, const std::vector<Got>& got, double centerHz, double fromSec = 0, double toSec = 1e9, double sroPpm = 0) {
    Match m;
    std::vector<bool> used(got.size(), false);
    for (const auto& s : sent) {
        // the receiver's clock: sample count / nominal rate; a clock offset stretches its times and shrinks its frequencies
        const double tUw = (s.startSec + s.preamble / iridium::kSymbolRate) * (1 + sroPpm * 1e-6);
        if (tUw < fromSec || tUw > toSec) continue;
        m.sent++;
        int best = -1;
        for (size_t i = 0; i < got.size(); i++) {
            if (used[i]) continue;
            const double dt = got[i].bits.timeSec - tUw, df = got[i].bits.freqHz - centerHz - s.freqHz / (1 + sroPpm * 1e-6);
            if (std::fabs(dt) < 60e-6 && std::fabs(df) < 3e3) { best = (int)i; break; }
        }
        if (best < 0) continue;
        used[best] = true;
        m.found++;
        const Got& g = got[best];
        m.maxTimeErr = std::max(m.maxTimeErr, std::fabs(g.bits.timeSec - tUw));
        m.maxFreqErr = std::max(m.maxFreqErr, std::fabs(g.bits.freqHz - centerHz - s.freqHz / (1 + sroPpm * 1e-6)));
        if (g.bits.downlink != s.downlink) { m.wrongDir++; continue; }
        int e = 0;
        const size_t nb = std::min(g.bits.bits.size(), s.bits.size());
        for (size_t i = 0; i < nb; i++) e += g.bits.bits[i] != s.bits[i];
        e += (int)(s.bits.size() - nb);
        m.bitErrors += e;
        if (e == 0) m.exact++;
    }
    for (size_t i = 0; i < got.size(); i++) if (!used[i]) m.extra++;
    return m;
}

} // namespace iridiumtest
} // namespace dect2
