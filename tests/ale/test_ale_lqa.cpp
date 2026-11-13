// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ALE link-quality memory (ale_lqa.h): rank the channels of a station by hour of the day from soundings, against a synthetic ionosphere, and compare with
// "take the strongest of what was heard lately".
#include "dect2/ale/ale_lqa.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kHz[5] = {3596e3, 5372e3, 7102e3, 10145e3, 14109e3};
static const double kPeakHour[5] = {2, 5, 10, 13, 15};     // the hour each channel is at its best (night to afternoon)
static const double kPeak[5] = {18, 20, 22, 19, 21};

static double truth(int ch, double hour, double dayOffset) {   // dB in 3 kHz
    const double x = 0.5 * (1 + std::cos(2 * M_PI * (hour - kPeakHour[ch]) / 24.0));   // 1 at the peak hour, 0 twelve hours away
    return -12 + (kPeak[ch] + 12) * std::pow(x, 1.6) + dayOffset;
}
static int bestAt(double hour) {
    int b = 0;
    for (int c = 1; c < 5; c++) if (truth(c, hour, 0) > truth(b, hour, 0)) b = c;
    return b;
}

struct Result { double lqa = 0, lastHeard = 0, random = 0.2; };

// days of soundings, then asks for the best channel at every hour of the next day
static Result trial(int days, double sigma, double loss, int seed, bool addCalls = false) {
    AleLqa store;
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, sigma), dayN(0, 3);
    std::uniform_real_distribution<double> u(0, 1);
    struct Last { double utc, snr; int ch; };
    std::vector<Last> lastObs;
    double t = 1.7e9;
    t = std::floor(t / 86400) * 86400;
    for (int d = 0; d < days; d++) {
        const double off = dayN(rng);
        for (double s = 0; s < 86400; s += 1800) {
            for (int ch = 0; ch < 5; ch++) {
                if (u(rng) < loss) continue;
                const double snr = truth(ch, s / 3600.0, off) + noise(rng);
                if (snr < -12) continue;   // below the noise nothing is heard
                AleLqaObs o; o.utc = t + s + ch * 20; o.station = "KMA"; o.hz = kHz[ch]; o.snrDb = (float)snr; o.ber = (float)std::max(0.0, std::min(0.4, 0.3 - (snr + 5) / 80.0)); o.sounding = true;
                store.add(o);
                lastObs.push_back({o.utc, snr, ch});
                if (addCalls) { o.sounding = false; o.utc += 3; store.add(o); }
            }
        }
        t += 86400;
    }
    Result r;
    int okL = 0, okH = 0, n = 0;
    for (int h = 0; h < 24; h++) {
        const double now = t + h * 3600 + 600;
        const int want = bestAt(h + 10.0 / 60);
        const auto ranks = store.rank("KMA", now);
        if (ranks.empty()) continue;
        int got = -1;
        for (int c = 0; c < 5; c++) if (std::fabs(ranks[0].hz - kHz[c]) < 1) got = c;
        // equal in practice: a channel within 2 dB of the best is as good a choice
        if (got >= 0 && truth(got, h + 10.0 / 60, 0) >= truth(want, h + 10.0 / 60, 0) - 2.0) okL++;
        // baseline: the strongest observation of the last 3 hours of the previous day (what a person looking at a stations list would do)
        int bl = -1; double bs = -1e9;
        for (const auto& o : lastObs) if (o.utc > now - 86400 - 3 * 3600 && o.utc <= now - 86400 && o.snr > bs) { bs = o.snr; bl = o.ch; }
        // "just now" baseline: strongest of the last observations before the end of the last sounding round (the hour 23 values)
        bl = -1; bs = -1e9;
        for (const auto& o : lastObs) if (o.utc > t - 3 * 3600 && o.snr > bs) { bs = o.snr; bl = o.ch; }
        if (bl >= 0 && truth(bl, h + 10.0 / 60, 0) >= truth(want, h + 10.0 / 60, 0) - 2.0) okH++;
        n++;
    }
    r.lqa = n ? (double)okL / n : 0; r.lastHeard = n ? (double)okH / n : 0;
    return r;
}

static void sweeps() {
    printf("best channel by hour of the day (a channel within 2 dB of the best counts as right), 5 channels, mean of 8 seeds:\n");
    printf("  days  sigma  loss | LQA store | strongest-lately | random\n");
    const int days[] = {1, 2, 4, 10, 20};
    double acc20 = 0, accLast20 = 0;
    for (int d : days)
        for (double sg : {2.0, 4.0, 8.0}) {
            double a = 0, b = 0;
            for (int s = 0; s < 8; s++) { const Result r = trial(d, sg, 0.3, 50 + s); a += r.lqa; b += r.lastHeard; }
            a /= 8; b /= 8;
            printf("  %4d  %4.0f  30%% | %8.0f%% | %15.0f%% | 20%%\n", d, sg, a * 100, b * 100);
            if (d == 20 && sg == 4) { acc20 = a; accLast20 = b; }
        }
    CHECK(acc20 >= 0.9, "20 days, 4 dB noise, 30%% lost: %.0f%% right", acc20 * 100);
    CHECK(acc20 > accLast20 + 0.2, "better than the strongest of the last observations (%.0f%% against %.0f%%)", acc20 * 100, accLast20 * 100);
    // heavy loss still works with enough days
    double h = 0;
    for (int s = 0; s < 8; s++) h += trial(20, 6, 0.7, 900 + s).lqa;
    h /= 8;
    printf("  20 days, 6 dB noise, 70%% of the soundings lost: %.0f%% right\n", h * 100);
    CHECK(h >= 0.8, "heavy loss: %.0f%%", h * 100);
}

static void behaviour() {
    AleLqa s;
    const double t0 = 1.7e9 - std::fmod(1.7e9, 86400);
    // the hour profile and the confidence: one lucky strong observation does not beat nine good ones
    for (int d = 0; d < 9; d++) { AleLqaObs o; o.utc = t0 - (d + 1) * 86400 + 12 * 3600; o.station = "AAA"; o.hz = 7102e3; o.snrDb = 12; o.sounding = true; s.add(o); }
    { AleLqaObs o; o.utc = t0 - 86400 + 12 * 3600; o.station = "AAA"; o.hz = 10145e3; o.snrDb = 16; o.sounding = true; s.add(o); }
    auto r = s.rank("AAA", t0 + 12 * 3600 + 300);
    CHECK(r.size() == 2 && std::fabs(r[0].hz - 7102e3) < 1, "nine observations beat one lucky one (%s)", r.empty() ? "none" : std::to_string(r[0].hz).c_str());
    // a channel heard strong a few minutes ago rises
    { AleLqaObs o; o.utc = t0 + 12 * 3600 + 100; o.station = "AAA"; o.hz = 10145e3; o.snrDb = 24; o.sounding = true; s.add(o); }
    r = s.rank("AAA", t0 + 12 * 3600 + 300);
    CHECK(std::fabs(r[0].hz - 10145e3) < 1 && r[0].recent, "just heard strong wins");
    // nothing for another hour of the day gives a weak prior only, below the observed channel
    auto night = s.rank("AAA", t0 + 2 * 3600);
    CHECK(night.size() == 2 && night[0].score < 50, "other-hour data is only a faint prior (%.0f)", night.empty() ? -1 : night[0].score);
    // unknown station
    CHECK(s.rank("ZZZ", t0).empty(), "unknown station");
    // old data fades (two half lives = a quarter weight)
    AleLqa f;
    { AleLqaObs o; o.utc = t0 - 56 * 86400; o.station = "OLD"; o.hz = 5372e3; o.snrDb = 20; f.add(o); }
    { AleLqaObs o; o.utc = t0 - 56 * 86400 + 5; o.station = "OLD"; o.hz = 5372e3; o.snrDb = 20; f.add(o); }
    const auto fo = f.profile("OLD", 5372e3, t0);
    CHECK(fo.size() == 24, "profile size");
    double w = 0; for (const auto& h : fo) w += h.w;
    CHECK(w > 0.05 && w < 0.6, "four half lives later the weight is small: %.2f", w);
    // propagation: an open band is worth more than a closed one for the same data
    AleLqa p;
    for (int d = 0; d < 5; d++) {
        AleLqaObs a; a.utc = t0 - (d + 1) * 86400 + 12 * 3600; a.station = "BBB"; a.hz = 7102e3; a.snrDb = 10; p.add(a);   // 40 m
        a.hz = 14109e3; p.add(a);                                                                                           // 20 m
    }
    std::vector<hfp::BandNow> bands(hfp::kBands);
    for (int b = 0; b < hfp::kBands; b++) bands[(size_t)b].band = b;
    bands[(size_t)hfp::bandOf(14109e3)].state = hfp::Open;
    bands[(size_t)hfp::bandOf(7102e3)].state = hfp::Closed;
    const auto pr = p.rank("BBB", t0 + 12 * 3600 + 30, &bands);
    CHECK(pr.size() == 2 && std::fabs(pr[0].hz - 14109e3) < 1 && pr[0].propState == hfp::Open && pr[1].propState == hfp::Closed, "propagation tips the ranking");
    // save / load: same ranking
    const std::string txt = s.save();
    AleLqa q;
    q.load(txt);
    const auto a1 = s.rank("AAA", t0 + 12 * 3600 + 300), a2 = q.rank("AAA", t0 + 12 * 3600 + 300);
    CHECK(a1.size() == a2.size() && !a1.empty() && std::fabs(a1[0].score - a2[0].score) < 0.1 && std::fabs(a1[0].hz - a2[0].hz) < 1, "save and load keep the ranking");
    // damaged text does not load or crash
    AleLqa bad;
    bad.load("alelqa 1\ngarbage line\nX 1 99 1 1 1 1 1 1 1\nY nan 3 1 1 1 1 1 1 1\n" + txt.substr(0, txt.size() / 2));
    bad.load("not a file");
    CHECK(bad.rank("AAA", t0).size() <= 2, "damaged lines skipped");
    // the table is bounded
    AleLqa::Config cfg; cfg.maxStations = 5; cfg.maxChannels = 3;
    AleLqa b(cfg);
    for (int i = 0; i < 50; i++) { AleLqaObs o; o.utc = t0 + i; o.station = "S" + std::to_string(i); o.hz = 7e6 + i * 1000; o.snrDb = 5; b.add(o); }
    for (int i = 0; i < 20; i++) { AleLqaObs o; o.utc = t0 + 100 + i; o.station = "S49"; o.hz = 7e6 + i * 5000; o.snrDb = 5; b.add(o); }
    CHECK(b.stations().size() <= 5 && b.rank("S49", t0 + 200).size() <= 3, "bounded");
    // garbage observations are refused
    AleLqaObs g; g.utc = t0; g.station = ""; g.hz = 7e6; b.add(g);
    g.station = "X"; g.hz = 0; b.add(g);
    g.hz = 7e6; g.snrDb = NAN; b.add(g);
    CHECK(b.rank("X", t0).empty(), "garbage refused");
}

int main() {
    behaviour();
    sweeps();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
