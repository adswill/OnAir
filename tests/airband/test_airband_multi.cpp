// Airband, wide capture with many channels (an airport's frequencies in one capture): eight AM channels on the 25 kHz and the 8.33 kHz
// raster (two neighbouring pairs), spread over +-750 kHz around the dial, voice-like modulation, noise, the radio's tuning error up to
// +-10 kHz. Every channel must open for its own transmissions only (start and length), nothing may leak to a channel that is quiet, and
// each channel's audio must be present while it is keyed. Per-channel squelch offsets and the channel grouping are checked too.
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/airband/airband_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static const double kDial = 118.700e6;
static const double kOff = airbandTuning().tuneOffsetHz;

struct Setup { std::vector<AirbandGenChannel> gen; std::vector<AirbandChannel> list; };

static Setup layout8(double secs) {
    struct D { double off; bool n833; const char* name; };
    // offsets from the dial in 8.333 kHz steps (the dial is on the raster)
    const D d[8] = {{-200000, false, "A"}, {-175000, false, "B"}, {0, false, "C"}, {8333.3333, true, "D"}, {16666.667, true, "E"},
                    {300000, false, "F"}, {-600000, false, "G"}, {750000, false, "H"}};
    Setup s;
    for (int i = 0; i < 8; i++) {
        AirbandGenChannel g;
        g.offsetHz = d[i].off; g.is833 = d[i].n833; g.label = d[i].name;
        g.levelDb = -4.0 * (i % 4);      // 0 .. -12 dB
        g.cfoHz = (i % 3 - 1) * 120.0;   // transmitters a little off
        g.audio = 0; g.mod = 0.8;
        g.period = 4.0; g.on = 1.4; g.phase = 0.3 + 0.55 * i;   // staggered, overlapping between channels
        // neighbours 25 kHz apart (A, B) and 8.33 kHz apart (C, D, E) never transmit at the same time: a plain squelch cannot tell them from splatter
        if (i == 1) g.phase += 2.0;
        if (i == 3) g.phase += 1.0;
        if (i == 4) g.phase += 2.0;
        s.gen.push_back(g);
        AirbandChannel c; c.freqHz = kDial + d[i].off; c.is833 = d[i].n833; c.label = d[i].name;
        s.list.push_back(c);
    }
    (void)secs;
    return s;
}

struct Run { AirbandTelemetry tel; std::vector<std::vector<float>> aud; };

static Run run(const Setup& st, double rate, double cfo, double snr, double secs, const std::vector<AirbandChannel>* listOverride = nullptr,
               float squelch = 6.f) {
    AirbandGenConfig g; g.rate = rate; g.dialOffsetHz = -kOff; g.chans = st.gen; g.snrDb = snr; g.cfoHz = cfo;
    AirbandGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * rate));
    for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
    AirbandReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    rx.setCenterHz(kDial + kOff);
    const auto& list = listOverride ? *listOverride : st.list;
    rx.setChannels(list);
    rx.setSquelchDb(squelch);
    Run r;
    r.aud.resize(list.size());
    rx.setChannelTap([&](int c, const float* a, size_t n) { r.aud[(size_t)c].insert(r.aud[(size_t)c].end(), a, a + n); });
    for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
    rx.telemetry(r.tel, 0);
    return r;
}

// the keyed windows of a channel that lie fully inside [t0, t1 - 0.7]
static std::vector<std::pair<double, double>> windows(const AirbandGenChannel& g, double t0, double t1) {
    std::vector<std::pair<double, double>> w;
    for (double s = g.phase; s < t1; s += g.period) if (s >= t0 && s + g.on <= t1 - 0.7) w.push_back({s, g.on});
    return w;
}

static void check(const char* what, const Setup& st, const Run& r, double secs, double t0, int nch = 8) {
    int bad = 0;
    for (int c = 0; c < nch; c++) {
        const auto want = windows(st.gen[(size_t)c], t0, secs);
        int matched = 0, extra = 0;
        for (const auto& e : r.tel.activity) {
            if (e.chan != c || e.startSec < t0) continue;
            bool ok = false;
            for (const auto& w : want) if (std::fabs(e.startSec - w.first) < 0.08 && std::fabs(e.durSec - w.second) < 0.15) ok = true;
            if (ok) matched++; else if (e.startSec + e.durSec < secs - 0.7) extra++;
        }
        // audio: energy inside the keyed windows, none well outside them
        double inside = 0, outside = 0;
        const auto& a = r.aud[(size_t)c];
        std::vector<std::pair<double, double>> all;
        for (double s = st.gen[(size_t)c].phase; s < secs; s += st.gen[(size_t)c].period) all.push_back({s, st.gen[(size_t)c].on});
        for (size_t i = 0; i < a.size(); i++) {
            const double t = (double)i / 8000.0;
            if (t < t0) continue;
            bool in = false, nearBy = false;
            for (const auto& w : all) { if (t > w.first + 0.1 && t < w.first + w.second - 0.05) in = true; if (t > w.first - 0.3 && t < w.first + w.second + 0.9) nearBy = true; }
            if (in) inside += (double)a[i] * a[i]; else if (!nearBy) outside += (double)a[i] * a[i];
        }
        const bool ok = matched == (int)want.size() && extra == 0 && inside > 1e-3 * (double)want.size() && outside < 1e-6 + 0.001 * inside;
        if (!ok) {
            bad++;
            printf("    %s channel %d (%s): %d of %zu transmissions, %d others, audio inside %.3g outside %.3g\n", what, c, st.gen[(size_t)c].label.c_str(), matched, want.size(), extra, inside, outside);
        }
        CHECK(ok, "%s: channel %d", what, c);
    }
    printf("  %s: %d of %d channels right, tuning %+.0f Hz\n", what, nch - bad, nch, r.tel.cfoHz);
}

int main() {
    const double secs = 12;
    const Setup st = layout8(secs);
    // by default: 8 s where the check starts at 2 s (every channel keys once in 2 .. 7.3 s), one radio error of each sign and size, the harder
    // band watch, a shorter noise run; the long run (ONAIR_TEST_LONG=1) does every case for 12 s. Runs that start checking at 6 s keep 12 s.
    const bool L = testLong();
    const double secsQ = L ? secs : 8;
    // ---- 2.4 Msps (RTL-SDR) and 2 Msps, radio right (the 22 dB run below covers it by default)
    if (L) check("2.4 Msps, no error", st, run(st, 2.4e6, 0, 25, secs), secs, 2.0);
    // ---- the radio's tuning error: +-5 kHz (37 ppm) and +-7 kHz (51 ppm) at 137 MHz, on 25 kHz channels only and on all eight
    for (double cfo : {-5000.0, 7000.0, -7000.0}) {
        if (!L && cfo == -7000.0) continue;
        const double rate = cfo == -5000.0 ? 2.0e6 : 2.4e6;   // 2.0 Msps with the radio 5 kHz low, 2.4 Msps with it +-7 kHz
        char b[64]; snprintf(b, sizeof b, "%.1f Msps, radio %+.0f Hz", rate / 1e6, cfo);
        const Run r = run(st, rate, cfo, 25, secs);
        CHECK(std::fabs(r.tel.cfoHz - cfo) < 400 && r.tel.tuneKnown, "%s: measured %+.0f", b, r.tel.cfoHz);
        check(b, st, r, secs, 6.0);
    }
    // ---- +-10 kHz (73 ppm): beyond half an 8.33 kHz step, so with 8.33 channels in the list it is ambiguous; a list of 25 kHz channels must follow it
    {
        Setup s25;
        for (int i : {0, 1, 2, 5, 6, 7}) { s25.gen.push_back(st.gen[(size_t)i]); s25.list.push_back(st.list[(size_t)i]); }
        for (double cfo : {10000.0, -10000.0}) {
            if (!L && cfo < 0) continue;
            char b[64]; snprintf(b, sizeof b, "25 kHz only, radio %+.0f Hz", cfo);
            const Run r = run(s25, 2.4e6, cfo, 25, secs);
            CHECK(std::fabs(r.tel.cfoHz - cfo) < 500 && r.tel.tuneKnown, "%s: measured %+.0f", b, r.tel.cfoHz);
            check(b, s25, r, secs, 6.0, 6);
        }
    }
    // ---- weak signals: 22 dB carrier to noise (the weakest channel is 12 dB below)
    check("2.4 Msps, 22 dB", st, run(st, 2.4e6, 0, 22, secsQ), secsQ, 2.0);
    // ---- per-channel squelch: raise one channel's threshold above its carrier: it stays closed, the others still open
    {
        std::vector<AirbandChannel> l = st.list;
        l[2].squelchDb = 40;   // channel C needs 40 dB
        const Run r = run(st, 2.4e6, 0, 25, secs, &l);
        int c2 = 0, others = 0;
        for (const auto& e : r.tel.activity) { if (e.chan == 2) c2++; else others++; }
        CHECK(c2 == 0 && others >= 12, "per-channel squelch: channel C %d openings, others %d", c2, others);
        std::vector<AirbandChannel> l2 = st.list;
        l2[2].squelchDb = 2;
        const Run r2 = run(st, 2.4e6, 0, 25, secs, &l2);
        int c3 = 0;
        for (const auto& e : r2.tel.activity) if (e.chan == 2) c3++;
        CHECK(c3 >= 3, "per-channel squelch low: channel C %d openings", c3);
    }
    // ---- the band watch: channels that are on the air but not in the list are found, named right, and listed ones are not repeated
    for (double cfo : {0.0, 6000.0}) {
        if (!L && cfo == 0.0) continue;
        std::vector<AirbandChannel> l = {st.list[0], st.list[1], st.list[2]};
        const Run r = run(st, 2.4e6, cfo, 25, secs, &l);
        int found = 0, wrong = 0;
        for (int c = 3; c < 8; c++) {
            const std::string nm = airbandName(st.list[(size_t)c].freqHz, st.list[(size_t)c].is833);
            bool hit = false;
            for (const auto& x : r.tel.seen) if (x.name == nm && std::fabs(x.freqHz - st.list[(size_t)c].freqHz) < 5 && x.is833 == st.list[(size_t)c].is833) hit = true;
            found += hit;
            CHECK(hit, "band watch (radio %+.0f Hz): channel %s not seen", cfo, nm.c_str());
        }
        for (const auto& x : r.tel.seen) {
            bool known = false;
            for (int c = 0; c < 8; c++) if (std::fabs(x.freqHz - st.list[(size_t)c].freqHz) < 5) known = true;
            if (!known || (x.freqHz == st.list[0].freqHz || x.freqHz == st.list[1].freqHz || x.freqHz == st.list[2].freqHz)) { wrong++; printf("    watch: unexpected %s %.1f\n", x.name.c_str(), x.freqHz); }
        }
        CHECK(wrong == 0, "band watch (radio %+.0f Hz): %d unexpected entries of %zu", cfo, wrong, r.tel.seen.size());
        printf("  band watch, radio %+.0f Hz: %d of 5 unlisted channels seen, %zu entries\n", cfo, found, r.tel.seen.size());
    }
    // ---- noise only: the band watch finds nothing
    {
        Setup q = st;
        for (auto& g : q.gen) g.levelDb = -200;
        std::vector<AirbandChannel> none;
        const Run r = run(q, 2.4e6, 0, 20, testLongPick(6.0, 12.0), &none);
        CHECK(r.tel.seen.empty() && r.tel.activity.empty(), "noise: %zu carriers seen", r.tel.seen.size());
    }
    // ---- scan with hold: the scan stays on the channel it stopped on; without hold it goes on to the next one that opens
    for (bool hold : {false, true}) {
        AirbandGenChannel a = st.gen[0], b = st.gen[2];
        a.bursts = {{1.0, 1.4}}; b.bursts = {{5.0, 1.4}};
        AirbandGenConfig g; g.rate = 1e6; g.dialOffsetHz = -kOff; g.chans = {a, b}; g.snrDb = 25;
        AirbandGenerator gen(g);
        std::vector<cf32> x((size_t)(6.2 * 1e6));
        for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
        AirbandReceiver rx;
        rx.setSilent(true); rx.configure(1e6); rx.setCenterHz(kDial + kOff);
        rx.setChannels({st.list[0], st.list[2]});
        rx.setScan(true); rx.setScanHold(hold);
        for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
        AirbandTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.channels.size() == 2 && t.channels[1].open && t.channels[1].playing == !hold, "scan, hold %d: B open %d playing %d", hold, t.channels.size() == 2 ? t.channels[1].open : -1, t.channels.size() == 2 ? t.channels[1].playing : -1);
        CHECK(t.scanChan == (hold ? 0 : 1), "scan, hold %d: stopped on channel %d", hold, t.scanChan);
    }
    // ---- grouping of a frequency list into captures
    {
        std::vector<double> f = {118.5e6, 118.525e6, 121.9e6, 121.7e6, 123.45e6, 126.7e6, 127.0e6, 135.0e6};
        const auto g = airbandPlanGroups(f, 2.4e6 * 0.8);
        CHECK(g.size() == 4, "groups: %zu", g.size());
        size_t total = 0;
        for (const auto& x : g) { total += x.members.size(); for (size_t m : x.members) CHECK(std::fabs(f[m] - x.centerHz) <= 0.5 * 2.4e6 * 0.8 + 1, "member %zu outside its group", m); }
        CHECK(total == f.size(), "every frequency in a group once: %zu", total);
        const auto one = airbandPlanGroups({118.5e6, 118.525e6, 119.0e6}, 2e6);
        CHECK(one.size() == 1 && one[0].members.size() == 3, "one group");
        CHECK(airbandPlanGroups({}, 2e6).empty(), "empty");
        const auto huge = airbandPlanGroups({118.0e6, 136.0e6}, 2e6);
        CHECK(huge.size() == 2, "two far apart: %zu", huge.size());
    }
    return testSummary("airband multi");
}
