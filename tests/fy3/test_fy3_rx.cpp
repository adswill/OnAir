// FY-3 AHRPT / MPT end to end on the built-in test signal (fy3_gen.h; no off-air recording is available): the receivers with carrier offsets up to
// +-45 kHz, a Doppler ramp, symbol clock error, noise, every carrier phase and both polarities / arrangements of the rails, sample rates from 4 to
// 20 Msps, the symbol rate measured by itself (2.6 and 2.8 Msym/s). Every VIRR line that comes back must equal the test line its on-board time
// names, pixel for pixel. Noise alone gives nothing.
#include "dect2/fy3/fy3_gen.h"
#include "dect2/fy3/fy3_proto.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/fy3/fy3_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

struct Case {
    const char* name; int product; double rate, rs, snr, cfo, ppm, phase, drift, seconds;
    bool conj, swap; int sat; int preset; double duty; int minRows;
};

static Fy3Telemetry run(const Case& k, bool check = true) {
    Fy3GenConfig g;
    g.rate = k.rate; g.product = k.product; g.symbolRate = k.rs; g.snrDb = k.snr; g.cfoHz = k.cfo; g.clockPpm = k.ppm; g.phaseDeg = k.phase;
    g.driftHzPerS = k.drift; g.conjugate = k.conj; g.swapIq = k.swap; g.sat = k.sat; g.duty = k.duty; g.seed = 9;
    Fy3Generator gen(g);
    Fy3Receiver rx(k.product);
    rx.configure(k.rate);
    rx.setPreset(k.preset);
    std::vector<cf32> buf(65536);
    Fy3Telemetry t;
    uint64_t seq = 0;
    double cpu = 0;
    for (double done = 0; done < k.seconds * k.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        const double t0 = processCpuSecs();
        rx.feed(buf.data(), buf.size());
        cpu += (processCpuSecs() - t0);
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    printf("  %s: %s; cfo %.0f Hz (sent %.0f), Es/N0 %.1f dB, lock %.2f, rs %.4f%s, %llu ok %llu bad, rails %d/%d hyp %d, %llu rows, RS %llu, receiver %.2f s CPU for %.2f s\n",
           k.name, fy3Summary(t).c_str(), t.cfoHz, k.cfo, t.snrDb, t.carrierLock, t.symbolRate / 1e6, t.rateMeasured > 0 ? " (measured)" : "", (unsigned long long)t.blocksOk,
           (unsigned long long)t.blocksBad, t.railPhase[0], t.railPhase[1], t.diffMode, (unsigned long long)t.lines, (unsigned long long)t.rsCorrected, cpu, k.seconds);
    if (!check) return t;
    CHECK(t.state == 3, "%s: state %d (%s)", k.name, t.state, t.hint.c_str());
    CHECK(std::fabs(t.cfoHz - k.cfo) < 3000, "%s: carrier %.0f", k.name, t.cfoHz);
    CHECK(t.blocksBad <= 3 + t.blocksOk / 200, "%s: %llu bad of %llu", k.name, (unsigned long long)t.blocksBad, (unsigned long long)(t.blocksOk + t.blocksBad));
    CHECK(t.sat == k.sat, "%s: satellite %d", k.name, t.sat);
    CHECK((int)t.lines >= k.minRows, "%s: %llu rows", k.name, (unsigned long long)t.lines);
    return t;
}

static void checkVirr(const Case& k, const Fy3Telemetry& t) {
    const Fy3Image* im = nullptr;
    for (const auto& i : t.images) if (i->inst == fy3::Virr) im = i.get();
    CHECK(im != nullptr, "%s: no VIRR picture", k.name);
    if (!im) return;
    int exact = 0, seen = 0, badPx = 0;
    for (const auto& l : im->row) {
        if (!l) continue;
        seen++;
        const int n = (int)std::lround(((double)l->ms - 36000000.0) / fy3::kVirrLineMs);
        bool ok = (int)l->px.size() == fy3::kVirrChannels * fy3::kVirrWidth;
        for (int c = 0; ok && c < fy3::kVirrChannels; c++)
            for (int x = 0; x < fy3::kVirrWidth; x += 7)
                if (l->px[(size_t)c * fy3::kVirrWidth + (size_t)x] != (fy3::testPixel(c, x, n) >> 2)) { ok = false; badPx++; break; }
        if (ok) exact++;
    }
    CHECK(seen > 0 && exact == seen, "%s: %d of %d picture rows exact (%d with wrong pixels)", k.name, exact, seen, badPx);
    CHECK(im->channels == fy3::kVirrChannels && im->width == fy3::kVirrWidth, "%s: picture shape", k.name);
}

int main() {
    const Case cases[] = {
        //  name                     prod rate   rs      snr  cfo     ppm  phase drift  secs conj  swap  sat preset duty minRows
        {"AHRPT FY-3C 8M",            0, 8e6,    2.6e6,  30,  0,      0,   0,    0,     1.5, false, false, 2, 0, 0.5, 6},
        {"AHRPT FY-3C 8M +45k",       0, 8e6,    2.6e6,  30,  45000,  0,   0,    0,     1.5, false, false, 2, 0, 0.5, 6},
        {"AHRPT FY-3C 8M -45k ramp",  0, 8e6,    2.6e6,  12,  -45000, 20,  70,   -80,    2.0, false, false, 2, 0, 0.5, 6},
        {"AHRPT FY-3A 2.8M auto",     0, 8e6,    2.8e6,  15,  12000,  -30, 200,  40,     2.0, false, false, 0, 0, 0.5, 6},
        {"AHRPT conj",                0, 8e6,    2.6e6,  15,  3000,   0,   300,  0,      1.5, true,  false, 2, 0, 0.5, 6},
        {"AHRPT swap",                0, 8e6,    2.6e6,  15,  -9000,  0,   10,   0,      1.5, false, true,  2, 0, 0.5, 6},
        {"AHRPT conj+swap",           0, 8e6,    2.6e6,  15,  1000,   0,   100,  0,      1.5, true,  true,  1, 0, 0.5, 6},
        {"AHRPT 4M",                  0, 4e6,    2.6e6,  25,  20000,  0,   0,    0,     1.5, false, false, 2, 0, 0.5, 6},
        {"AHRPT 4M 2.8",              0, 4e6,    2.8e6,  25,  -20000, 0,   0,    0,     1.5, false, false, 0, 2, 0.5, 6},
        {"AHRPT 6M",                  0, 6e6,    2.6e6,  12,  15000,  0,   0,    0,     1.5, false, false, 2, 0, 0.5, 6},
        {"AHRPT 10M",                 0, 10e6,   2.8e6,  12,  -30000, 0,   0,    0,     1.5, false, false, 1, 0, 0.5, 6},
        {"AHRPT 20M",                 0, 20e6,   2.6e6,  12,  25000,  0,   0,    0,     1.5, false, false, 2, 0, 0.5, 6},
    };
    for (const Case& k : cases) {
        const Fy3Telemetry t = run(k);
        if (k.sat <= 2 && t.state == 3) checkVirr(k, t);
    }
    {   // noise only: nothing decodes, nothing is claimed
        Case k = {"noise only", 0, 8e6, 2.6e6, -30, 0, 0, 0, 0, 2.0, false, false, 2, 0, 0.5, 0};
        Fy3Generator* unused = nullptr; (void)unused;
        Fy3GenConfig g; g.rate = 8e6; g.snrDb = -40; g.seed = 4;
        Fy3Generator gen(g);
        Fy3Receiver rx(0); rx.configure(8e6);
        std::vector<cf32> buf(65536);
        Fy3Telemetry t; uint64_t seq = 0;
        for (int i = 0; i < 160; i++) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); if (rx.telemetry(t, seq)) seq = t.seq; }
        printf("  %s: %s, %llu frames, hint \"%s\"\n", k.name, fy3Summary(t).c_str(), (unsigned long long)t.blocksOk, t.hint.c_str());
        CHECK(t.blocksOk == 0 && t.lines == 0 && t.state < 3, "noise decoded %llu frames", (unsigned long long)t.blocksOk);
        CHECK(!t.hint.empty(), "no hint");
    }
    {   // MPT: MERSI-1 scans (a reduced symbol rate keeps the test short; the receiver takes any rate)
        Case k = {"MPT 6 Msym/s at 12 Msps", 1, 12e6, 6e6, 15, 20000, 0, 0, 0, 5.5, false, false, 2, 0, 1.0, 0};
        Fy3GenConfig g;
        g.rate = k.rate; g.product = 1; g.symbolRate = k.rs; g.snrDb = k.snr; g.cfoHz = k.cfo; g.sat = k.sat; g.duty = 1.0; g.seed = 3;
        Fy3Generator gen(g);
        Fy3Receiver rx(1);
        rx.configure(k.rate);
        rx.setSymbolRate(6e6);
        std::vector<cf32> buf(65536);
        Fy3Telemetry t; uint64_t seq = 0;
        for (double done = 0; done < k.seconds * k.rate; done += (double)buf.size()) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); if (rx.telemetry(t, seq)) seq = t.seq; }
        printf("  MPT: %s, %llu ok %llu bad, vc3 %llu frames, %llu rows, hint \"%s\"\n", fy3Summary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
               (unsigned long long)t.vcidFrames[3], (unsigned long long)t.lines, t.hint.c_str());
        CHECK(t.state == 3 && t.blocksOk > 100 && t.vcidFrames[3] > 100, "MPT frames");
        CHECK(t.blocksBad <= 3, "MPT bad %llu", (unsigned long long)t.blocksBad);
        const Fy3Image* im = nullptr;
        for (const auto& i : t.images) if (i->inst == fy3::Mersi1) im = i.get();
        CHECK(im != nullptr && im->rows >= 10, "MERSI picture %p", (const void*)im);
        if (im) {
            int bad = 0, seen = 0;
            for (const auto& l : im->row) {
                if (!l) continue;
                seen++;
                // the 1 km channels (index 5 on) are the test pixel's top eight bits
                const int ch = 7;
                for (int x = 0; x < 2048; x += 53) if (l->px[(size_t)ch * 2048 + (size_t)x] != ((fy3::testPixel(ch, x, l->idx) << 2) >> 4)) bad++;
            }
            CHECK(seen >= 8 && bad == 0, "MERSI rows %d, %d wrong pixels", seen, bad);
        }
    }
    return testSummary("fy3 rx");
}
