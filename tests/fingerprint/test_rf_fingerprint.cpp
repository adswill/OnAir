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
    double jitterState = 0;
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
    Options o; o.kind = k; o.tunedHz = tuned; o.noiseLag1 = ch.noiseLag1();
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

static Spec txA() { Spec a; a.cfo = 123.456; a.wn = 5000; a.zeta = 0.5; a.chirp = 80; a.tauMs = 4; a.spurs = {{30e3, -25}}; a.iqGain = 0.04; a.iqPhaseDeg = 1.0; return a; }
static Spec txB() { Spec a = txA(); a.wn = 20000; a.zeta = 0.9; a.chirp = -60; a.tauMs = 12; return a; }                  // same carrier, other transient
static Spec txC() { Spec a = txA(); a.spurs = {{-55e3, -28}}; a.iqGain = 0.1; a.iqPhaseDeg = 3; return a; }               // same carrier + transient, other spur and image
static Spec txD() { Spec a = txA(); a.cfo = 138.5; return a; }                                                             // 15 Hz away

int main() {
    const double R = 2.4e6;
    {   // carrier-only accuracy and transient
        auto x = synth(txA(), R, 250e3, 25, {{0.1, 1.3}}, 1.35, 1);
        Features f = measure(x, R, 250e3, Kind::NFM);
        CHECK(f.valid, "invalid");
        CHECK(std::fabs(f.cfoHz - 123.456) < 0.1, "cfo %.4f", f.cfoHz);
        CHECK(f.cfoStdHz < 0.2, "cfo std %.4f", f.cfoStdHz);
        CHECK(f.carrierOnly, "carrier only expected, dev %.1f", f.devRmsHz);
        CHECK(f.transient, "transient expected");
        CHECK(f.overshootPct > 5 && f.overshootPct < 40, "overshoot %.1f", f.overshootPct);
        CHECK(f.riseUs > 30 && f.riseUs < 600, "rise %.0f us", f.riseUs);
        CHECK(f.chirpHz > 30 && f.chirpHz < 120, "chirp %.1f", f.chirpHz);
        CHECK(f.snrDb > 15, "snr %.1f", f.snrDb);
        CHECK(f.spurValid && !f.spurs.empty() && std::fabs(f.spurs[0].offHz - 30e3) < 800 && std::fabs(f.spurs[0].dbc + 25) < 3, "spur %.0f Hz %.1f dBc (n=%zu)",
              f.spurs.empty() ? 0 : f.spurs[0].offHz, f.spurs.empty() ? 0 : f.spurs[0].dbc, f.spurs.size());
        CHECK(f.imageDbc < 90, "image not seen");
        printf("A: cfo %.4f std %.4f rise %.0f os %.1f chirp %.1f settle %.0f ms snr %.1f image %.1f\n", f.cfoHz, f.cfoStdHz, f.riseUs, f.overshootPct, f.chirpHz, f.settleMs, f.snrDb, f.imageDbc);
    }
    {   // identification: 4 transmitters x 3 captures (other noise, key-up time, off-centre, and one at another sample rate)
        const Spec tx[4] = {txA(), txB(), txC(), txD()};
        const char* nm[4] = {"A", "B", "C", "D"};
        Features f[4][3];
        const double rates[3] = {2.4e6, 2.4e6, 2.0e6};
        const double offs[3] = {250e3, 280e3, -310e3};
        const double keys[3] = {0.1, 0.17, 0.08};
        for (int t = 0; t < 4; t++)
            for (int c = 0; c < 3; c++) {
                auto x = synth(tx[t], rates[c], offs[c], 24, {{keys[c], keys[c] + 1.0}}, keys[c] + 1.05, (uint32_t)(10 * t + c + 5));
                f[t][c] = measure(x, rates[c], offs[c], Kind::NFM);
                CHECK(f[t][c].valid, "tx %s capture %d invalid", nm[t], c);
            }
        MatchOptions mo; mo.cfoTolHz = 0.5;
        Library lib;
        for (int t = 0; t < 4; t++) lib.add(nm[t], f[t][0]);
        for (int t = 0; t < 4; t++)
            for (int c = 1; c < 3; c++) {
                auto rk = lib.rank(f[t][c], mo);
                const bool ok = !rk.empty() && lib.items[rk[0].first].name == nm[t];
                CHECK(ok, "tx %s capture %d matched %s", nm[t], c, rk.empty() ? "-" : lib.items[rk[0].first].name.c_str());
                double own = 0, other = 0;
                for (auto& r : rk) { if (lib.items[r.first].name == nm[t]) own = r.second.score; else other = std::max(other, r.second.score); }
                printf("  %s#%d: own %.0f best other %.0f\n", nm[t], c, own, other);
                if (own < 70) { Match mm = compare(f[t][c], f[t][0], mo); for (auto& p : mm.parts) printf("      %s d=%.2f w=%.1f\n", p.name.c_str(), p.d, p.w);
                    for (int q = 0; q < 2; q++) { const Features& ff = q ? f[t][0] : f[t][c]; printf("      spurs%d img %.1f:", q, ff.imageDbc); for (auto& sp : ff.spurs) printf(" %.0f/%.1f", sp.offHz, sp.dbc); printf("\n"); }
                    printf("      cfo %.3f/%.3f rise %.0f/%.0f os %.1f/%.1f chirp %.0f/%.0f settle %.0f/%.0f drift %.2f/%.2f\n", f[t][c].cfoHz, f[t][0].cfoHz, f[t][c].riseUs, f[t][0].riseUs, f[t][c].overshootPct, f[t][0].overshootPct, f[t][c].chirpHz, f[t][0].chirpHz, f[t][c].settleMs, f[t][0].settleMs, f[t][c].driftHzPerS, f[t][0].driftHzPerS); }
                CHECK(own >= 60 && own > other + 20, "tx %s capture %d: own %.0f other %.0f", nm[t], c, own, other);
            }
        // mHz-class resolution: 2 Hz apart is rejected when the allowance is tight
        Spec e = txA(); e.cfo += 2.0;
        auto x = synth(e, R, 250e3, 24, {{0.1, 1.1}}, 1.15, 77);
        Features fe = measure(x, R, 250e3, Kind::NFM);
        MatchOptions tight; tight.cfoTolHz = 0.25;
        CHECK(compare(fe, f[0][0], tight).score < 20, "2 Hz apart still scores %.0f", compare(fe, f[0][0], tight).score);
        CHECK(compare(f[0][1], f[0][0], tight).score > 60, "same tx tight score %.0f", compare(f[0][1], f[0][0], tight).score);
    }
    {   // modulated speech: not carrier-only; a sub-audible repeater tone alone still is
        Spec m = txA(); m.audioDev = 2500; m.hum100 = 40;
        auto x = synth(m, R, 250e3, 28, {{0.1, 1.5}}, 1.55, 3);
        Features f = measure(x, R, 250e3, Kind::NFM);
        CHECK(f.valid && !f.carrierOnly, "speech counted as carrier only (dev %.0f)", f.devRmsHz);
        CHECK(f.devRmsHz > 800 && f.devRmsHz < 3500, "dev rms %.0f", f.devRmsHz);
        CHECK(f.tiltValid, "no tilt");
        CHECK(f.hum[2] > 15 && f.hum[2] < 70, "hum100 %.1f", f.hum[2]);
        CHECK(std::fabs(f.cfoHz - 123.456) < 6, "cfo with speech %.2f", f.cfoHz);
        Spec t = txA(); t.ctcssDev = 600;
        auto y = synth(t, R, 250e3, 28, {{0.1, 1.2}}, 1.25, 4);
        Features g = measure(y, R, 250e3, Kind::NFM, false);
        CHECK(g.valid && g.carrierOnly, "CTCSS-only carrier not carrier-only (dev %.0f)", g.devRmsHz);
        Spec w = txA(); w.audioDev = 3000;
        auto yy = synth(w, R, 250e3, 6, {{0.1, 1.2}}, 1.25, 5);
        Features h = measure(yy, R, 250e3, Kind::NFM, false);
        CHECK(!h.valid || h.snrDb < 12, "weak signal should not give a confident print");
    }
    {   // WFM pilot: frequency error to a few mHz, level, two exciters told apart
        Spec p1; p1.cfo = -420.0; p1.pilotDev = 6750; p1.pilotErr = 0.800; p1.pilotJitterDeg = 0.5; p1.audioDev = 20000; p1.wn = 3000;
        Spec p2 = p1; p2.pilotErr = 0.350; p2.pilotDev = 7100; p2.cfo = -420.0;
        auto x1 = synth(p1, R, -400e3, 40, {{0.1, 1.4}}, 1.45, 11);
        auto x1b = synth(p1, R, -400e3, 40, {{0.12, 1.4}}, 1.45, 12);
        auto x2 = synth(p2, R, -400e3, 40, {{0.1, 1.4}}, 1.45, 13);
        Features f1 = measure(x1, R, -400e3, Kind::WFM, false, 93.9e6), f1b = measure(x1b, R, -400e3, Kind::WFM, false, 93.9e6), f2 = measure(x2, R, -400e3, Kind::WFM, false, 93.9e6);
        CHECK(f1.valid && f1.pilot, "pilot not found");
        CHECK(std::fabs(f1.pilotErrHz - 0.800) < 0.02, "pilot err %.4f", f1.pilotErrHz);
        CHECK(std::fabs(f1.pilotDevHz - 6750) < 300, "pilot dev %.0f", f1.pilotDevHz);
        CHECK(std::fabs(f2.pilotErrHz - 0.350) < 0.02, "pilot2 err %.4f", f2.pilotErrHz);
        MatchOptions mo; mo.cfoTolHz = 8;   // WFM audio moves the carrier estimate by several Hz; the pilot decides
        const double same = compare(f1, f1b, mo).score, diff = compare(f1, f2, mo).score;
        printf("WFM same %.0f different %.0f (pilot err %.4f %.4f)\n", same, diff, f1.pilotErrHz, f2.pilotErrHz);
        CHECK(same > 50 && diff < same - 25, "wfm same %.0f diff %.0f", same, diff);
    }
    {   // AM carrier and modulation
        Spec m; m.cfo = 40.25; m.amDepth = 0.5; m.wn = 4000;
        auto x = synth(m, R, 150e3, 30, {{0.1, 1.2}}, 1.25, 21);
        Features f = measure(x, R, 150e3, Kind::AM, false, 125.0e6);
        CHECK(f.valid && !f.carrierOnly, "AM modulated flagged carrier-only (depth %.1f)", f.devRmsHz);
        CHECK(f.devRmsHz > 15 && f.devRmsHz < 40, "AM depth rms %.1f percent", f.devRmsHz);
        Spec c = m; c.amDepth = 0;
        auto y = synth(c, R, 150e3, 30, {{0.1, 1.2}}, 1.25, 22);
        Features g = measure(y, R, 150e3, Kind::AM, false, 125.0e6);
        CHECK(g.valid && g.carrierOnly, "AM carrier not carrier-only (%.2f)", g.devRmsHz);
        CHECK(std::fabs(g.cfoHz - 40.25) < 0.3, "AM cfo %.3f", g.cfoHz);
    }
    {   // watcher: carrier-only key-ups logged with times, mixed with a speech transmission, matched to the library
        const double r = 1.0e6, off = 120e3;
        Spec A = txA(); A.spurs.clear();
        Spec B = txB(); B.spurs.clear(); B.cfo = 61.0; B.audioDev = 2500;
        const std::vector<Seg> sa = {{0.6, 1.2}, {4.6, 5.6}}, sb = {{2.4, 3.6}};
        auto xa = synth(A, r, off, 24, sa, 6.2, 31);
        auto xb = synth(B, r, off, 24, sb, 6.2, 32);
        // both transmitters in one stream: add the carriers (noise doubles in power, fine)
        for (size_t i = 0; i < xa.size(); i++) xa[i] += xb[i];
        Watcher w(r, off, Kind::NFM, 145.5e6);
        for (size_t i = 0; i < xa.size(); i += 40000) w.feed(xa.data() + i, std::min<size_t>(40000, xa.size() - i), 1000 + (int64_t)(i / r));
        auto ev = w.poll();
        CHECK(ev.size() == 3, "events %zu", ev.size());
        if (ev.size() == 3) {
            const double ts[3] = {0.6, 2.4, 4.6}, ds[3] = {0.6, 1.2, 1.0};
            for (int i = 0; i < 3; i++) {
                CHECK(std::fabs(ev[(size_t)i].tStart - ts[i]) < 0.12, "event %d start %.2f", i, ev[(size_t)i].tStart);
                CHECK(std::fabs(ev[(size_t)i].durationS - ds[i]) < 0.15, "event %d dur %.2f", i, ev[(size_t)i].durationS);
                CHECK(ev[(size_t)i].f.valid, "event %d analysis invalid", i);
            }
            CHECK(ev[0].f.carrierOnly && !ev[1].f.carrierOnly && ev[2].f.carrierOnly, "carrier-only flags %d %d %d", ev[0].f.carrierOnly, ev[1].f.carrierOnly, ev[2].f.carrierOnly);
            MatchOptions mo; mo.cfoTolHz = 1.0;
            const double same = compare(ev[0].f, ev[2].f, mo).score, diff = compare(ev[0].f, ev[1].f, mo).score;
            printf("snr %.1f %.1f %.1f std %.3f %.3f\n", ev[0].f.snrDb, ev[1].f.snrDb, ev[2].f.snrDb, ev[0].f.cfoStdHz, ev[2].f.cfoStdHz); printf("watcher: key-up 1 vs 3 %.0f, vs the speech transmission %.0f (cfo %.3f %.3f %.3f)\n", same, diff, ev[0].f.cfoHz, ev[1].f.cfoHz, ev[2].f.cfoHz);
            CHECK(same > 55 && same > diff + 25, "watcher match same %.0f diff %.0f", same, diff);
            CHECK(std::fabs(ev[0].f.cfoHz - 123.456) < 1.2 && std::fabs(ev[2].f.cfoHz - 123.456) < 1.2, "watcher cfo");
        }
        // a carrier that is simply on: capture it now
        auto xc = synth(A, r, off, 24, {{0.0, 3.0}}, 2.0, 41);
        Watcher w2(r, off, Kind::NFM, 145.5e6);
        w2.feed(xc.data(), xc.size());
        Features fc = w2.captureNow(1.0);
        CHECK(fc.valid && fc.carrierOnly && std::fabs(fc.cfoHz - 123.456) < 0.3, "capture now: valid %d cfo %.3f", fc.valid, fc.cfoHz);
        CHECK(!fc.transient, "no key-up inside an already-on carrier");
    }
    {   // library file round trip and a hostile line
        Library lib;
        auto x = synth(txA(), R, 250e3, 25, {{0.1, 1.2}}, 1.25, 51);
        Features f = measure(x, R, 250e3, Kind::NFM);
        lib.add("Hill | A\nrepeater", f, "note");
        Library l2;
        CHECK(l2.fromText(lib.toText() + "garbage line\n#c\nx|y|1|nokeys\n"), "parse");
        CHECK(l2.items.size() == 1, "items %zu", l2.items.size());
        if (l2.items.size() == 1) CHECK(compare(l2.items[0].f, f).score > 99.0, "round trip score %.2f", compare(l2.items[0].f, f).score);
        Features other = f; other.kind = Kind::AM;
        CHECK(compare(other, f).score == 0, "different kinds must not match");
    }
    printf(fails ? "%d FAILURES\n" : "rf_fingerprint OK\n", fails);
    return fails ? 1 : 0;
}
