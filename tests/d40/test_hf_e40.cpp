#include "dect2/d40/hf_e40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::mt19937 rng(3); std::normal_distribution<double> nd(0, 2);
    // synthetic site: 14 MHz good by day (07-17 UTC), 7 MHz good by night; learned from noisy readings over 10 days, with hour 3 never read on 14.1 MHz
    BandClock bc; const std::vector<double> fr = {7.0e6, 14.1e6, 21.15e6};
    auto truth = [](double f, int h) { const bool day = h >= 7 && h < 18; return f == 7.0e6 ? (day ? 2.0 : 18.0) : f == 14.1e6 ? (day ? 20.0 : 5.0) : (day ? 12.0 : -8.0); };
    for (int d = 0; d < 10; d++) for (int h = 0; h < 24; h++) for (double f : fr) if (!(h == 3 && f == 14.1e6)) bc.add(f, h, truth(f, h) + nd(rng));
    int right = 0; for (int h = 0; h < 24; h++) { auto p = bc.best(fr, h); if (p.ok && p.freqHz == (h >= 7 && h < 18 ? 14.1e6 : 7.0e6)) right++; }
    CHECK(right == 24);
    bool nb = false; const double e = bc.expected(14.1e6, 3, &nb); CHECK(nb && std::fabs(e - 5.0) < 3);
    CHECK(std::isnan(bc.expected(10.0e6, 5)));
    CHECK(!bc.best(fr, 12, 40).ok);                       // nothing reaches 40 dB: says so instead of guessing
    const int ch = bc.hoursToChange(fr, 5); CHECK(ch == 2);   // 7 MHz until 06, 14 MHz from 07
    BandClock empty; CHECK(!empty.best(fr, 0).ok && empty.hoursToChange(fr, 0) == 0);
    // fading: flat carrier, 4 fades/min of 12 dB, flutter of 300 /min
    std::normal_distribution<float> n1(0, 0.3f); const double fs = 10;
    std::vector<float> flat, qsb, flut;
    for (int i = 0; i < 600; i++) { const double t = i / fs; flat.push_back(n1(rng)); qsb.push_back(6 * (float)std::sin(2 * M_PI * (4.0 / 60) * t) + n1(rng)); }
    for (int i = 0; i < 6000; i++) flut.push_back(6 * (float)std::sin(2 * M_PI * 20.0 * i / 100.0) + n1(rng));
    auto a = fadeStats(flat, fs), b = fadeStats(qsb, fs), c = fadeStats(flut, 100);
    CHECK(a.ok && a.verdict == "flat"); CHECK(b.verdict == "QSB" && std::fabs(b.ratePerMin - 4) < 1 && std::fabs(b.depthDb - 11.5) < 3);
    CHECK(c.verdict == "flutter"); CHECK(!fadeStats({1, 2, 3}, fs).ok);
    if (!fails) std::printf("hf_e40 ok\n"); return fails ? 1 : 0;
}
