// PSK demodulator receiver on its own test signal (makePskdSynth): it finds, analyses and locks every order with nothing set by hand,
// with the radio's tuning error, off the dial, at the radio's 2 Msps with the dial 250 kHz from the centre, as a 48 kHz recording, a slow
// narrow signal in a wide span (the zoom), a pass's Doppler ramp; the sync words are counted and the bits are saved; settings given by hand
// are used; noise alone never locks.
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include "dect2/pskdsp/pskdsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Run { PskdTelemetry t; };

static PskdTelemetry run(const SynthConfig& cfg, double rate, double dialOffset, double seconds, const PskdSettings* s = nullptr, bool noiseOnly = false) {
    PskdReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(dialOffset);
    if (s) rx.setSettings(*s);
    auto syn = makePskdSynth(cfg, rate);
    std::vector<cf32> buf(8192);
    PskdTelemetry t;
    uint64_t seq = 0;
    uint32_t r = 5;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) {
                r = r * 1664525u + 1013904223u; const float a = ((float)(r >> 8) / 16777216.f - 0.5f) * 0.3f;
                r = r * 1664525u + 1013904223u; const float b = ((float)(r >> 8) / 16777216.f - 0.5f) * 0.3f;
                v = cf32(a, b);
            }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        rx.telemetry(t, seq) && (seq = t.seq);
    }
    return t;
}

static void show(const char* name, const PskdTelemetry& t) {
    printf("%-46s state %d %s %.2f Bd %s, Eb/N0 %.1f dB, carrier %+.1f Hz, syncs %llu, bits %llu (%s)\n", name, t.state, pskdOrderName(t.order), t.baud,
           t.shape.c_str(), t.ebn0Db, t.cfoHz, (unsigned long long)t.syncs, (unsigned long long)t.bitsTotal, t.status.c_str());
    if (getenv("PSK_VERBOSE") || t.state != 2) for (const auto& l : t.log) printf("    %s\n", l.c_str());
}

int main() {
    struct Case { const char* name; int order; double baud, alpha, rate, dial, cfo, dop, snr, secs; int wantOrder; };
    const Case cases[] = {
        {"BPSK 1200 Bd at 250 kHz, +2.5 kHz off", 0, 1200, 0.35, 250000, 0, 2500, 0, 22, 4, 1},
        {"QPSK 9600 Bd at 250 kHz, -6 kHz off", 1, 9600, 0.35, 250000, 0, -6000, 0, 22, 2, 2},
        {"OQPSK 4800 Bd at 250 kHz", 2, 4800, 0.5, 250000, 0, 800, 0, 22, 3, 3},
        {"8PSK 4800 Bd at 250 kHz", 3, 4800, 0.35, 250000, 0, -1200, 0, 28, 3, 4},
        {"BPSK 1200 Bd at 2 Msps, dial 250 kHz from centre", 0, 1200, 0.35, 2000000, -250000, 1500, 0, 22, 3, 1},
        {"BPSK 2400 Bd at 48 kHz (a recording)", 0, 2400, 0.35, 48000, 0, 0, 0, 20, 3, 1},
        {"BPSK 150 Bd in a 200 kHz span (zoom)", 0, 150, 0.35, 250000, 0, 4000, 0, 24, 14, 1},
        {"BPSK 4800 Bd, Doppler 150 Hz/s", 0, 4800, 0.35, 250000, 0, 0, 150, 22, 8, 1},
        {"QPSK 2400 Bd rectangular", 1, 2400, -1, 250000, 0, 1000, 0, 22, 3, 2},
    };
    for (const Case& c : cases) {
        SynthConfig cfg;
        cfg.modeOpt[0] = c.order; cfg.modeVal[0] = c.baud; cfg.modeVal[1] = c.alpha; cfg.modeVal[2] = c.dop; cfg.cfoHz = c.cfo; cfg.snrDb = c.snr;
        PskdSettings s; s.sync = "1ACFFC1D";
        PskdTelemetry t = run(cfg, c.rate, c.dial, c.secs, &s);
        show(c.name, t);
        CHECK(t.state == 2, "%s: not locked", c.name);
        CHECK(t.order == c.wantOrder, "%s: order %s", c.name, pskdOrderName(t.order));
        CHECK(std::fabs(t.baud - c.baud) < 0.005 * c.baud, "%s: baud %.2f", c.name, t.baud);
        // the test signal has a sync word every 992 bits
        const double bitsPerSec = c.baud * (c.order == 0 ? 1 : c.order == 3 ? 3 : 2);
        CHECK(t.syncs >= (uint64_t)std::max(1.0, 0.3 * c.secs * bitsPerSec / 992), "%s: %llu sync words", c.name, (unsigned long long)t.syncs);
        if (c.dop == 0) CHECK(std::fabs(t.cfoHz - c.cfo) < std::max(5.0, 0.01 * c.baud), "%s: carrier %+.1f", c.name, t.cfoHz);
    }
    // settings by hand: order, rate and roll-off given, differential coding, the bits saved as bytes
    {
        SynthConfig cfg;
        cfg.modeOpt[0] = 1; cfg.modeOpt[1] = 1; cfg.modeVal[0] = 4800; cfg.snrDb = 22;
        PskdSettings s; s.order = 2; s.baud = 4800; s.alpha = 0.35; s.diff = 1; s.sync = "1ACFFC1D";
        const std::string path = "pskdsp_test_bits.bin";
        std::remove(path.c_str());
        s.saveBits = true; s.savePath = path;
        PskdTelemetry t = run(cfg, 250000, 0, 3, &s);
        show("QPSK DPSK, everything set by hand, saving", t);
        CHECK(t.state == 2 && t.syncs >= 5, "by hand: state %d syncs %llu", t.state, (unsigned long long)t.syncs);
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        const long sz = f ? (long)f.tellg() : 0;
        CHECK(sz > 1000 && (uint64_t)sz == t.bytesSaved, "saved %ld bytes, told %llu", sz, (unsigned long long)t.bytesSaved);
        std::remove(path.c_str());
    }
    // noise alone
    {
        SynthConfig cfg;
        PskdTelemetry t = run(cfg, 250000, 0, 4, nullptr, true);
        show("noise only", t);
        CHECK(t.state != 2, "noise locked");
    }
    return testSummary("pskdsp_rx");
}
