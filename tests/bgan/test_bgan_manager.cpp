// BGAN manager: finds several bearers in one wide capture, identifies each, reads the signalling, and tells a GMR-2 class carrier apart.
#include "dect2/bgan/bgan_manager.h"
#include "dect2/bgan/bgan_gen.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2; using namespace dect2::bgan;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// a simple filtered OQPSK carrier at the GMR-2 forward rate, wide enough for the width classifier (no framing: only the spectrum is checked)
static void addOqpsk(std::vector<cf32>& out, double fs, double offsetHz, double amp, uint32_t seed) {
    const double rs = 270833.33 / 2;
    std::mt19937 rng(seed);
    const int span = 8;
    const double dtSym = rs / fs;
    std::vector<float> bits(int(out.size() * dtSym / 2) * 2 + 64);
    for (auto& b : bits) b = (rng() & 1) ? 1.f : -1.f;
    double t = 0, ph = 0;
    for (size_t n = 0; n < out.size(); n++, t += dtSym) {
        const int k0 = int(std::floor(t));
        float i = 0, q = 0;
        for (int k = k0 - span; k <= k0 + span; k++) {
            if (k < 0 || size_t(k) * 2 + 1 >= bits.size()) continue;
            const double x = t - k;
            const double x2 = x - 0.5;
            const double p1 = std::fabs(x) < 8 ? std::cos(M_PI * x / 16) * std::cos(M_PI * x / 16) * (std::fabs(x) < 1e-6 ? 1 : std::sin(M_PI * x) / (M_PI * x)) : 0;
            const double p2 = std::fabs(x2) < 8 ? std::cos(M_PI * x2 / 16) * std::cos(M_PI * x2 / 16) * (std::fabs(x2) < 1e-6 ? 1 : std::sin(M_PI * x2) / (M_PI * x2)) : 0;
            i += float(p1) * bits[k * 2]; q += float(p2) * bits[k * 2 + 1];
        }
        ph += 2 * M_PI * offsetHz / fs;
        out[n] += float(amp) * cf32(float(std::cos(ph)), float(std::sin(ph))) * cf32(i, q);
    }
}

int main() {
    const double fs = 2000000;
    SynthConfig sc;
    sc.modeOpt[0] = 0;
    sc.modeVal[0] = 72;
    auto synth = makeSynth(sc, fs);
    Manager m;
    ManagerConfig c; c.rate = fs; c.centerHz = 1545.5e6;
    m.configure(c);
    size_t chunk = size_t(fs * 0.05);
    std::vector<cf32> buf(chunk);
    BganTelemetry t;
    for (int i = 0; i < 200; i++) {
        synth->generate(buf.data(), chunk);
        m.feed(buf.data(), chunk);
    }
    CHECK(m.snapshot(t, 0), "snapshot");
    int decoded = 0, spots = 0;
    for (auto& r : t.rows) { if (r.kind == 1 && r.state == 2) decoded++; if (r.spotBeam >= 0) spots++; std::printf("%.4f MHz w %.0f kind %d %s state %d cn0 %.1f spot %d beam %d plmn %s sat %s load %.0f%%\n", r.freqHz / 1e6, r.widthHz, r.kind, (r.bearer + " " + r.subtype).c_str(), r.state, r.cn0Dbhz, r.spotBeam, r.beamType, r.plmn.c_str(), r.satellite.c_str(), r.load * 100); }
    CHECK(decoded >= 5, "decoded carriers %d", decoded);
    CHECK(t.dataValid && !t.satellites.empty() && !t.network.empty(), "summary sat '%s' net '%s'", t.satellites.c_str(), t.network.c_str());
    CHECK(!t.log.empty(), "log");
    CHECK(!t.beams.empty(), "beams");
    bool haveGlobal = false, haveNarrow = false;
    for (auto& r : t.rows) { haveGlobal |= r.beamType == 0 && r.bearer == "F80T0.25Q-1B"; haveNarrow |= r.beamType == 2; }
    CHECK(haveGlobal && haveNarrow, "beam types");

    // the GMR-2 class carrier
    Manager m2;
    ManagerConfig c2; c2.rate = fs; c2.centerHz = 1540e6;
    m2.configure(c2);
    std::mt19937 rng(5);
    std::normal_distribution<float> n(0, 0.002f);
    uint32_t seed = 1;
    for (int i = 0; i < 180; i++) {
        std::fill(buf.begin(), buf.end(), cf32(0, 0));
        addOqpsk(buf, fs, 100000, 0.05, seed++);
        for (auto& v : buf) v += cf32(n(rng), n(rng));
        m2.feed(buf.data(), chunk);
    }
    BganTelemetry t2;
    m2.snapshot(t2, 0);
    int gmr = 0;
    for (auto& r : t2.rows) { std::printf("gmr test: %.4f MHz width %.0f kind %d state %d\n", r.freqHz / 1e6, r.widthHz, r.kind, r.state); if (r.kind == 2) gmr++; }
    CHECK(gmr >= 1 || t2.identifiedGmr >= 1, "GMR-2 class carrier not identified");
    std::printf(fails ? "bgan manager: %d FAILED\n" : "bgan manager: all passed\n", fails);
    return fails ? 1 : 0;
}
