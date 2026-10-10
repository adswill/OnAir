// Airband receiver on the test generator with real-world faults (tests/impair.h): squelch timing per channel, no false openings on noise
// or on a strong neighbour 8.33 kHz away, audio SNR, tuning error of +-7 kHz, sample clock, 8-bit clipping, overmodulation, two stations
// on one channel, very short transmissions, bad samples, channel names.
#include "dect2/airband_gen.h"
#include "dect2/airband_rx.h"
#include "dect2/airband_tel.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kRate = 1e6, kDial = 118.700e6;
static const double kOff = airbandTuning().tuneOffsetHz;

struct Run {
    AirbandTelemetry tel;
    std::vector<std::vector<float>> aud;   // each channel's audio after the squelch
};

static Run run(AirbandGenConfig g, double secs, const std::vector<AirbandChannel>& list,
               const std::function<void(std::vector<cf32>&)>& fault = nullptr, double rxRate = kRate) {
    g.rate = kRate; g.dialOffsetHz = -kOff;
    AirbandGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * kRate));
    for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
    if (fault) fault(x);
    AirbandReceiver rx;
    rx.setSilent(true);
    rx.configure(rxRate);
    rx.setCenterHz(kDial + kOff);
    rx.setChannels(list);
    Run r;
    r.aud.resize(list.size());
    rx.setChannelTap([&](int c, const float* a, size_t n) { r.aud[(size_t)c].insert(r.aud[(size_t)c].end(), a, a + n); });
    for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
    rx.telemetry(r.tel, 0);
    return r;
}

static std::vector<const AirbandActivity*> entries(const Run& r, int chan) {
    std::vector<const AirbandActivity*> v;
    for (const auto& e : r.tel.activity) if (e.chan == chan) v.insert(v.begin(), &e);
    return v;
}

// every burst of channel c that lies inside [t0, t1] is in the log once with the right start and length, and nothing else is
static void checkTiming(const char* what, const Run& r, const AirbandGenChannel& gc, int c, double t0, double t1, double slack = 0.05) {
    std::vector<std::pair<double, double>> want;
    for (double s = gc.phase; s < t1; s += gc.period) if (s >= t0 && s + gc.on <= t1 - 0.6) want.push_back({s, gc.on});
    const auto got = entries(r, c);
    int matched = 0, extra = 0;
    for (const auto* e : got) {
        if (e->startSec < t0 - 0.5) continue;
        bool ok = false;
        for (const auto& w : want) if (std::fabs(e->startSec - w.first) < slack && std::fabs(e->durSec - w.second) < 2 * slack) ok = true;
        if (ok) matched++; else if (e->startSec + e->durSec < t1 - 0.6) { extra++; printf("    %s ch %d: unexpected %.3f s + %.3f s\n", what, c, e->startSec, e->durSec); }
    }
    CHECK(matched == (int)want.size() && extra == 0, "%s channel %d: %d of %zu transmissions matched, %d others", what, c, matched, want.size(), extra);
}

// audio SNR of a 1 kHz tone in the open stretches (the tone transmissions last 1 s or more)
static double toneSnr(const std::vector<float>& a) {
    double best = -99;
    size_t i = 0;
    while (i < a.size()) {
        while (i < a.size() && a[i] == 0) i++;
        size_t j = i;
        while (j < a.size() && a[j] != 0) j++;
        if (j - i > 7200) {   // the first 0.6 s of a 1 s tone burst, after its start (the squelch hang follows the burst)
            const size_t s = i + 1200, e = i + 4800;
            double c = 0, sn = 0, p = 0;
            for (size_t k = s; k < e; k++) { const double w = 2 * M_PI * 1000 * k / 8000.0; c += a[k] * std::cos(w); sn += a[k] * std::sin(w); p += a[k] * a[k]; }
            const double n = (double)(e - s), ps = 2 * (c * c + sn * sn) / (n * n);
            const double pn = p / n - ps;
            best = std::max(best, 10 * std::log10(ps / std::max(pn, 1e-12)));
        }
        i = j;
    }
    return best;
}

int main() {
    // ---- names
    {
        double f; bool n;
        CHECK(airbandParse("118.005", f, n) && n && std::fabs(f - 118.000e6) < 1, "118.005 -> %.1f %d", f, n);
        CHECK(airbandParse("118.010", f, n) && n && std::fabs(f - 118.008333e6) < 1, "118.010 -> %.1f", f);
        CHECK(airbandParse("132.015", f, n) && n && std::fabs(f - 132.0166667e6) < 1, "132.015 -> %.1f", f);
        CHECK(airbandParse("118.025", f, n) && !n && std::fabs(f - 118.025e6) < 1, "118.025");
        CHECK(airbandParse("121.5", f, n) && !n && std::fabs(f - 121.5e6) < 1, "121.5");
        CHECK(airbandParse("118.0083", f, n) && n && std::fabs(f - 118.008333e6) < 1, "118.0083");
        CHECK(!airbandParse("118.020", f, n) && !airbandParse("118.012", f, n) && !airbandParse("117.9", f, n) && !airbandParse("137.000", f, n) && !airbandParse("abc", f, n), "invalid names accepted");
        CHECK(airbandParse("136.990", f, n) && n, "136.990");
        CHECK(airbandName(118.008333e6, true) == "118.010" && airbandName(118.0e6, true) == "118.005" && airbandName(118.025e6, false) == "118.025" && airbandName(136.991667e6, true) == "136.990", "names");
        for (int k = 0; k < 300; k++) {   // every 8.33 channel name round-trips
            const double fr = 118e6 + k * kAirband833;
            double g; bool m;
            if (!airbandParse(airbandName(fr, true), g, m) || !m || std::fabs(g - fr) > 1) { CHECK(false, "round trip %.1f", fr); break; }
        }
    }
    const auto layout = airbandTestLayout();
    const auto list = airbandChannelsFor(layout, kDial);
    // ---- squelch timing on every channel
    {
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25;
        const Run r = run(g, 20, list);
        for (int c = 0; c < (int)layout.size(); c++) if (!layout[(size_t)c].continuous) checkTiming("clean", r, layout[(size_t)c], c, 0.3, 20);
        CHECK(r.tel.channels.size() == 5 && r.tel.channels[3].open, "ATIS open");
        CHECK(std::fabs(r.tel.cfoHz) < 150, "tuning error %.0f Hz with none", r.tel.cfoHz);
        if (getenv("AIRDUMP")) { FILE* f = fopen(getenv("AIRDUMP"), "wb"); fwrite(r.aud[4].data(), 4, r.aud[4].size(), f); fclose(f); }
        const double s = toneSnr(r.aud[4]);
        printf("  clean: %zu log entries, tone SNR %.1f dB, tuning %.0f Hz\n", r.tel.activity.size(), s, r.tel.cfoHz);
        CHECK(s > 20, "tone SNR %.1f dB at 25 dB carrier to noise", s);
    }
    // ---- weak signals: audio SNR against carrier to noise
    for (double snr : {15.0, 8.0}) {
        AirbandGenConfig g; g.chans = {layout[4]}; g.snrDb = snr;
        const Run r = run(g, 12, {list[4]});
        const double s = toneSnr(r.aud[0]);
        printf("  carrier to noise %.0f dB: tone SNR %.1f dB, %zu transmissions\n", snr, s, r.tel.activity.size());
        CHECK(s > snr - 6, "tone SNR %.1f dB at %.0f dB", s, snr);
        checkTiming("weak", r, layout[4], 0, 0.3, 12, 0.06);
    }
    // ---- noise only: never opens
    {
        AirbandGenConfig g; g.snrDb = 20;
        AirbandGenChannel quiet = layout[0]; quiet.levelDb = -200; g.chans = {quiet};
        const Run r = run(g, 30, list);
        CHECK(r.tel.activity.empty() && r.tel.state == 1, "noise: %zu openings", r.tel.activity.size());
    }
    // ---- a neighbour 30 dB stronger, 8.33 kHz away, not listed; also with the radio 1.5 kHz off
    for (double cfo : {0.0, -1500.0, 2000.0}) {
        AirbandGenChannel nbr; nbr.offsetHz = kAirband833; nbr.levelDb = 30; nbr.period = 3; nbr.on = 1.5; nbr.phase = 0.2;
        AirbandGenConfig g; g.chans = {nbr}; g.snrDb = 15; g.cfoHz = cfo;
        AirbandChannel me; me.freqHz = kDial; me.is833 = true;
        const Run r = run(g, 15, {me});
        CHECK(r.tel.activity.empty(), "neighbour (radio %+.0f Hz): %zu false openings, tuning %.0f", cfo, r.tel.activity.size(), r.tel.cfoHz);
        // and the wanted channel still opens under the neighbour
        AirbandGenChannel want; want.is833 = true; want.audio = 1; want.period = 4; want.on = 1.2; want.phase = 1.1;
        g.chans = {nbr, want};
        const Run r2 = run(g, 15, {me});
        checkTiming("under neighbour", r2, want, 0, 4, 15, 0.06);
    }
    // ---- tuning error +-7 kHz (50 ppm at 137 MHz): found from the channels together, never the neighbour
    for (double cfo : {7000.0, -7000.0, 3000.0}) {
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25; g.cfoHz = cfo;
        const Run r = run(g, 24, list);
        printf("  radio %+.0f Hz: measured %+.0f Hz\n", cfo, r.tel.cfoHz);
        CHECK(std::fabs(r.tel.cfoHz - cfo) < 300 && r.tel.tuneKnown, "radio %+.0f Hz: measured %+.0f", cfo, r.tel.cfoHz);
        for (int c = 0; c < (int)layout.size(); c++) if (!layout[(size_t)c].continuous) checkTiming("tuning", r, layout[(size_t)c], c, 10, 24);
    }
    // ---- sample clock 40 ppm fast, and an unusual rate (the receiver told 1 Msps, the stream has 40 ppm more)
    {
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25;
        const Run r = run(g, 12, list, [](std::vector<cf32>& x) { x = impair::clock(x, 40); });
        for (int c = 0; c < (int)layout.size(); c++) if (!layout[(size_t)c].continuous) checkTiming("clock", r, layout[(size_t)c], c, 0.3, 12, 0.06);
    }
    // ---- 8-bit radio driven into clipping
    {
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25;
        const Run r = run(g, 12, list, [](std::vector<cf32>& x) { impair::clip8(x, 5.0); });
        for (int c = 0; c < (int)layout.size(); c++) if (!layout[(size_t)c].continuous) checkTiming("clip8", r, layout[(size_t)c], c, 0.3, 12, 0.06);
    }
    // ---- overmodulation: the envelope clipped at zero
    {
        AirbandGenChannel o = layout[4]; o.mod = 1.4;
        AirbandGenConfig g; g.chans = {o}; g.snrDb = 25;
        const Run r = run(g, 12, {list[4]});
        checkTiming("overmodulation", r, o, 0, 0.3, 12);
        const double s = toneSnr(r.aud[0]);
        printf("  overmodulated 140%%: tone %.1f dB over distortion and noise\n", s);
        CHECK(s > 6, "overmodulation: tone %.1f dB", s);
    }
    // ---- two stations on one channel (400 Hz apart): opens, says heterodyne
    {
        AirbandGenChannel a = layout[0], b = layout[0];
        a.continuous = false; a.period = 5; a.on = 2; a.phase = 1; a.cfoHz = 0;
        b.levelDb = -6; b.cfoHz = 400; b.period = 5; b.on = 2; b.phase = 1; b.audio = 2;
        AirbandGenConfig g; g.chans = {a, b}; g.snrDb = 25;
        const Run r = run(g, 12, {list[0]});
        const auto e = entries(r, 0);
        bool het = false;
        for (const auto* x : e) het |= x->heterodyne;
        CHECK(e.size() >= 2 && het, "two stations: %zu openings, heterodyne %d", e.size(), het);
    }
    // ---- very short transmissions (150 ms, 80 ms) and one that starts before the recording
    {
        AirbandGenChannel s = layout[1];
        s.bursts = {{-0.5, 1.0}, {2.0, 0.15}, {4.0, 0.08}, {6.0, 0.15}};
        AirbandGenConfig g; g.chans = {s}; g.snrDb = 25;
        const Run r = run(g, 8, {list[1]});
        const auto e = entries(r, 0);
        int shortOk = 0;
        for (const auto* x : e) if (x->startSec > 1 && std::fabs(x->durSec - (x->startSec < 3.5 || x->startSec > 5.5 ? 0.15 : 0.08)) < 0.05) shortOk++;
        CHECK(shortOk == 3 && e.size() == 4, "short transmissions: %d of 3 right, %zu entries", shortOk, e.size());
    }
    // ---- bad samples: NaN and infinity in the middle of a transmission
    {
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25;
        const Run r = run(g, 10, list, [](std::vector<cf32>& x) { for (size_t i = 3000000; i < 3000500; i++) x[i] = cf32(NAN, INFINITY); });
        CHECK(r.tel.activity.size() >= 6 && std::isfinite(r.tel.cfoHz), "NaN input: %zu entries", r.tel.activity.size());
        bool finite = true;
        for (const auto& a : r.aud) for (float v : a) finite &= std::isfinite(v);
        CHECK(finite, "NaN reached the audio");
    }
    // ---- a channel outside the band is reported, not received
    {
        AirbandChannel far; far.freqHz = 125.0e6;
        AirbandGenConfig g; g.chans = layout; g.snrDb = 25;
        const Run r = run(g, 2, {far, list[0]});
        CHECK(!r.tel.channels[0].inBand && r.tel.channels[1].inBand, "in band flags");
    }
    printf(fails ? "airband rx: %d FAILED\n" : "airband rx: all passed\n", fails);
    return fails ? 1 : 0;
}
