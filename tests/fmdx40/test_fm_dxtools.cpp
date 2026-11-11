#include "dect2/fm/fm_dxtools.h"
#include <cstdio>
#include <random>
using namespace dect2::fmdx;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
int main() {
    // co-channel: two PIs alternate on 93.9, with bit-error PIs sprinkled in
    std::mt19937 g(1);
    PiWatch w;
    for (int i = 0; i < 100; i++) {
        const int pi = (i % 3 == 0) ? 0x5201 : 0x5202;
        w.add(93.9, pi, pi == 0x5201 ? 12 : 22);
        if (i % 11 == 0) w.add(93.9, 0x5200 + (int)(g() % 200) + 16, 5);   // random bit-error PIs, each seen about once
    }
    for (int i = 0; i < 50; i++) w.add(101.1, 0x4321, 25);   // a clean station
    auto co = w.coChannel();
    check(co.size() == 1 && std::fabs(co[0].mhz - 93.9) < 1e-9, "only the shared channel is reported");
    check(co[0].pis.size() == 2 && co[0].pis[0].pi == 0x5202 && co[0].pis[0].snrDb > co[0].pis[1].snrDb, "stronger PI listed first with its SNR");
    check(co[0].pis[0].share > 0.55 && co[0].pis[1].share > 0.25, "shares");
    // a single burst of a wrong PI (3 reads) is not enough
    PiWatch w2;
    for (int i = 0; i < 60; i++) w2.add(88.1, 0x1111, 20);
    for (int i = 0; i < 3; i++) w2.add(88.1, 0x2222, 20);
    check(w2.coChannel().empty(), "a few bit errors are not co-channel");
    // an unknown PI (0) is ignored
    PiWatch w3;
    for (int i = 0; i < 20; i++) { w3.add(90, 0, 10); w3.add(90, 0x3333, 10); }
    check(w3.coChannel().empty(), "PI 0 ignored");
    // rounding of the frequency key: 93.9 and 93.9004 are the same channel
    PiWatch w4;
    for (int i = 0; i < 10; i++) { w4.add(93.9, 1, 10); w4.add(93.9004, 2, 10); }
    check(w4.coChannel().size() == 1, "same channel within a kHz");
    // adjacent risk
    std::vector<ListedStation> st = {{95.0, 5}, {95.1, 30}, {95.2, 8}, {98.0, 20}, {98.4, 25}};
    auto ar = adjacentRisk(st);
    check(ar.size() == 2, "two stations at risk (95.0 and 95.2 under 95.1; 95.0 also under none else)");
    bool a0 = false, a2 = false;
    for (auto& r : ar) { if (r.index == 0 && std::fabs(r.neighbourKhz - 100) < 1e-6 && std::fabs(r.overDb - 25) < 1e-9) a0 = true; if (r.index == 2 && std::fabs(r.neighbourKhz + 100) < 1e-6) a2 = true; }
    check(a0 && a2, "neighbour offset and excess level");
    check(adjacentRisk({{98.0, 20}, {98.4, 25}}).empty(), "400 kHz away is no risk");
    check(adjacentRisk({{95.0, 20}, {95.1, 30}}).empty(), "10 dB over is below the default 20 dB");
    check(adjacentRisk({{95.0, 20}, {95.1, 30}}, 8).size() == 1, "threshold adjustable");
    // best heading: a lobe at 130 degrees, 60 degrees wide, noisy readings every 20 degrees
    std::normal_distribution<double> nd(0, 0.8);
    for (int trial = 0; trial < 20; trial++) {
        std::vector<HeadingSample> hs;
        for (int d = 0; d < 360; d += 20) {
            double off = std::fabs(std::fmod(d - 130 + 540, 360) - 180);
            hs.push_back({(double)d, -70 + 18 * std::exp(-off * off / (2 * 30.0 * 30.0)) + nd(g)});
        }
        auto b = bestHeading(hs);
        check(b.ok && std::fabs(b.deg - 130) <= 20, "best heading within one step of the true lobe, across noise");
        check(b.gainDb > 3 && b.widthDeg >= 20 && b.widthDeg < 140, "gain and lobe width plausible");
    }
    // wrap-around: lobe at 350
    std::vector<HeadingSample> wr;
    for (int d = 0; d < 360; d += 30) { double off = std::fabs(std::fmod(d - 350 + 540, 360) - 180); wr.push_back({(double)d, -80 + 15 * std::exp(-off * off / 800.0)}); }
    auto bw = bestHeading(wr);
    check(bw.ok && (std::fabs(bw.deg - 330) < 1 || std::fabs(bw.deg - 0) < 1), "lobe across north");
    // negative and >360 headings accepted; duplicates averaged
    std::vector<HeadingSample> dup = {{-90, -60}, {0, -50}, {0, -50}, {90, -62}, {180, -64}, {450, -61}};
    auto bd = bestHeading(dup);
    check(bd.ok && std::fabs(bd.deg - 0) < 1, "normalised headings and duplicates");
    check(!bestHeading({{0, -50}, {10, -49}, {20, -48}, {30, -47}}).ok, "headings that do not go round are refused");
    check(!bestHeading({{0, -50}, {90, -49}}).ok, "too few headings refused");
    std::printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
