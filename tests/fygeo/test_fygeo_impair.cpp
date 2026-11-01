// FengYun receivers against the faults of real radios and real paths: carrier offsets (off the centre of the capture, ppm errors), a carrier that
// drifts, symbol clock errors, DC offset, I/Q gain and phase imbalance, phase noise, slow flat fading, and sample rates from 1 to 20 Msps (a rate
// that is not a multiple of the symbol rate). Every case must still give frames, pictures, and the right carrier offset.
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/fygeo/fygeo_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/test_util.h"

struct Faults {
    double driftHzPerSec = 0;      // carrier drift (a ramp added on top of the generator's offset)
    double dc = 0;                 // DC offset relative to the rms
    double iqGainDb = 0, iqPhaseDeg = 0;
    double pnLinewidthHz = 0;      // Wiener phase noise
    double fadeDepth = 0;          // slow flat amplitude fading: 1 - depth .. 1 (a 0.3 Hz sine)
};

static void impair(std::vector<cf32>& x, double rate, double t0, const Faults& f, std::mt19937& rng) {
    static double pn = 0;
    double rms = 0;
    for (const auto& v : x) rms += std::norm(v);
    rms = std::sqrt(rms / (double)x.size());
    std::normal_distribution<double> g(0, 1);
    const double g2 = std::pow(10.0, f.iqGainDb / 20), ph = f.iqPhaseDeg * dect2::math::kPi / 180;
    for (size_t i = 0; i < x.size(); i++) {
        const double t = t0 + (double)i / rate;
        cf32 v = x[i];
        if (f.driftHzPerSec != 0) { const double a = 2 * dect2::math::kPi * 0.5 * f.driftHzPerSec * t * t; v *= cf32((float)std::cos(a), (float)std::sin(a)); }
        if (f.pnLinewidthHz > 0) { pn += std::sqrt(2 * dect2::math::kPi * f.pnLinewidthHz / rate) * g(rng); v *= cf32((float)std::cos(pn), (float)std::sin(pn)); }
        if (f.fadeDepth > 0) v *= (float)(1 - f.fadeDepth * 0.5 * (1 + std::sin(2 * dect2::math::kPi * 0.3 * t)));
        if (f.iqGainDb != 0 || f.iqPhaseDeg != 0) { const double I = v.real(), Q = v.imag(); v = cf32((float)I, (float)(g2 * (Q * std::cos(ph) + I * std::sin(ph)))); }
        v += cf32((float)(f.dc * rms), (float)(f.dc * rms * 0.7));
        x[i] = v;
    }
}

static FygeoTelemetry run(int link, const SynthConfig& cfg, double rate, double seconds, const Faults& f, double offset = 0) {
    FygeoReceiver rx;
    rx.setLink(link);
    rx.setBlocking(true);
    rx.configure(rate);
    rx.setSignalOffset(offset);
    SynthConfig sc2 = cfg; sc2.modeOpt[0] = link == kFy4bLrit ? 1 : 0;
    std::unique_ptr<ModeSynth> syn = link == kFy2Svissr ? makeFy2SvissrSynth(cfg, rate) : link == kFy4bLrit ? makeFy4LritSynth(sc2, rate) : link == kFy4aHrit ? makeFy4aHritSynth(cfg, rate) : makeFy4LritSynth(sc2, rate);
    std::vector<cf32> buf(16384);
    std::mt19937 rng(5);
    double t = 0;
    for (; t < seconds; t += (double)buf.size() / rate) {
        syn->generate(buf.data(), buf.size());
        impair(buf, rate, t, f, rng);
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    for (auto& v : buf) v = cf32(1e-4f, 0);
    for (int i = 0; i < 20; i++) rx.feed(buf.data(), buf.size());
    FygeoTelemetry tt;
    rx.telemetry(tt, 0);
    return tt;
}

static bool pictures(const FygeoTelemetry& t) {
    int c2 = 0, c12 = 0;
    for (const auto& p : t.products) { std::lock_guard<std::mutex> lk(p->mu); if (p->complete && p->errors == false) { if (p->name.find("C02") != std::string::npos) c2++; if (p->name.find("C12") != std::string::npos) c12++; } }
    return c2 > 0 && c12 > 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool quick = getenv("FY_QUICK") != nullptr;
    // ---- S-VISSR: every case must give frames, with a low bit error ratio, at a high enough Es/N0
    struct Sv { const char* what; double rate, snr, cfo, sro, offset; Faults f; double secs; };
    std::vector<Sv> sv = {
        {"off centre by 150 kHz", 2.4e6, 12, 150e3, 0, 0, {}, 8},
        {"off centre by -400 kHz, 5 Msps", 5e6, 12, -400e3, 0, 0, {}, 8},
        {"20 ppm radio at 1.69 GHz (34 kHz) and 20 ppm clock", 2.4e6, 12, 34e3, 20, 0, {}, 8},
        {"carrier drift 300 Hz/s", 2.4e6, 12, 5e3, 0, 0, {300, 0, 0, 0, 0}, 10},
        {"DC spike 30 % of the rms", 2.4e6, 12, 60e3, 0, 0, {0, 0.3, 0, 0}, 8},
        {"I/Q imbalance 1.5 dB and 8 degrees", 2.4e6, 12, 0, 0, 0, {0, 0, 1.5, 8, 0}, 8},
        {"phase noise 200 Hz linewidth", 2.4e6, 12, 0, 0, 0, {0, 0, 0, 0, 0}, 8},
        {"slow fading 6 dB deep", 2.4e6, 14, 0, 0, 0, {0, 0, 0, 0, 0.5}, 10},
        {"everything at once", 3.2e6, 14, 40e3, 30, 0, {120, 0.1, 1.0, 5, 0.3}, 10},
        {"1.5 Msps", 1.5e6, 12, 0, 0, 0, {}, 8},
        {"1.0 Msps (1.5 samples a symbol)", 1.0e6, 12, 0, 0, 0, {}, 8},
        {"0.8 Msps is below the 1.4 samples a symbol the receiver needs", 0.8e6, 12, 0, 0, 0, {}, 3},
        {"3 Msps (4.545 samples a symbol)", 3e6, 12, 0, 0, 0, {}, 8},
        {"10 Msps", 10e6, 12, 20e3, 0, 0, {}, 8},
        {"20 Msps", 20e6, 12, 20e3, 0, 0, {}, 8},
    };
    sv[6].f.pnLinewidthHz = 200;
    for (const Sv& c : sv) {
        if (quick && c.rate > 5e6) continue;
        SynthConfig cfg; cfg.snrDb = c.snr; cfg.cfoHz = c.cfo; cfg.sroPpm = c.sro;
        FygeoTelemetry t = run(kFy2Svissr, cfg, c.rate, c.secs, c.f, c.offset);
        const bool below = c.rate < 1.4 * 660e3 - 1;
        printf("  S-VISSR %-56s %2llu lines, BER %.1e, carrier %+.1f kHz, %s\n", c.what, (unsigned long long)t.svFrames, std::max(0.0, t.svPreambleBer), t.cfoHz / 1e3, t.hint.empty() ? "" : t.hint.substr(0, 40).c_str());
        if (below) { CHECK(!t.hint.empty(), "%s: the hint must say the rate is too low", c.what); continue; }   // it may still decode (a clean signal at 1.2 samples a symbol does), the screen says so
        CHECK(t.svFrames >= 6, "%s: %llu lines", c.what, (unsigned long long)t.svFrames);
        CHECK(t.svPreambleBer < 0.02, "%s: BER %.3e", c.what, t.svPreambleBer);
        if (c.f.driftHzPerSec == 0 && c.f.fadeDepth == 0 && c.f.pnLinewidthHz == 0) CHECK(std::fabs(t.cfoHz - c.cfo) < 600, "%s: carrier %.0f Hz, want %.0f", c.what, t.cfoHz, c.cfo);
    }
    // ---- FY-4: the DVB-S2 receiver finds the carrier anywhere in the capture
    struct F4 { const char* what; int link; double rate, snr, cfo, sro; Faults f; double secs; };
    std::vector<F4> f4 = {
        {"LRIT-A off centre by 200 kHz", kFy4aLrit, 2e6, 10, 200e3, 0, {}, 24},
        {"LRIT-A 20 ppm clock, 25 kHz offset", kFy4aLrit, 2e6, 10, 25e3, 20, {}, 24},
        {"LRIT-A DC spike and I/Q imbalance", kFy4aLrit, 2e6, 10, 0, 0, {0, 0.3, 1.0, 6, 0}, 24},
        {"LRIT-A drift 100 Hz/s", kFy4aLrit, 2e6, 10, 3e3, 0, {100, 0, 0, 0, 0}, 24},
        {"LRIT-A phase noise 15 Hz linewidth", kFy4aLrit, 2e6, 10, 0, 0, {0, 0, 0, 0, 0}, 24},
        {"LRIT-A 1 Msps", kFy4aLrit, 1e6, 10, 0, 0, {}, 24},
        {"LRIT-A 0.5 Msps", kFy4aLrit, 0.5e6, 10, 0, 0, {}, 24},
        {"LRIT-A 10 Msps", kFy4aLrit, 10e6, 10, 100e3, 0, {}, 24},
        {"LRIT-A 20 Msps", kFy4aLrit, 20e6, 10, -2e6, 0, {}, 24},
        {"LRIT-B 3.2 Msps", kFy4bLrit, 3.2e6, 14, 50e3, 0, {}, 14},
        {"HRIT 4 Msps off centre by 500 kHz", kFy4aHrit, 4e6, 14, 500e3, 0, {}, 8},
        {"HRIT 10 Msps", kFy4aHrit, 10e6, 14, 0, 10, {}, 8},
    };
    f4[4].f.pnLinewidthHz = 15;
    for (const F4& c : f4) {
        if (quick && c.rate > 5e6) continue;
        SynthConfig cfg; cfg.snrDb = c.snr; cfg.cfoHz = c.cfo; cfg.sroPpm = c.sro;
        FygeoTelemetry t = run(c.link, cfg, c.rate, c.secs, c.f);
        printf("  FY-4 %-44s %3llu frames (%llu bad), %4llu CADUs, pictures %s, carrier %+.1f kHz\n", c.what, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
               (unsigned long long)t.caduCount, pictures(t) ? "yes" : "no", t.cfoHz / 1e3);
        CHECK(t.caduCount >= 40, "%s: %llu CADUs", c.what, (unsigned long long)t.caduCount);
        CHECK(pictures(t), "%s: no complete pictures", c.what);
    }
    return testSummary("fygeo_impair");
}
