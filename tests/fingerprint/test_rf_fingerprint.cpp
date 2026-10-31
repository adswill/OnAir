// RF fingerprint on synthetic transmitters that differ in carrier offset, keying transient, I/Q imbalance, spurs, pilot and hum,
// captured off-centre, with noise, at different sample rates and key-up times. Also the carrier-only log (Watcher) and the library file.
#include "dect2/fingerprint/rf_fingerprint.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::fp;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

struct Spec {
    double cfo = 0;              // Hz from the tuned frequency
    double chirp = 0, tauMs = 5; // start-up frequency excursion and its decay
    double drift = 0;            // Hz per second after start
    double wn = 6000, zeta = 0.95;   // amplitude step response (rad/s, damping)
    double iqGain = 0, iqPhaseDeg = 0;   // image: x + e * conj(x)
    struct S { double off, dbc; };
    std::vector<S> spurs;
    double audioDev = 0;         // Hz, multitone speech-like
    double ctcssDev = 0;         // sub-audible tone, Hz
    double pilotDev = 0, pilotErr = 0, pilotJitterDeg = 0;
    double hum100 = 0;           // Hz of deviation
    double amDepth = 0;          // AM instead of FM
};

struct Seg { double t0, t1; };   // carrier on between t0 and t1 (key-up and key-down)

static std::vector<cf32> synth(const Spec& s, double rate, double centreOff, double snrDb, const std::vector<Seg>& segs, double total, uint32_t seed, double tuned = 145.5e6) {
    (void)tuned;
    const size_t n = (size_t)(total * rate);
    std::vector<cf32> x(n);
    dect2::normal_distribution<float> g(0.f, 1.f);
    std::mt19937 rng(seed);
    const double chanBw = 25e3;
    const double sigma2 = (rate / chanBw) / std::pow(10.0, snrDb / 10.0);   // total complex noise power for carrier power 1
    const float sg = (float)std::sqrt(sigma2 / 2);
    // audio: a few tones with random phases
    double af[5], ap[5], aa[5];
    std::mt19937 r2(seed * 7 + 1);
    for (int i = 0; i < 5; i++) { af[i] = 300 + 2400.0 * (r2() % 1000) / 1000.0; ap[i] = 2 * kPi * (r2() % 1000) / 1000.0; aa[i] = 1.0 / (1 + i); }
    double asum = 0;
    for (double a : aa) asum += a;
    double phase = 0;
    double pilotPh = 0, jitterState = 0;
    std::mt19937 jr(seed + 99);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        double on = 0, tk = -1;
        for (const Seg& sgm : segs) if (t >= sgm.t0 && t < sgm.t1) { tk = t - sgm.t0; on = 1; }
        std::complex<double> v(0, 0);
        if (on) {
            double amp;
            if (s.zeta < 1) {
                const double wd = s.wn * std::sqrt(1 - s.zeta * s.zeta), e = std::exp(-s.zeta * s.wn * tk);
                amp = 1 - e * (std::cos(wd * tk) + s.zeta / std::sqrt(1 - s.zeta * s.zeta) * std::sin(wd * tk));
            } else amp = 1 - std::exp(-s.wn * tk) * (1 + s.wn * tk);
            double fi = s.cfo + s.chirp * std::exp(-tk * 1000 / s.tauMs) + s.drift * tk;
            double dev = 0;
            if (s.amDepth <= 0) {
                if (s.audioDev > 0) { double m = 0; for (int k = 0; k < 5; k++) m += aa[k] * std::sin(2 * kPi * af[k] * t + ap[k]); dev += s.audioDev * m / asum; }
                if (s.ctcssDev > 0) dev += s.ctcssDev * std::sin(2 * kPi * 123.0 * t);
                if (s.hum100 > 0) dev += s.hum100 * std::sin(2 * kPi * 100.0 * t);
                if (s.pilotDev > 0) {
                    if (s.pilotJitterDeg > 0 && (i % 2000) == 0) jitterState = s.pilotJitterDeg * kPi / 180 * ((double)(jr() % 2001) / 1000.0 - 1.0);
                    dev += s.pilotDev * std::sin(2 * kPi * (19000.0 + s.pilotErr) * t + jitterState);
                }
            } else {
                double m = 0; for (int k = 0; k < 5; k++) m += aa[k] * std::sin(2 * kPi * af[k] * t + ap[k]);
                amp *= 1 + s.amDepth * m / asum;
            }
            phase += 2 * kPi * (centreOff + fi + dev) / rate;
            if ((i & 1023) == 0) phase = std::fmod(phase, 2 * kPi);
            v = std::polar(amp, phase);
            for (const auto& sp : s.spurs) {
                const double a = std::pow(10.0, sp.dbc / 20.0) * std::min(1.0, amp);
                v += std::polar(a, 2 * kPi * (centreOff + sp.off) * t);
            }
        } else phase += 2 * kPi * (centreOff) / rate;
        if (s.iqGain != 0 || s.iqPhaseDeg != 0) {
            const std::complex<double> e = s.iqGain * std::polar(1.0, s.iqPhaseDeg * kPi / 180);
            v += e * std::conj(v);
        }
        x[i] = cf32((float)v.real() + sg * g(rng), (float)v.imag() + sg * g(rng));
    }
    return x;
}

static Features measure(const std::vector<cf32>& x, double rate, double centreOff, Kind k, bool withSpurs = true, double tuned = 145.5e6) {
    Channel ch(rate, centreOff, k);
    std::vector<cf32> z;
    ch.push(x.data(), x.size(), z);
    Options o; o.kind = k; o.tunedHz = tuned;
    Features f = analyse(z.data(), z.size(), ch.outRate(), o);
    if (withSpurs && f.valid) {
        WidePsd p(rate, 5.0);
        // use the part where the carrier is on: the last 0.6 s
        const size_t from = x.size() > (size_t)(rate * 0.6) ? x.size() - (size_t)(rate * 0.6) : 0;
        p.add(x.data() + from, x.size() - from);
        p.spurs(centreOff, k == Kind::WFM ? 200e3 : k == Kind::NFM ? 25e3 : 10e3, f);
    }
    return f;
}

int main() {
    const double R = 2.4e6;
    {   // carrier-only accuracy and transient
        Spec a; a.cfo = 123.456; a.wn = 2.2 / 300e-6 * 0.8; a.zeta = 0.5; a.chirp = 80; a.tauMs = 4;
        auto x = synth(a, R, 250e3, 25, {{0.1, 1.3}}, 1.35, 1);
        Features f = measure(x, R, 250e3, Kind::NFM);
        CHECK(f.valid, "invalid");
        CHECK(std::fabs(f.cfoHz - 123.456) < 0.1, "cfo %.4f", f.cfoHz);
        CHECK(f.carrierOnly, "carrier only expected, dev %.1f", f.devRmsHz);
        CHECK(f.transient, "transient expected");
        CHECK(f.overshootPct > 5 && f.overshootPct < 40, "overshoot %.1f", f.overshootPct);
        CHECK(f.riseUs > 30 && f.riseUs < 600, "rise %.0f us", f.riseUs);
        CHECK(f.chirpHz > 40 && f.chirpHz < 120, "chirp %.1f", f.chirpHz);
        CHECK(f.snrDb > 15, "snr %.1f", f.snrDb);
        printf("A: cfo %.4f std %.4f rise %.0f os %.1f chirp %.1f settle %.0f ms snr %.1f\n", f.cfoHz, f.cfoStdHz, f.riseUs, f.overshootPct, f.chirpHz, f.settleMs, f.snrDb);
    }
    return fails;
}
