// HRPT / AHRPT end to end on the built-in test signal (hrpt_gen.h; no off-air recording exists for this mode): MetOp AHRPT, FY-3 AHRPT and
// NOAA HRPT through the receiver with carrier offsets up to +-45 kHz, a Doppler ramp, symbol clock error, noise and different carrier
// phases (the QPSK phase ambiguity), at 8 Msps and near the lowest rate. Every picture line that comes back must equal the test line
// its on-board time names, pixel for pixel. Noise alone gives nothing.
#include "dect2/hrpt/hrpt_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/hrpt/hrpt_tel.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

struct Case { const char* name; int variant; double rate, snr, cfo, ppm, phase, drift, seconds; bool invert; int minLines; };

static void run(const Case& k) {
    HrptGenConfig g;
    g.rate = k.rate; g.variant = k.variant; g.snrDb = k.snr; g.cfoHz = k.cfo; g.clockPpm = k.ppm; g.phaseDeg = k.phase;
    g.driftHzPerS = k.drift; g.invert = k.invert; g.lines = 400; g.seed = 9;
    HrptGenerator gen(g);
    HrptReceiver rx;
    rx.configure(k.rate);
    rx.setVariant(k.variant);
    std::vector<cf32> buf(65536);
    HrptTelemetry t;
    uint64_t seq = 0;
    double cpu = 0;
    for (double done = 0; done < k.seconds * k.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        const double t0 = processCpuSecs();
        rx.feed(buf.data(), buf.size());
        cpu += (processCpuSecs() - t0);
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    printf("  %s: %s; cfo %.0f Hz (sent %.0f), Es/N0 %.1f dB, lock %.2f, %llu ok %llu bad, rotation %d, %llu lines, RS %llu, receiver %.2f s CPU for %.2f s\n",
           k.name, hrptSummary(t).c_str(), t.cfoHz, k.cfo, t.snrDb, t.carrierLock, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.rotation,
           (unsigned long long)t.lines, (unsigned long long)t.rsCorrected, cpu, k.seconds);
    CHECK(t.state == 3, "%s: state %d", k.name, t.state);
    CHECK(std::fabs(t.cfoHz - k.cfo) < 3000, "%s: carrier %.0f", k.name, t.cfoHz);
    CHECK(t.variant == k.variant, "%s: variant %d", k.name, t.variant);
    if (k.variant == 2) {
        CHECK(t.vcidFrames[5] > 20 && t.lines == 0 && t.blocksBad < 3, "%s: %llu frames on VC 5, %llu lines, %llu bad", k.name, (unsigned long long)t.vcidFrames[5], (unsigned long long)t.lines, (unsigned long long)t.blocksBad);
        return;
    }
    if (k.variant == 1) CHECK(t.rotation == 4 || t.rotation == 5 || t.rotation == 6 || t.rotation == 7, "%s: rotation %d (G2 is inverted, so a mirrored one)", k.name, t.rotation);
    CHECK(t.image && (int)t.lines >= k.minLines, "%s: %llu lines", k.name, (unsigned long long)t.lines);
    if (!t.image) return;
    int exact = 0, seen = 0, badPx = 0;
    for (const auto& l : t.image->line) {
        if (!l) continue;
        seen++;
        const int n = (int)std::lround(((double)l->ms - 36000000.0) / hrpt::kLineMs);
        bool ok = (int)l->px.size() == kHrptChannels * kHrptWidth;
        for (int c = 0; ok && c < kHrptChannels; c++)
            for (int x = 0; x < kHrptWidth; x++)
                if (l->px[(size_t)c * kHrptWidth + (size_t)x] != (uint8_t)(hrptTestPixel(c, x, n) >> 2)) { ok = false; badPx++; break; }
        exact += ok;
    }
    CHECK(seen >= k.minLines && exact == seen, "%s: %d of %d lines exact", k.name, exact, seen);
}

int main() {
    if (const char* e = getenv("HRPT_SWEEP")) {   // HRPT_SWEEP="6,4,3": MetOp at these Es/N0 for a measurement (the checks print, nothing more)
        for (const char* p = e; *p;) {
            const double snr = atof(p);
            char name[64]; snprintf(name, sizeof name, "MetOp, 8 Msps, Es/N0 %.1f dB, +30 kHz", snr);
            run({name, 1, 8e6, snr, 30e3, 10, 0, 0, getenv("HRPT_SECS") ? atof(getenv("HRPT_SECS")) : 1.5, false, 0});
            while (*p && *p != ',') p++; if (*p) p++;
        }
        return 0;
    }
    // MetOp AHRPT
    run({"MetOp, 8 Msps, Es/N0 14 dB, +30 kHz, 40 ppm", 1, 8e6, 14, 30e3, 40, 0, 0, 0.7, false, 5});
    run({"MetOp, 8 Msps, Es/N0 10 dB, -45 kHz, carrier phase 200 deg", 1, 8e6, 10, -45e3, -30, 200, 0, 0.7, false, 5});
    run({"MetOp, 8 Msps, Es/N0 12 dB, +10 kHz and a Doppler ramp of -400 Hz/s", 1, 8e6, 12, 10e3, 0, 100, -400, 0.8, false, 5});
    run({"MetOp, 8 Msps, Es/N0 2.5 dB, +30 kHz (the threshold; ~4 dB before 2026-10)", 1, 8e6, 2.5, 30e3, 10, 0, 0, 1.5, false, 5});
    run({"MetOp, 5 Msps, Es/N0 12 dB, +20 kHz", 1, 5e6, 12, 20e3, 0, 300, 0, 0.7, false, 5});
    run({"MetOp, 10 Msps, on the dial (the app's test signal), no noise", 1, 10e6, 99, 0, 0, 0, 0, 0.6, false, 5});
    // FY-3 AHRPT: frames only
    run({"FY-3, 8 Msps, Es/N0 12 dB, -20 kHz", 2, 8e6, 12, -20e3, 20, 90, 0, 0.5, false, 0});
    // NOAA HRPT
    run({"NOAA, 8 Msps, Es/N0 14 dB, +25 kHz", 3, 8e6, 14, 25e3, 20, 0, 0, 1.2, false, 4});
    run({"NOAA, 8 Msps, Es/N0 14 dB, -40 kHz, opposite polarity", 3, 8e6, 14, -40e3, 0, 140, 0, 1.2, true, 4});
    // automatic starts on MetOp (a NOAA signal needs the supervisor to step through the variants, minutes of signal: not run here)
    {
        HrptGenConfig g;
        g.rate = 8e6; g.variant = 1; g.snrDb = 14; g.cfoHz = -15e3; g.lines = 400;
        HrptGenerator gen(g);
        HrptReceiver rx;
        rx.configure(8e6);
        std::vector<cf32> buf(65536);
        HrptTelemetry t;
        uint64_t seq = 0;
        for (double done = 0; done < 0.6 * 8e6; done += (double)buf.size()) {
            gen.generate(buf.data(), buf.size());
            rx.feed(buf.data(), buf.size());
            if (rx.telemetry(t, seq)) seq = t.seq;
        }
        printf("  automatic on a MetOp signal: %s\n", hrptSummary(t).c_str());
        CHECK(t.state == 3 && t.variant == 1 && t.varSetting == 0, "automatic: state %d variant %d", t.state, t.variant);
    }
    // noise alone gives nothing
    for (int v = 1; v <= 3; v++) {
        HrptReceiver rx;
        rx.configure(8e6);
        rx.setVariant(v);
        std::mt19937 rng(1);
        dect2::normal_distribution<float> n(0.f, 0.2f);
        std::vector<cf32> buf(65536);
        HrptTelemetry t;
        uint64_t seq = 0;
        for (int k = 0; k < 60; k++) {
            for (auto& x : buf) x = cf32{n(rng), n(rng)};
            rx.feed(buf.data(), buf.size());
            if (rx.telemetry(t, seq)) seq = t.seq;
        }
        CHECK(t.blocksOk == 0 && t.lines == 0 && t.state < 2, "noise, variant %d: state %d, %llu frames, %llu lines", v, t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.lines);
    }
    printf(fails ? "hrpt rx: %d FAILED\n" : "hrpt rx: all passed\n", fails);
    return fails ? 1 : 0;
}
