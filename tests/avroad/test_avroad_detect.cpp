// Radars and OFDM structure: simulated signals only.
#include "avroad_gen.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include <cstdio>
#include <functional>

using namespace dect2;
using namespace dect2::avroad;
using namespace avgen;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AvRoadTelemetry run(int v, const std::vector<cf32>& x, double fs) {
    AvRoadReceiver r; r.setVariant(v); r.configure(fs);
    for (size_t i = 0; i < x.size(); i += 65536) r.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    std::vector<cf32> tail((size_t)(fs * 0.3)); r.feed(tail.data(), tail.size());
    AvRoadTelemetry t; r.telemetry(t, 0);
    return t;
}
static std::string fact(const AvRoadTelemetry& t, const char* k) { for (auto& f : t.facts) if (f.first == k) return f.second; return ""; }

// A rotating beam: Gaussian main lobe plus a flat sidelobe level. prf(t): the pulse repetition frequency in force.
struct Beam { double rpm, beamDeg, widthUs; float peakDb; float sideDb; };
static std::vector<cf32> radar(double fs, double seconds, const std::function<Beam(double)>& beam, const std::function<double(int)>& pri, unsigned seed) {
    std::vector<cf32> x((size_t)(fs * seconds));
    addNoise(x, 0, 1, seed);
    double t = 0.1; int k = 0; std::mt19937 r(seed + 9); std::uniform_real_distribution<double> ph(0, 2 * kPi);
    double az = 0, lastT = 0;
    while (t < seconds - 0.01) {
        const Beam b = beam(t);
        az += (t - lastT) * 360 * b.rpm / 60; lastT = t;
        const double da = std::remainder(az, 360.0);
        double g = -12 * (da * da) / (b.beamDeg * b.beamDeg);       // dB, -3 dB at half beamwidth
        if (g < b.sideDb) g = b.sideDb;
        const float a = std::pow(10.f, (b.peakDb + (float)g) / 20);
        const size_t from = (size_t)(t * fs), len = std::max<size_t>(1, (size_t)(b.widthUs * 1e-6 * fs));
        const cf32 c = std::polar(a, (float)ph(r));
        for (size_t i = from; i < from + len && i < x.size(); i++) x[i] += c;
        t += pri(k++);
    }
    return x;
}

static void testRadars() {
    const double fs = 2000000;
    for (double rpm : {24.0, 48.0}) for (unsigned s : {1u, 2u}) {
        double T = 60 / rpm;
        auto x = radar(fs, T * 5.2, [&](double) { return Beam{rpm, 2.0, 0.8, 30, -28}; }, [](int) { return 1.0 / 1000; }, s);
        AvRoadTelemetry t = run(MarineRadar, x, fs);
        CHECK(t.headline.find("rotation") == 0, "marine %.0f rpm: %s", rpm, t.headline.c_str());
        bool ok = false; double got = 0; if (sscanf(t.headline.c_str(), "rotation %lf", &got) == 1) ok = std::fabs(got - T) < 0.02 * T;
        CHECK(ok, "marine period %f want %f: %s", got, T, t.headline.c_str());
        CHECK(fact(t, "PRF").find("1000 Hz") == 0, "marine PRF: %s", fact(t, "PRF").c_str());
        double bw = 0; sscanf(fact(t, "beamwidth").c_str(), "%lf", &bw);
        CHECK(bw > 1.2 && bw < 5, "beamwidth %.2f (%s)", bw, fact(t, "beamwidth").c_str());
    }
    // Weak: main lobe 16 dB above the noise, no sidelobes above the noise
    { auto x = radar(fs, 12.5, [](double) { return Beam{24, 2.0, 0.8, 18, -40}; }, [](int) { return 1.0 / 1500; }, 3); AvRoadTelemetry t = run(MarineRadar, x, fs); CHECK(t.headline.find("rotation") == 0, "weak marine: %s", t.headline.c_str()); }
    // Noise only
    { std::vector<cf32> x((size_t)(fs * 6)); addNoise(x, 0, 1, 11); AvRoadTelemetry t = run(MarineRadar, x, fs); CHECK(t.blocksOk == 0 && t.headline.find("rotation") != 0, "radar in noise: %s", t.headline.c_str()); }

    // Weather: volume scan, four sweeps then repeat, level steps down with elevation, dual PRF at the top
    {
        const double fs2 = 2000000, rot = 6.0;
        const float steps[4] = {38, 34, 30, 26};
        auto beam = [&](double t) { int r = (int)(t / rot) % 8; int s = r % 4; return Beam{60 / rot, 1.0, 2.0, steps[s], -40}; };
        std::vector<double> pat;
        auto x = radar(fs2, rot * 8.5, beam, [](int i) { return i % 2 ? 1.0 / 900 : 1.0 / 1200; }, 4);
        AvRoadTelemetry t = run(WeatherRadar, x, fs2);
        printf("weather: %s | PRF %s | scan %s | rows %zu\n", t.headline.c_str(), fact(t, "PRF").c_str(), fact(t, "scan pattern").c_str(), t.rows.size());
        for (auto it = t.rows.rbegin(); it != t.rows.rend(); ++it) printf("   %.1f sweep %s %s\n", it->timeSec, it->id.c_str(), it->text.c_str());
        double got = 0; sscanf(t.headline.c_str(), "rotation %lf", &got);
        CHECK(std::fabs(got - rot) < 0.1, "weather period %f", got);
        CHECK(fact(t, "PRF").find("dual") == 0, "dual PRF: %s", fact(t, "PRF").c_str());
        CHECK(fact(t, "scan pattern").find("1 volumes") != std::string::npos || fact(t, "scan pattern").find("2 volumes") != std::string::npos, "volume restart seen: %s", fact(t, "scan pattern").c_str());
    }
}

// CP-OFDM random QPSK at the mode's rate; bursts optional
static std::vector<cf32> ofdm(double rate, int nfft, int ncp, double seconds, double frameSec, double duty, double cfo, double snr, unsigned seed) {
    std::mt19937 r(seed); std::uniform_int_distribution<int> b(0, 3);
    std::vector<cf32> x;
    const size_t total = (size_t)(rate * seconds);
    std::vector<cf32> sym((size_t)nfft);
    while (x.size() < total) {
        // an inverse DFT of 300 used carriers would be slow for many symbols: draw a band-limited sequence, which has the same CP property
        for (auto& s : sym) { const int k = b(r); s = std::polar(1.f, (float)(kPi * (k * 2 + 1) / 4)); }
        for (int i = 0; i < ncp; i++) x.push_back(sym[(size_t)(nfft - ncp + i)]);
        for (int i = 0; i < nfft; i++) x.push_back(sym[(size_t)i]);
    }
    x.resize(total);
    if (frameSec > 0) for (size_t i = 0; i < total; i++) { double p = std::fmod((double)i / rate, frameSec) / frameSec; if (p > duty) x[i] = 0; }
    mix(x, cfo, rate);
    addNoise(x, snr, 1, seed + 4);
    return x;
}
static void testOfdm() {
    struct K { int v; double rate; int nfft, ncp; double frame, duty; } ks[3] = {
        {Ldacs, 625000, 64, 11, 0, 1}, {Aeromacs, 5600000, 512, 64, 5e-3, 0.6}, {Cv2x, 15360000, 1024, 72, 0, 1}};
    for (auto& k : ks) {
        for (double cfo : {0.0, 2000.0}) for (double snr : {25.0, 10.0, 5.0}) {
            auto x = ofdm(k.rate, k.nfft, k.ncp, 0.5, k.frame, k.duty, cfo, snr, 3);
            AvRoadTelemetry t = run(k.v, x, k.rate);
            CHECK(t.blocksOk > 0, "%s cfo %.0f snr %.0f: headline %s corr %s", variantName(k.v), cfo, snr, t.headline.c_str(), fact(t, "prefix correlation").c_str());
            if (t.blocksOk && cfo != 0) { double g = 0; sscanf(fact(t, "carrier offset").c_str(), "%lf", &g); CHECK(std::fabs(g - cfo) < 200, "%s cfo measured %.0f want %.0f", variantName(k.v), g, cfo); }
        }
        std::vector<cf32> x((size_t)(k.rate * 0.5)); addNoise(x, 0, 1, 5);
        AvRoadTelemetry t = run(k.v, x, k.rate);
        CHECK(t.blocksOk == 0, "%s noise detected", variantName(k.v));
    }
    // AeroMACS 5 ms bursts
    { auto x = ofdm(5600000, 512, 64, 0.5, 5e-3, 0.6, 0, 20, 2); AvRoadTelemetry t = run(Aeromacs, x, 5600000); CHECK(fact(t, "frame structure").find("bursts") == 0, "bursts: %s", fact(t, "frame structure").c_str()); }
    // Wrong numerology: LDACS parameters do not fire on AeroMACS-shaped OFDM
    { auto x = ofdm(625000, 512, 64, 0.4, 0, 1, 0, 20, 2); CHECK(run(Ldacs, x, 625000).blocksOk == 0, "wrong numerology"); }
}

int main() {
    testRadars(); testOfdm();
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
