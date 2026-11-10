// 3.9 smart features, TV (docs/SMART5-tv-3.9.md items 11 - 15). Simulated only: synthetic PID counts, per-carrier SNR, Es/N0
// series, a hand-built cell_list_descriptor and generated test lines, with offsets, noise and filtering; no recording was on hand.
#include "dect2/smart5/cell_tx.h"
#include "dect2/smart5/ofdm_interf.h"
#include "dect2/smart5/pid_offair.h"
#include "dect2/smart5/sat_fade.h"
#include "dect2/smart5/vits.h"
#include <cmath>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2::smart5;
static const double kPi = 3.14159265358979323846;

static void testOffAir() {
    std::mt19937 rng(3);
    std::poisson_distribution<int> vid(2000), aud(170), si(40), nul(500);
    PidOffAir d;
    d.mapPid(1, 0x100, true); d.mapPid(1, 0x101, false);
    d.mapPid(2, 0x200, true); d.mapPid(2, 0x201, false);
    double t = 0;
    auto second = [&](bool s1Video, bool s2Audio, bool cc) {
        d.packets(0x100, s1Video ? vid(rng) : 3); d.packets(0x101, aud(rng));
        d.packets(0x200, vid(rng)); d.packets(0x201, s2Audio ? aud(rng) : 0);
        d.packets(0x000, si(rng)); d.packets(0x1FFF, s1Video ? nul(rng) : nul(rng) + 2000);   // the mux stuffs nulls in its place
        if (cc) for (int i = 0; i < 300; i++) d.packet(0x200, true);
        d.tick(t); t += 1;
    };
    for (int i = 0; i < 60; i++) second(true, true, false);
    CHECK(d.offAir().empty(), "false alarm");
    for (int i = 0; i < 10; i++) second(false, true, false);   // service 1's video encoder stops
    auto off = d.offAir();
    CHECK(off.size() == 1 && off[0].service == 1 && off[0].video && std::fabs(off[0].since - 60) < 1.5, "video off %zu", off.size());
    for (int i = 0; i < 10; i++) second(true, true, false);
    CHECK(d.offAir().empty() && d.log().size() == 1, "recovered");
    for (int i = 0; i < 10; i++) second(true, false, true);    // audio stops, but the mux has continuity errors: not judged
    CHECK(!d.muxFine() && d.offAir().empty(), "judged during bad reception");
    for (int i = 0; i < 10; i++) second(true, false, false);
    off = d.offAir();
    CHECK(off.size() == 1 && off[0].pid == 0x201 && !off[0].video, "audio off");
    CHECK(!d.text(off[0]).empty(), "text");
}

static void testInterferer() {
    std::mt19937 rng(4);
    std::normal_distribution<float> n(0, 1);
    // DVB-T 8k in 8 MHz: 6817 carriers at 1116 Hz; T2 32k: 27841 at 279 Hz; DAB 1536 at 1 kHz; centre offsets swept
    struct Sys { int nc; double df; } sys[] = {{6817, 1e6 * 8 / 7168}, {27841, 8e6 / 28672}, {1536, 1000}};
    for (auto& s : sys)
        for (double off : {0.0, 1.3e3, -27e3}) {
            const double f0 = 650e6 + off - s.nc / 2 * s.df;
            std::vector<float> snr(s.nc);
            for (auto& v : snr) v = 28 + 1.5f * n(rng);
            const int cw = s.nc / 3;
            for (int k = cw - 1; k <= cw + 1; k++) snr[k] = 8;                    // CW spur
            int ltec = -1; const double lteW = 4.5e6;
            if (s.nc * s.df > 6e6) {                                              // an LTE 5 MHz block fits only in TV channels
                const int w = (int)(lteW / s.df); ltec = s.nc * 2 / 3;
                for (int k = ltec - w / 2; k < ltec + w / 2; k++) snr[k] = 15 + n(rng);
            }
            for (int k = s.nc - s.nc / 50; k < s.nc; k++) snr[k] = 12 + n(rng);   // adjacent channel edge
            auto v = locateInterferers(snr, f0, s.df);
            bool gotCw = false, gotLte = ltec < 0, gotAdj = false;
            for (auto& i : v) {
                if (i.kind == InterfKind::CwSpur && std::fabs(i.centreHz - (f0 + cw * s.df)) < 2 * s.df) gotCw = true;
                if (i.kind == InterfKind::LteBlock && std::fabs(i.centreHz - (f0 + ltec * s.df)) < 50e3 && std::fabs(i.widthHz - lteW) < 0.1 * lteW) gotLte = true;
                if (i.kind == InterfKind::AdjacentChannel && i.lastCarrier == s.nc - 1) gotAdj = true;
            }
            CHECK(gotCw && gotLte && gotAdj && v.size() == (ltec < 0 ? 2u : 3u), "nc %d off %.0f: cw %d lte %d adj %d n %zu", s.nc, off, gotCw, gotLte, gotAdj, v.size());
        }
    std::vector<float> clean(6817);
    for (auto& v : clean) v = 25 + 1.5f * n(rng);
    CHECK(locateInterferers(clean, 0, 1116).empty(), "clean channel");
}

static void testSatFade() {
    CHECK(std::fabs(s2QefEsN0(4) - 1.00) < 1e-9 && std::fabs(s2QefEsN0(13) - 6.62) < 1e-9 && std::fabs(s2QefEsN0(28) - 16.05) < 1e-9, "table");
    CHECK(std::isnan(s2QefEsN0(0)) && std::isnan(s2QefEsN0(29)), "out of range");
    CHECK(std::fabs(s1QefEsN0(3, 4) - (5.5 + 10 * std::log10(1.5 * 188.0 / 204))) < 1e-9 && std::isnan(s1QefEsN0(4, 5)), "DVB-S");
    std::mt19937 rng(5);
    std::normal_distribution<double> n(0, 0.15);
    SatFadeLog l;
    double t = 0;
    const int mc = 13;   // 8PSK 2/3
    for (int i = 0; i < 1200; i++, t++) l.sample(t, 11.0 + n(rng), mc, s2QefEsN0(mc));
    CHECK(std::fabs(l.clearSkyDb() - 11.0) < 0.4 && std::fabs(l.marginDb() - 4.38) < 0.6 && !l.fading(), "clear %.2f margin %.2f", l.clearSkyDb(), l.marginDb());
    // a 3 dB rain fade over two minutes, ACM drops to QPSK 3/4 at the bottom
    for (int i = 0; i < 120; i++, t++) {
        const double d = 3 * std::sin(kPi * i / 120);
        const int m = d > 2 ? 7 : mc;
        l.sample(t, 11.0 - d + n(rng), m, s2QefEsN0(m));
    }
    for (int i = 0; i < 60; i++, t++) l.sample(t, 11.0 + n(rng), mc, s2QefEsN0(mc));
    CHECK(l.fades().size() == 1 && std::fabs(l.fades()[0].depthDb - 3) < 0.5 && l.fades()[0].length > 80 && l.fades()[0].length < 120,
          "fade %zu depth %.2f len %.0f", l.fades().size(), l.fades().empty() ? 0 : l.fades()[0].depthDb, l.fades().empty() ? 0 : l.fades()[0].length);
    CHECK(l.modcodChanges().size() == 2 && l.modcodChanges()[0].to == 7 && l.modcodChanges()[1].to == mc, "acm %zu", l.modcodChanges().size());
    // short dips are not fades
    for (int i = 0; i < 3; i++, t++) l.sample(t, 9.0, mc, s2QefEsN0(mc));
    for (int i = 0; i < 30; i++, t++) l.sample(t, 11.0, mc, s2QefEsN0(mc));
    CHECK(l.fades().size() == 1, "short dip logged");
    // lock lost in a deep fade
    for (int i = 0; i < 20; i++, t++) l.sample(t, 0, mc, s2QefEsN0(mc), false);
    for (int i = 0; i < 20; i++, t++) l.sample(t, 11.0, mc, s2QefEsN0(mc));
    CHECK(l.fades().size() == 2 && l.fades()[1].lostLock, "lock loss");
}

static void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

static void testCellTx() {
    // build the descriptor: two cells, the first with one subcell
    auto enc = [](double deg, double scale) { return (unsigned)(uint16_t)(int16_t)std::lround(deg * 32768.0 / scale); };
    std::vector<uint8_t> d;
    put16(d, 0x0101); put16(d, enc(54.60, 90)); put16(d, enc(25.20, 180));
    unsigned el = (unsigned)std::lround(0.20 * 32768 / 90), eo = (unsigned)std::lround(0.30 * 32768 / 180);
    d.push_back((uint8_t)(el >> 4)); d.push_back((uint8_t)((el & 15) << 4 | eo >> 8)); d.push_back((uint8_t)eo);
    d.push_back(8);
    d.push_back(3); put16(d, enc(54.70, 90)); put16(d, enc(25.30, 180)); d.push_back(0); d.push_back(0x10); d.push_back(0x10);
    put16(d, 0x0202); put16(d, enc(-33.9, 90)); put16(d, enc(-151.2, 180)); d.push_back(0); d.push_back(0); d.push_back(0); d.push_back(0);
    auto cells = parseCellList(d.data(), d.size());
    CHECK(cells.size() == 3 && cells[1].cellIdExtension == 3 && std::fabs(cells[0].latDeg - 54.60) < 0.003 && std::fabs(cells[2].lonDeg + 151.2) < 0.006
          && std::fabs(cells[0].extentLatDeg - 0.20) < 0.003 && std::fabs(cells[0].extentLonDeg - 0.30) < 0.006, "parse %zu", cells.size());
    CHECK(parseCellList(d.data(), 7).empty(), "truncated");
    CellTxBook b;
    b.addNit(cells);
    b.setUser(54.687, 25.280);   // Vilnius
    auto a = b.lookup(0x0101);
    CHECK(a.known && a.fromNit && std::fabs(a.latDeg - 54.70) < 0.01 && a.distKm >= 0 && a.distKm < 5, "lookup dist %.1f", a.distKm);
    auto s = b.lookup(0x0101, 3);
    CHECK(s.known && std::fabs(s.latDeg - 54.70) < 0.01, "subcell");
    CHECK(!b.lookup(0x0303).known, "unknown cell");
    b.name(0x0303, "Vilnius TV tower");
    CellTxBook c; c.load(b.save());
    CHECK(c.lookup(0x0303).name == "Vilnius TV tower" && !c.text(0x0303).empty(), "book");
    auto far = b.lookup(0x0202);
    CHECK(far.distKm > 15000, "antipode-ish %.0f", far.distKm);
}

// one-pole low-pass, for the impaired path
static std::vector<float> lowpass(const std::vector<float>& x, double fc, double fs) {
    const double a = std::exp(-2 * kPi * fc / fs);
    std::vector<float> y(x.size());
    double s = x.empty() ? 0 : x[0];
    for (size_t i = 0; i < x.size(); i++) { s = a * s + (1 - a) * x[i]; y[i] = (float)s; }
    return y;
}

static void testVits() {
    std::mt19937 rng(6);
    std::normal_distribution<float> n(0, 1);
    for (double fs : {13.5e6, 20e6, 27e6}) {
        const int N = (int)(64e-6 * fs);
        auto t = [&](int i) { return i / fs * 1e6; };   // us
        // line 17: bar 10 - 20 us, 2T pulse at 26 us (T = 100 ns), staircase 32 - 60 us with 4.43 MHz at 20 IRE (DG 6 %, DP 4 deg)
        const double fsc = 4.43361875e6, dg = 0.06, dpDeg = 4;
        std::vector<float> l(N, 0);
        for (int i = 0; i < N; i++) {
            const double u = t(i);
            if (u >= 10 && u < 20) l[i] = 100;
            const double tp = (u - 26) * 1e-6, T = 100e-9;
            if (std::fabs(tp) < 2 * T) l[i] = (float)(100 * std::pow(std::cos(kPi * tp / (4 * T)), 2));
            if (u >= 32 && u < 60) {
                const int k = std::min(5, (int)((u - 32) / (28.0 / 6)));
                const double amp = 20 * (1 - dg * k / 5), ph = dpDeg * kPi / 180 * k / 5;
                l[i] = (float)(k * 20 + amp * std::sin(2 * kPi * fsc * i / fs + ph));
            }
            l[i] += 0.3f * n(rng) + 0.5f;   // noise and a little offset
        }
        VitsResult r = analyseVitsLine(l, fs);
        CHECK(r.bar && std::fabs(r.barIre - 100.5) < 1 && std::fabs(r.barTiltPct) < 1, "fs %.1f bar %d %.1f tilt %.2f", fs / 1e6, r.bar, r.barIre, r.barTiltPct);
        CHECK(r.pulse2T && std::fabs(r.hadNs - 200) < 25 && r.kpbPct < 1.0, "fs %.1f pulse %d had %.0f kpb %.2f", fs / 1e6, r.pulse2T, r.hadNs, r.kpbPct);
        CHECK(r.staircase && r.steps == 6 && std::fabs(r.subcarrierHz - fsc) < 0.03e6 && std::fabs(r.dgPct - 6) < 1 && std::fabs(r.dpDeg - dpDeg) < 1,
              "fs %.1f stairs %d n %d fsc %.3f dg %.2f dp %.2f", fs / 1e6, r.staircase, r.steps, r.subcarrierHz / 1e6, r.dgPct, r.dpDeg);
        // the same line through a 3 MHz low-pass: the pulse loses height, K-PB grows
        VitsResult f = analyseVitsLine(lowpass(l, 3e6, fs), fs);
        CHECK(f.pulse2T && f.kpbPct > 2 && f.hadNs > r.hadNs, "fs %.1f filtered kpb %.2f had %.0f", fs / 1e6, f.kpbPct, f.hadNs);

        // line 18: multiburst 0.5, 1, 2, 4, 4.8, 5.8 MHz (5.8 only when fs allows) on a 50 IRE pedestal, 30 IRE amplitude
        std::vector<double> fr = {0.5e6, 1e6, 2e6, 4e6, 4.8e6};
        if (fs > 15e6) fr.push_back(5.8e6);
        std::vector<float> m(N, 0);
        for (int i = 0; i < N; i++) {
            const double u = t(i);
            if (u >= 10 && u < 62) m[i] = 50;
            for (size_t k = 0; k < fr.size(); k++) {
                const double a = 12 + k * 8.5;
                if (u >= a && u < a + 6.5) m[i] = (float)(50 + 30 * std::sin(2 * kPi * fr[k] * (u - a) * 1e-6));
            }
            m[i] += 0.3f * n(rng);
        }
        VitsResult mb = analyseVitsLine(m, fs);
        CHECK(mb.multiburst && mb.bursts.size() == fr.size(), "fs %.1f multiburst %d %zu", fs / 1e6, mb.multiburst, mb.bursts.size());
        for (size_t k = 0; k < mb.bursts.size() && k < fr.size(); k++)
            CHECK(std::fabs(mb.bursts[k].freqHz - fr[k]) < 0.04 * fr[k] && std::fabs(mb.bursts[k].relDb) < 0.5, "burst %zu f %.2f rel %.2f", k, mb.bursts[k].freqHz / 1e6, mb.bursts[k].relDb);
        // after a 2 MHz one-pole the response falls with frequency
        VitsResult mf = analyseVitsLine(lowpass(m, 2e6, fs), fs);
        CHECK(mf.multiburst && mf.bursts.size() == fr.size(), "fs %.1f filtered multiburst", fs / 1e6);
        if (mf.bursts.size() == fr.size()) {
            const double h0 = 1 / std::sqrt(1 + std::pow(fr[0] / 2e6, 2));
            const double h3 = 1 / std::sqrt(1 + std::pow(fr[3] / 2e6, 2));
            const double want = 20 * std::log10(h3 / h0);
            CHECK(std::fabs(mf.bursts[3].relDb - want) < 1.5, "fs %.1f 4 MHz %.2f dB want %.2f", fs / 1e6, mf.bursts[3].relDb, want);
        }
        CHECK(!r.multiburst && !mb.bar && !mb.staircase, "cross detection");
    }
    std::vector<float> black(1000, 0);
    CHECK(!analyseVitsLine(black, 13.5e6).any(), "black line");
}

int main() {
    testOffAir(); testInterferer(); testSatFade(); testCellTx(); testVits();
    return testSummary("smart5_tv");
}
