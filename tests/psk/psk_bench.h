// PSK31 threshold bench (SENSITIVITY-3.9-B): one signal through the whole PskReceiver at 250 kHz, SNR in the signal's bandwidth (the baud
// rate, as PskGenSignal defines it), optional tuning error and drift. A unit is a word of the sent text found in the decoded text.
#pragma once
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace sens39b {

struct PskResult { int words = 0, found = 0; uint64_t chars = 0; double cpuPerSec = 0; double firstSec = -1; };

inline PskResult pskRun(double snrDb, uint32_t seed, int mode = 0, double hz = 1000, double cfo = 0, double driftHzPerMin = 0, double secs = 45) {
    using namespace dect2;
    const double rate = 250000, off = -20000;
    PskGenSignal s;
    s.offsetHz = hz; s.mode = mode; s.snrDb = snrDb; s.repeat = false; s.driftHzPerMin = driftHzPerMin; s.startSec = 0.3 + 0.01 * (seed % 50);
    s.phase0 = 0.37 * seed;
    std::string text;
    for (int i = 0; i < 3; i++) text += "LY2ABC DE G4XYZ" + std::to_string(seed % 10) + " UR RST 579 NAME JOHN QTH LONDON " + std::to_string(i) + "\n";
    s.text = text;
    PskGenConfig c;
    c.rate = rate; c.dialOffsetHz = off; c.cfoHz = cfo; c.signals = {s}; c.seed = seed;
    PskGenerator g(c);
    PskReceiver r;
    r.setSilent(true);
    r.configure(rate);
    r.setSignalOffset(off);
    r.setMode(mode);
    PskTelemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    double gen = 0, dec = 0;
    PskResult out;
    for (double t = 0; t < secs; t += 0.1) {
        auto a = std::chrono::steady_clock::now();
        g.generate(x.data(), x.size());
        auto b = std::chrono::steady_clock::now();
        r.feed(x.data(), x.size());
        auto e = std::chrono::steady_clock::now();
        gen += std::chrono::duration<double>(b - a).count(); dec += std::chrono::duration<double>(e - b).count();
        PskTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    out.cpuPerSec = dec / secs;
    if (getenv("PSKDBG")) for (const auto& sg : res.signals) fprintf(stderr, "  sig %.1f Hz chars %llu snr %.1f dcd %d text [%s]\n", sg.offsetHz, (unsigned long long)sg.chars, sg.snrDb, sg.dcd, sg.text.c_str());
    std::string got;
    for (const auto& sg : res.signals) if (std::fabs(sg.offsetHz - (hz + cfo)) < 40 && sg.text.size() > got.size()) { got = sg.text; out.chars = sg.chars; }
    // words in order: each sent word is looked for after the previous one found
    std::istringstream is(text);
    std::string w;
    size_t pos = 0;
    while (is >> w) {
        out.words++;
        const size_t p = got.find(w, pos);
        if (p != std::string::npos && p < pos + 3 * w.size() + 40) { out.found++; pos = p + w.size(); }
    }
    return out;
}

} // namespace sens39b
