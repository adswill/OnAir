// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL over a radio chain: the 48 kHz transmitter output is resampled to the radio rate, mixed by a carrier offset (the two radios' oscillators) with a
// sample clock error, then goes to the receiver at the radio rate (it resamples to 48 kHz itself). Every case must lock, decode frames and read the beacon.
#include "dect2/dsp/resampler.h"
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kaal/kaal_txgen.h"
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static int gBitrate = 16;

static TxRequest request() {
    static uint64_t pulled = 0;
    TxRequest r;
    r.modeId = "kaal";
    r.callsign = "LY2ABC";
    r.grid = "KO26ab";
    r.lat = 54.687; r.lon = 25.279;
    r.text = "Hi, I am testing KAAL.";
    r.opt["kaal.powerDbm"] = 20;
    r.opt["kaal.beacon"] = 1;
    r.opt["kaal.bitrate"] = gBitrate;
    pulled = 0;
    r.pullAudio = [](float* st, int n) {
        for (int i = 0; i < n; i++, pulled++) {
            const double t = std::fmod((double)pulled / 48000.0, 6.0);
            const float v = (float)(0.09 * std::sin(2 * 3.141592653589793 * (150 + 60 * std::sin(2 * 3.141592653589793 * 1.3 * t)) * t) * (0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 3 * t)));
            st[2 * i] = st[2 * i + 1] = v;
        }
        return n;
    };
    return r;
}

// sample clock error, streaming: the receiver's clock runs (1 + ppm) times the transmitter's (cubic Lagrange interpolation)
struct ClockError {
    double step = 1.0, t = 2;
    int64_t base = 0;
    std::vector<cf32> buf;
    void process(const cf32* x, size_t n, std::vector<cf32>& y) {
        buf.insert(buf.end(), x, x + n);
        while (t - (double)base < (double)buf.size() - 3) {
            const int64_t i = (int64_t)t;
            const double f = t - (double)i;
            const size_t o = (size_t)(i - base);
            const double w0 = -f * (f - 1) * (f - 2) / 6, w1 = (f + 1) * (f - 1) * (f - 2) / 2, w2 = -(f + 1) * f * (f - 2) / 2, w3 = (f + 1) * f * (f - 1) / 6;
            y.push_back(buf[o - 1] * (float)w0 + buf[o] * (float)w1 + buf[o + 1] * (float)w2 + buf[o + 2] * (float)w3);
            t += step;
        }
        const int64_t keep = (int64_t)t - 4;
        if (keep > base + 8192) { buf.erase(buf.begin(), buf.begin() + (keep - base)); base = keep; }
    }
};

struct Result { bool locked = false; double lockAt = -1; int lost = 0; uint64_t ok = 0, bad = 0; std::string bio; double cfo = 0, beaconAt = -1, bioAt = -1; int symRate = 0; };

// txRate: the rate the transmit framework gives the generator (a HackRF); rxRate: what the receiving radio runs (an RTL-SDR); the receiver gets 8-bit samples
// with a DC spike at the tune centre. cfoHz is the offset of the receiver's tune from the transmitter's carrier (the signal is at +cfoHz in the receiver).
static Result run(double txRate, double rxRate, double cfoHz, double ppm, double secs, bool eightBit, const std::function<void(const cf32*, size_t)>& sink = nullptr) {
    TxRequest rq = request();
    std::string err;
    auto gen = makeKaalTxGenerator(rq, txRate, err);
    Result res;
    if (!gen) return res;
    ClockError ce;
    ce.step = 1.0 - ppm * 1e-6;
    RationalResampler rs;
    rs.configure(txRate, rxRate);
    KaalLiveReceiver rx;
    rx.setSilent(true);
    rx.configure(rxRate);
    if (getenv("KAAL_LOG")) rx.setLogCallback([](const std::string& m) { printf("  log: %s\n", m.c_str()); });
    uint64_t sq = 0;
    double ph = 0;
    const double dph = 2 * 3.141592653589793 * cfoHz / rxRate;
    std::vector<cf32> blk((size_t)(txRate / 20)), a, b;
    KaalTelemetry t;
    float gain = 0;
    for (size_t done = 0; done < (size_t)(secs * txRate); done += blk.size()) {
        gen->generate(blk.data(), blk.size());
        if (gain == 0) {
            double p = 0;
            for (const cf32& v : blk) p += std::norm(v);
            gain = (float)(0.22 / std::sqrt(p / (double)blk.size() + 1e-30));   // about -13 dBFS rms into the receiver's converter
        }
        a.clear(); b.clear();
        ce.process(blk.data(), blk.size(), a);
        rs.process(a.data(), a.size(), b);
        for (cf32& s : b) {
            s *= gain * cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += dph;
            if (ph > 6.283185307179586) ph -= 6.283185307179586;
            if (ph < -6.283185307179586) ph += 6.283185307179586;
            if (eightBit) {   // an RTL-SDR: 8 bits, a DC spike at the tune centre
                const float dc = 0.02f;
                auto q = [&](float v) { return std::round(std::max(-1.f, std::min(127.f / 128.f, v + dc)) * 128.f) / 128.f; };
                s = cf32(q(s.real()), q(s.imag()));
            }
        }
        if (sink) sink(b.data(), b.size());
        rx.feed(b.data(), b.size());
        if (rx.telemetry(t, sq)) {
            sq = t.seq;
            if (t.state == 2 && !res.locked) { res.locked = true; res.lockAt = t.timeSec; }
            else if (res.locked && t.state != 2 && t.timeSec > 15) res.lost++;   // before that the receiver may search again once (carrier refine, block alignment)
            res.ok = t.blocksOk; res.bad = t.blocksBad; res.cfo = t.cfoHz; res.symRate = t.symRate;
            if (t.beaconValid && res.beaconAt < 0) res.beaconAt = t.timeSec;
            if (t.beaconValid && !t.text.empty() && res.bio.empty()) { res.bio = t.text; res.bioAt = t.timeSec; }
        }
    }
    return res;
}

// the same chain, but the receiver is the real engine reading an 8-bit IQ file at the radio rate (what an RTL-SDR recording is): ./test_kaal_chain engine <path> [cfo]
static int engineRun(const char* path, double cfo) {
    FILE* f = fopen(path, "wb");
    if (!f) return 2;
    run(2e6, 2.4e6, cfo, 2, 26, true, [&](const cf32* x, size_t n) {
        std::vector<int8_t> o(2 * n);
        for (size_t i = 0; i < n; i++) { o[2 * i] = (int8_t)std::lround(x[i].real() * 128.f); o[2 * i + 1] = (int8_t)std::lround(x[i].imag() * 128.f); }
        fwrite(o.data(), 1, o.size(), f);
    });
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "engine") return engineRun(argv[2], argc > 3 ? atof(argv[3]) : 3000);
    if (argc == 8) gBitrate = atoi(argv[7]);
    if (argc == 7 || argc == 8) {   // tx rx cfo ppm secs eightBit: one case, for looking into a failure
        const Result r = run(atof(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]), atof(argv[5]), atoi(argv[6]) != 0);
        printf("lock %d at %.1f s, lost %d, ok %llu bad %llu, cfo %.0f, beacon %.1f bio %.1f\n", (int)r.locked, r.lockAt, r.lost, (unsigned long long)r.ok, (unsigned long long)r.bad, r.cfo, r.beaconAt, r.bioAt);
        return 0;
    }
    // HackRF transmit at 2 Msps (and 10 Msps once), RTL-SDR receive at 2.4 and 2.048 Msps (the second is a non-integer ratio to 48 kHz), 8-bit samples
    struct Case { double tx, rx, cfo, ppm; int kb; };
    const Case cases[] = {
        {2e6, 2.4e6, 0, 0, 16},      {2e6, 2.4e6, 1500, 2, 16},   {2e6, 2.4e6, -1500, -2, 16},  {2e6, 2.048e6, 1500, 1, 16}, {2e6, 2.048e6, -1500, -1, 16}, {2e6, 2.048e6, 0, 0, 16},
        {2e6, 2.4e6, 3000, 5, 16},   {2e6, 2.4e6, -3000, -5, 16}, {2e6, 2.4e6, 4000, 0, 16},    {2e6, 2.4e6, -4000, 0, 16},  {2e6, 2.4e6, 7000, 0, 16},
        {10e6, 2.4e6, 2500, 3, 16},
        {2e6, 2.4e6, 0, 0, 12},      {2e6, 2.048e6, -1500, -1, 12}, {2e6, 2.4e6, 4000, 2, 12},
        {2e6, 2.4e6, 0, 0, 24},      {2e6, 2.4e6, 1500, 2, 24},   {2e6, 2.048e6, -1500, -1, 24}, {2e6, 2.4e6, 4000, 0, 24}, {2e6, 2.4e6, -4000, -2, 24}, {2e6, 2.048e6, 3000, 1, 24},
        {2e6, 2.4e6, 0, 0, 32},      {2e6, 2.048e6, 1500, 1, 32}, {2e6, 2.4e6, -4000, 0, 32},  {2e6, 2.4e6, 4000, 2, 32},
    };
    for (const Case& c : cases) {
        gBitrate = c.kb;
        const Result r = run(c.tx, c.rx, c.cfo, c.ppm, c.kb >= 24 ? 14 : 36, true);   // the narrow formats take their time to show the bio (one full frame in five)
        printf("%2d kbit/s tx %.3f Msps rx %.3f Msps cfo %6.0f ppm %3.0f: lock %d at %.1f s, lost %d, frames ok %llu bad %llu, cfo read %.0f, %d ksym/s, beacon at %.1f s, bio at %.1f s\n", c.kb, c.tx / 1e6,
               c.rx / 1e6, c.cfo, c.ppm, (int)r.locked, r.lockAt, r.lost, (unsigned long long)r.ok, (unsigned long long)r.bad, r.cfo, r.symRate / 1000, r.beaconAt, r.bioAt);
        fflush(stdout);
        CHECK(r.locked && r.lost == 0 && r.lockAt < 12, "%d kbit/s tx %.0f rx %.0f cfo %.0f: lock %d at %.1f lost %d", c.kb, c.tx, c.rx, c.cfo, (int)r.locked, r.lockAt, r.lost);
        CHECK(r.ok > 300 && r.bad * 20 < r.ok + 1, "%d kbit/s rx %.0f cfo %.0f: frames ok %llu bad %llu", c.kb, c.rx, c.cfo, (unsigned long long)r.ok, (unsigned long long)r.bad);
        CHECK(r.symRate == (c.kb >= 24 ? 16000 : 8000), "%d kbit/s: symbol rate read %d", c.kb, r.symRate);
        CHECK(r.beaconAt > 0 && r.beaconAt < 14, "%d kbit/s cfo %.0f: first beacon at %.1f s", c.kb, c.cfo, r.beaconAt);
        CHECK(!r.bio.empty(), "%d kbit/s cfo %.0f: no bio", c.kb, c.cfo);
    }
    return testSummary("kaal_chain");
}
