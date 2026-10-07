// Analog TV test-signal generator, checked by an analysis that does not use the receiver: the composite video is read from the generator's
// tap and measured against the numbers of BT.470-6 (pulse widths, line and field timing, levels, burst, colour bars), and the radio
// signal is demodulated with an ideal frequency-domain detector and measured against what the standard asks of the transmitted spectrum.
#include "dect2/atv_card.h"
#include "dect2/atv_gen.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static const double kFs = AtvGenerator::kInternalRate;     // 12 Msps: the output at this rate is the internal signal

struct Pulse { double startUs, widthUs; };

// the composite of `lines` lines of frame 0 onward, as the generator made it
static std::vector<float> compositeOf(AtvGenConfig c, double secs) {
    c.rate = kFs;
    AtvGenerator g(c);
    std::vector<float> v;
    g.setCompositeTap([&](const float* x, size_t n) { v.insert(v.end(), x, x + n); });
    std::vector<cf32> sink(1 << 15);
    for (size_t done = 0; done < (size_t)(secs * kFs); done += sink.size()) g.generate(sink.data(), sink.size());
    return v;
}

// edges of the synchronising pulses: the signal is averaged over 1 us (this removes the colour subcarrier), the half-way level is crossed
// with linear interpolation; pulses shorter than 1 us are not pulses
static std::vector<Pulse> findPulses(const std::vector<float>& v, double syncLevel) {
    const int w = 12;      // 1 us
    std::vector<float> s(v.size(), 0.f);
    double acc = 0;
    for (size_t i = 0; i < v.size(); i++) {
        acc += v[i];
        if (i >= (size_t)w) acc -= v[i - w];
        s[i] = (float)(acc / w);
    }
    const double thr = syncLevel / 2;
    std::vector<Pulse> out;
    bool in = false;
    double t0 = 0;
    for (size_t i = w + 1; i < s.size(); i++) {
        const double a = s[i - 1] - (i >= (size_t)w + 1 ? 0 : 0), b = s[i];
        // the box filter delays by w/2 samples: compensate
        if (!in && a > thr && b <= thr) { in = true; t0 = ((double)(i - 1) + (a - thr) / (a - b) - w / 2.0 + 0.5) / kFs * 1e6; }
        else if (in && a < thr && b >= thr) {
            const double t1 = ((double)(i - 1) + (thr - a) / (b - a) - w / 2.0 + 0.5) / kFs * 1e6;
            if (t1 - t0 > 1.0) out.push_back({t0, t1 - t0});
            in = false;
        }
    }
    return out;
}

// average of the composite between two times
static double meanBetween(const std::vector<float>& v, double t0Us, double t1Us) {
    const size_t a = (size_t)std::ceil(t0Us * 1e-6 * kFs), b = (size_t)std::floor(t1Us * 1e-6 * kFs);
    double s = 0;
    for (size_t i = a; i <= b; i++) s += v[i];
    return s / (double)(b - a + 1);
}

static void checkTiming(const AtvFormat& f, const std::vector<float>& v, const char* tag) {
    const auto pulses = findPulses(v, f.syncLevel);
    // widths by class
    int nEq = 0, nBroad = 0, nLine = 0;
    double sumLine = 0, sumEq = 0, sumBroad = 0, sumGapB = 0;
    std::vector<double> lineStarts;
    for (size_t i = 0; i < pulses.size(); i++) {
        const double w = pulses[i].widthUs;
        if (w > 20) { nBroad++; sumBroad += w; }
        else if (w < 3.4) { nEq++; sumEq += w; }
        else { nLine++; sumLine += w; lineStarts.push_back(pulses[i].startUs); }
    }
    printf("%s: %zu pulses: line %d (mean %.3f us), equalising %d (%.3f us), broad %d (%.3f us)\n", tag, pulses.size(), nLine, sumLine / std::max(1, nLine), nEq, sumEq / std::max(1, nEq), nBroad, sumBroad / std::max(1, nBroad));
    (void)sumGapB;
    CHECK(near(sumLine / nLine, f.syncUs, 0.03), "%s: line sync width %.3f us, expected %.2f", tag, sumLine / nLine, f.syncUs);
    CHECK(near(sumEq / nEq, f.eqUs, 0.05), "%s: equalising pulse %.3f us, expected %.2f", tag, sumEq / nEq, f.eqUs);
    CHECK(near(sumBroad / nBroad, f.broadUs, 0.05), "%s: field sync pulse %.3f us, expected %.2f", tag, sumBroad / nBroad, f.broadUs);
    // pulse counts in two frames (the second one is complete): eq pulses 4 x eq, broad 2 x eq a frame
    // line period: consecutive regular pulses
    std::vector<double> iv;
    for (size_t i = 1; i < lineStarts.size(); i++) {
        const double d = lineStarts[i] - lineStarts[i - 1];
        if (std::fabs(d - f.lineUs) < 1.0) iv.push_back(d);
    }
    CHECK(iv.size() > 200, "%s: only %zu regular line intervals", tag, iv.size());
    const double mean = std::accumulate(iv.begin(), iv.end(), 0.0) / iv.size();
    double dev = 0;
    for (double d : iv) dev = std::max(dev, std::fabs(d - mean));
    CHECK(near(mean, f.lineUs, 0.0005) && dev < 0.01, "%s: line period %.5f us (max deviation %.4f), expected %.5f", tag, mean, dev, f.lineUs);
    // field sync: the start of each group of broad pulses; spacing between fields and where it falls on the line grid
    std::vector<double> broadStarts;
    for (size_t i = 0; i < pulses.size(); i++) if (pulses[i].widthUs > 20 && (broadStarts.empty() || pulses[i].startUs - broadStarts.back() > 5 * f.lineUs)) broadStarts.push_back(pulses[i].startUs);
    CHECK(broadStarts.size() >= 4, "%s: %zu field syncs found", tag, broadStarts.size());
    // (the first pulse of the first block starts at t = 0, before the first sample, and is not seen)
    for (size_t i = 2; i < broadStarts.size(); i++)
        CHECK(near(broadStarts[i] - broadStarts[i - 1], f.lineUs * f.lines / 2, 0.01), "%s: fields %.3f us apart, expected %.3f", tag, broadStarts[i] - broadStarts[i - 1], f.lineUs * f.lines / 2);
    // level of the sync tip and of blanking
    const double lvl0 = lineStarts[lineStarts.size() / 2];
    CHECK(near(meanBetween(v, lvl0 + 1.5, lvl0 + 3.2), f.syncLevel, 0.002), "%s: sync tip %.4f, expected %.4f", tag, meanBetween(v, lvl0 + 1.5, lvl0 + 3.2), f.syncLevel);
    CHECK(near(meanBetween(v, lvl0 - 1.0, lvl0 - 0.4), 0.0, 0.002), "%s: front porch level %.4f", tag, meanBetween(v, lvl0 - 1.0, lvl0 - 0.4));
}

// sample index of the start of a line (1 based, in frame 0)
static double lineStartUs(const AtvFormat& f, int line) { return (line - 1) * f.lineUs; }

// amplitude and phase of the subcarrier in a window (Hann weighted): returns the sin and cos components (u, v)
static void subcarrier(const std::vector<float>& v, double fsc, double t0Us, double t1Us, double& us, double& vs, double& mean) {
    const size_t a = (size_t)std::ceil(t0Us * 1e-6 * kFs), b = (size_t)std::floor(t1Us * 1e-6 * kFs);
    double ss = 0, sc = 0, sw = 0, sm = 0;
    for (size_t i = a; i <= b; i++) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - a + 0.5) / (double)(b - a + 1));
        const double ph = 2 * M_PI * fsc * (double)i / kFs;
        ss += w * v[i] * std::sin(ph); sc += w * v[i] * std::cos(ph); sw += w * w; sm += w * v[i];
    }
    // for a sine of amplitude A the sum is A * sum(w^2) / 2 ... for a Hann window sum(w * w) is what the correlation gives
    double sw1 = 0;
    for (size_t i = a; i <= b; i++) sw1 += 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - a + 0.5) / (double)(b - a + 1));
    (void)sw;
    us = 2 * ss / sw1; vs = 2 * sc / sw1; mean = sm / sw1;
}

static void checkColour(const AtvFormat& f, const std::vector<float>& v, const AtvCard& card, const char* tag) {
    int bx0, bx1, by0, by1;
    card.barsArea(bx0, bx1, by0, by1);
    const double pxUs = f.activeUs / f.picW;
    // pick lines of frame 1 so the generator has settled: use frame 0 anyway (it is exact from the first sample)
    int tested = 0, signOk = 0, signBad = 0;
    for (int row = by0 + 8; row < by1 - 8 && tested < 40; row += 7) {
        const int fld = row & 1, k = row >> 1;
        const int line = f.firstLine[fld] + k;
        const double ls = lineStartUs(f, line) + (fld == 1 ? 0 : 0);
        // burst: phase from the sine and cosine components over the burst
        const double bs = ls + f.burstStartUs + 0.4, be = ls + f.burstStartUs + f.burstCycles / f.fscHz * 1e6 - 0.3;
        double bu, bv, bm;
        subcarrier(v, f.fscHz, bs, be, bu, bv, bm);
        double th = std::atan2(bv, bu) * 180 / M_PI;                    // sin(wt + th) = sin cos(th) + cos sin(th)
        const double bamp = std::hypot(bu, bv);
        if (f.pal) {
            CHECK(std::fabs(std::fabs(th) - 135) < 6, "%s line %d: burst phase %.1f degrees, expected +-135", tag, line, th);
            CHECK(near(bamp, f.burstAmp, 0.15 * f.burstAmp), "%s line %d: burst amplitude %.4f, expected %.4f", tag, line, bamp, f.burstAmp);
        } else {
            CHECK(std::fabs(std::fabs(th) - 180) < 6, "%s line %d: burst phase %.1f degrees, expected 180", tag, line, th);
            CHECK(near(bamp, f.burstAmp, 0.15 * f.burstAmp), "%s line %d: burst amplitude %.4f, expected %.4f", tag, line, bamp, f.burstAmp);
        }
        const int vsign = f.pal ? (th > 0 ? 1 : -1) : 1;                // the burst carries the sign of V (Table 2 item 2.9)
        // the bars
        for (int i = 1; i < 7; i++) {
            float rgb[3]; atvEbuBar(i, rgb);
            float y, u, vv;
            atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, vv);
            const int xa = bx0 + (bx1 - bx0) * i / 8, xb = bx0 + (bx1 - bx0) * (i + 1) / 8;
            const double t0 = ls + f.blankEndUs + (xa + 0.25 * (xb - xa)) * pxUs, t1 = ls + f.blankEndUs + (xa + 0.75 * (xb - xa)) * pxUs;
            double su, sv, sm;
            subcarrier(v, f.fscHz, t0, t1, su, sv, sm);
            const double sc = 1 - f.setup;
            CHECK(near(sm, f.setup + sc * y, 0.012), "%s line %d bar %d: luminance %.4f, expected %.4f", tag, line, i, sm, f.setup + sc * y);
            CHECK(near(su, sc * u, 0.02) && near(sv * vsign, sc * vv, 0.02), "%s line %d bar %d: chroma U %.3f V %.3f, expected %.3f %.3f (V sign %d)", tag, line, i, su, sv, sc * u, sc * vv, vsign);
            if (near(sv * vsign, sc * vv, 0.02)) signOk++; else signBad++;
        }
        tested++;
    }
    CHECK(tested > 10 && signBad == 0, "%s: colour bars on %d lines, %d wrong", tag, tested, signBad);
}

// ---- the radio signal
static void idealDemod(const std::vector<cf32>& x, double fs, double fv, double vsb, double bw, std::vector<float>& out, double& phase) {
    const int n = (int)x.size();
    std::vector<cf32> y(x.size());
    double ph = 0;
    for (int i = 0; i < n; i++) {
        ph = std::fmod(2 * M_PI * fv * (double)i / fs, 2 * M_PI);
        y[(size_t)i] = x[(size_t)i] * cf32((float)std::cos(ph), (float)-std::sin(ph));
    }
    Fft fft(n);
    // carrier phase from the DC bin
    std::vector<cf32> Y = y;
    fft.forward(Y.data());
    phase = std::arg(std::complex<double>(Y[0].real(), Y[0].imag()));
    for (int k = 0; k < n; k++) {
        const double f = (k < n / 2 ? k : k - n) * fs / n;
        double h = f >= vsb ? 1 : f <= -vsb ? 0 : 0.5 + 0.5 * std::sin(M_PI * f / (2 * vsb));
        if (std::fabs(f) > bw) h = 0;
        Y[(size_t)k] *= (float)h;
    }
    fft.inverse(Y.data());
    out.resize((size_t)n);
    const cf32 rot((float)std::cos(phase), (float)-std::sin(phase));
    for (int i = 0; i < n; i++) out[(size_t)i] = (Y[(size_t)i] * rot).real() / (float)n;
}

static double bandPower(const std::vector<float>& psd, double fs, double f0, double f1) {      // psd: |X|^2, bin k at k fs / n (fftshifted not used)
    const int n = (int)psd.size();
    double s = 0;
    for (int k = 0; k < n; k++) {
        const double f = (k < n / 2 ? k : k - n) * fs / n;
        if (f >= f0 && f < f1) s += psd[(size_t)k];
    }
    return s;
}


// ---- SECAM: frequency and amplitude of the subcarrier in a stretch of the composite, found on a grid of frequencies (3 us hold about 13 cycles)
static void secamTone(const std::vector<float>& v, double t0Us, double t1Us, double& fHz, double& amp) {
    const size_t a = (size_t)std::ceil(t0Us * 1e-6 * kFs), b = (size_t)std::floor(t1Us * 1e-6 * kFs);
    double mean = 0, sw = 0;
    std::vector<double> w(b - a + 1);
    for (size_t i = a; i <= b; i++) { w[i - a] = 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - a + 0.5) / (double)(b - a + 1)); mean += w[i - a] * v[i]; sw += w[i - a]; }
    mean /= sw;
    double best = -1, bf = 0;
    auto power = [&](double f) {
        double c = 0, s2 = 0;
        for (size_t i = a; i <= b; i++) { const double ph = 2 * M_PI * f * (double)i / kFs; c += w[i - a] * (v[i] - mean) * std::cos(ph); s2 += w[i - a] * (v[i] - mean) * std::sin(ph); }
        return c * c + s2 * s2;
    };
    for (double f = 3.4e6; f <= 5.4e6; f += 10e3) { const double pw = power(f); if (pw > best) { best = pw; bf = f; } }
    for (double f = bf - 10e3; f <= bf + 10e3; f += 1e3) { const double pw = power(f); if (pw > best) { best = pw; bf = f; } }
    fHz = bf;
    amp = 2 * std::sqrt(best) / sw;
}

static void checkSecam(int sys, int ident, const char* tag) {
    AtvGenConfig c; c.sys = sys; c.colour = kAtvSecam; c.cnrDb = 100; c.secamIdent = ident;
    AtvGenerator g(c);
    CHECK(g.ok(), "%s: generator", tag);
    if (!g.ok()) return;
    const AtvFormat& f = g.format();
    const auto v = compositeOf(c, 0.085);
    checkTiming(f, v, tag);
    const double pxUs = f.activeUs / f.picW;
    int bx0, bx1, by0, by1;
    g.card().barsArea(bx0, bx1, by0, by1);
    // lines of the first frame: even absolute line index (line - 1) = D'R
    int nR = 0, nB = 0, bad = 0;
    for (int row = by0 + 10; row < by1 - 10; row += 9) {
        const int fld = row & 1, k = row >> 1, line = f.firstLine[fld] + k;
        const bool isR = ((line - 1) & 1) == 0;
        const double ls = lineStartUs(f, line);
        double fr, am;
        // lead-in: the rest frequency, from 5.6 us on
        secamTone(v, ls + 6.5, ls + 9.7, fr, am);
        if (ident == 2) {
            CHECK(am < 0.01, "%s line %d: chroma %.4f on the back porch with the lines not identified", tag, line, am);
        } else {
            const double f0 = isR ? kSecamF0R : kSecamF0B;
            CHECK(near(fr, f0, 15e3), "%s line %d (%s): lead-in at %.0f Hz, expected %.0f", tag, line, isR ? "D'R" : "D'B", fr, f0);
            const double expAmp = kSecamM0 * std::abs(atvSecamHfPreEmph(f0));
            CHECK(near(am, expAmp, 0.12 * expAmp), "%s line %d: lead-in amplitude %.4f, expected %.4f (M0 times the anti-bell at the rest frequency)", tag, line, am, expAmp);
        }
        // before 5.6 us (the sync pulse and the front of the back porch) and after the active line: no subcarrier (item 2.17: blanked)
        double f2, a2;
        secamTone(v, ls + 0.2, ls + 3.4, f2, a2);
        CHECK(a2 < 0.012, "%s line %d: %.4f of subcarrier during the sync pulse", tag, line, a2);
        // the bars
        for (int i = 1; i < 7; i++) {
            float rgb[3]; atvEbuBar(i, rgb);
            float y, u, vv;
            atvRgbToYuv(rgb[0], rgb[1], rgb[2], y, u, vv);
            const double D = isR ? atvSecamDr(rgb[0], y) : atvSecamDb(rgb[2], y);
            const double fexp = std::min(isR ? kSecamMaxR : kSecamMaxB, std::max(isR ? kSecamMinR : kSecamMinB, (isR ? kSecamF0R : kSecamF0B) + (isR ? kSecamDevR : kSecamDevB) * D));
            const int xa = bx0 + (bx1 - bx0) * i / 8, xb = bx0 + (bx1 - bx0) * (i + 1) / 8;
            const double t0 = ls + f.blankEndUs + (xa + 0.3 * (xb - xa)) * pxUs, t1 = ls + f.blankEndUs + (xa + 0.7 * (xb - xa)) * pxUs;
            double fm, ab;
            secamTone(v, t0, t1, fm, ab);
            const double expAmp = kSecamM0 * std::abs(atvSecamHfPreEmph(fexp));
            if (!near(fm, fexp, 20e3)) bad++;
            CHECK(near(fm, fexp, 20e3), "%s line %d bar %d (%s): subcarrier %.0f Hz, expected %.0f (D' = %.3f)", tag, line, i, isR ? "D'R" : "D'B", fm, fexp, D);
            CHECK(near(ab, expAmp, 0.15 * expAmp + 0.004), "%s line %d bar %d: amplitude %.4f, expected %.4f", tag, line, i, ab, expAmp);
        }
        if (isR) nR++; else nB++;
        if (nR + nB <= 2) printf("%s line %d (%s): lead-in %.0f Hz amplitude %.4f\n", tag, line, isR ? "D'R" : "D'B", fr, am);
    }
    CHECK(nR > 3 && nB > 3 && bad == 0, "%s: %d D'R lines, %d D'B lines, %d wrong frequencies", tag, nR, nB, bad);
    // the nine lines of field identification: the trapezoid ends at +350 kHz (D'R lines) or -350 kHz (D'B lines)
    int nb = 0;
    for (int line : {7, 8, 9, 10, 11, 12, 13, 14, 15, 320, 321, 322, 323, 324, 325, 326, 327, 328, 16, 17, 22, 6}) {
        const bool isR = ((line - 1) & 1) == 0, bottle = f.secamBottleLine(line);
        const double ls = lineStartUs(f, line);
        double fr, am;
        secamTone(v, ls + 40, ls + 55, fr, am);
        if (bottle && ident != 1) {
            const double fexp = isR ? kSecamF0R + 350e3 : kSecamF0B - 350e3;
            CHECK(near(fr, fexp, 15e3), "%s line %d (%s): identification signal ends at %.0f Hz, expected %.0f", tag, line, isR ? "D'R" : "D'B", fr, fexp);
            nb++;
        } else {
            CHECK(am < 0.012, "%s line %d: %.4f of subcarrier on a line of the field blanking without identification", tag, line, am);
        }
    }
    if (ident != 1) CHECK(nb == 18, "%s: %d identification lines checked", tag, nb);
}

int main() {
    // ---- composite video of the B/G PAL signal
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 100;
        AtvGenerator g(c);
        CHECK(g.ok(), "generator B/G PAL");
        const auto v = compositeOf(c, 0.085);
        checkTiming(g.format(), v, "B/G PAL");
        checkColour(g.format(), v, g.card(), "B/G PAL");
    }
    // ---- NTSC M
    {
        AtvGenConfig c; c.sys = kAtvM; c.colour = kAtvNtsc; c.cnrDb = 100;
        AtvGenerator g(c);
        CHECK(g.ok(), "generator M NTSC");
        const auto v = compositeOf(c, 0.072);
        checkTiming(g.format(), v, "M NTSC");
        checkColour(g.format(), v, g.card(), "M NTSC");
        // black level: setup 7.5 IRE on the black bar
        int bx0, bx1, by0, by1; g.card().barsArea(bx0, bx1, by0, by1);
        const AtvFormat& f = g.format();
        const int row = by0 + 10, line = f.firstLine[row & 1] + (row >> 1);
        const double ls = lineStartUs(f, line), pxUs = f.activeUs / f.picW;
        const int xa = bx0 + (bx1 - bx0) * 7 / 8, xb = bx0 + (bx1 - bx0) * 8 / 8;
        CHECK(near(meanBetween(v, ls + f.blankEndUs + (xa + 0.2 * (xb - xa)) * pxUs, ls + f.blankEndUs + (xa + 0.8 * (xb - xa)) * pxUs), 0.075, 0.004), "NTSC black bar at 7.5 IRE");
    }
    // ---- other systems and colour options: they build, with the right numbers
    for (int sys : {kAtvB, kAtvI, kAtvDK, kAtvN}) {
        AtvGenConfig c; c.sys = sys; c.colour = kAtvPal; c.cnrDb = 100;
        AtvGenerator g(c);
        CHECK(g.ok(), "generator system %d", sys);
        if (!g.ok()) continue;
        const auto v = compositeOf(c, 0.085);
        checkTiming(g.format(), v, g.format().name.c_str());
        checkColour(g.format(), v, g.card(), g.format().name.c_str());
    }
    { AtvGenConfig c; c.sys = kAtvM; c.colour = kAtvPal; AtvGenerator g(c); CHECK(g.ok(), "PAL-M"); if (g.ok()) { const auto v = compositeOf(c, 0.072); checkTiming(g.format(), v, "PAL-M"); checkColour(g.format(), v, g.card(), "PAL-M"); } }
    { AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvMono; AtvGenerator g(c); CHECK(g.ok(), "monochrome"); }
    { AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvNtsc; AtvGenerator g(c); CHECK(!g.ok(), "NTSC on a 625-line system is refused"); }

    // ---- SECAM
    checkSecam(kAtvG, 0, "SECAM B/G");
    checkSecam(kAtvDK, 0, "SECAM D/K");
    checkSecam(kAtvG, 1, "SECAM lines only");
    checkSecam(kAtvG, 2, "SECAM field only");
    { AtvGenConfig c; c.sys = kAtvM; c.colour = kAtvSecam; AtvGenerator g(c); CHECK(!g.ok(), "SECAM on M is refused"); }

    // ---- the radio signal: modulation depth, vestigial sideband, sound, noise, offsets
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 100; c.pattern = 0;
        c.rate = kFs;
        const int n = 1 << 20;
        AtvGenerator g(c);
        std::vector<cf32> x;
        g.generate(n, x);
        double rms = 0, pk = 0;
        for (const auto& s : x) { rms += std::norm(s); pk = std::max(pk, (double)std::abs(s)); }
        rms = std::sqrt(rms / n);
        printf("radio signal: rms %.3f, peak %.3f\n", rms, pk);
        CHECK(rms > 0.15 && rms < 0.30 && pk < 0.9, "signal level rms %.3f peak %.3f", rms, pk);
        const AtvFormat& f = g.format();
        std::vector<float> v; double ph;
        idealDemod(x, kFs, atvVisionOffsetHz(f), 0.75e6, 4.9e6, v, ph);
        // carrier amplitude: sync tip = level, blanking = level * (1 - 0.875 * 0.3) ; the detector halves it (Nyquist slope)
        std::vector<double> top;
        for (size_t i = 2000; i + 2000 < v.size(); i += 1) if (v[i] > 0.19) top.push_back(v[i]);
        std::sort(top.begin(), top.end());
        const double tip = top.empty() ? 0 : top[top.size() / 2];
        printf("ideal detector: sync tip %.4f (expected %.4f)\n", tip, 0.5 * c.level);
        CHECK(near(tip, 0.5 * c.level, 0.004), "sync tip after the detector %.4f, expected %.4f", tip, 0.5 * c.level);
        // the transmitted spectrum: the vestigial sideband is at least 20 dB down 1.5 MHz below the carrier (BT.470-6 Table 3 item 6: 20 dB at -1.25 MHz)
        Fft fft(n);
        std::vector<cf32> X = x;
        for (int i = 0; i < n; i++) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n); X[(size_t)i] *= (float)w; }
        fft.forward(X.data());
        std::vector<float> psd((size_t)n);
        for (int k = 0; k < n; k++) psd[(size_t)k] = std::norm(X[(size_t)k]);
        const double fv = atvVisionOffsetHz(f);
        const double upper = bandPower(psd, kFs, fv + 1.5e6, fv + 2.0e6), lower = bandPower(psd, kFs, fv - 2.0e6, fv - 1.5e6);
        const double up2 = bandPower(psd, kFs, fv + 0.3e6, fv + 0.6e6), lo2 = bandPower(psd, kFs, fv - 0.6e6, fv - 0.3e6);
        printf("sidebands: +1.5..2 MHz %.1f dB, -1.5..2 MHz %.1f dB below; +-0.3..0.6 MHz differ by %.2f dB\n", 0.0, 10 * std::log10(lower / upper), 10 * std::log10(lo2 / up2));
        CHECK(10 * std::log10(lower / upper) < -20, "vestigial sideband only %.1f dB down", 10 * std::log10(lower / upper));
        CHECK(std::fabs(10 * std::log10(lo2 / up2)) < 1.0, "below 0.75 MHz both sidebands are sent: %.2f dB apart", 10 * std::log10(lo2 / up2));
        // sound: power of the carrier against the peak vision carrier (-13 dB), deviation of the tone
        const double fsn = atvSoundOffsetHz(f);
        const double pSound = bandPower(psd, kFs, fsn - 150e3, fsn + 150e3);
        // |X|^2 of a Hann windowed complex tone of amplitude A: A^2 (sum w)^2 ; for the whole band the integral of the PSD scales with sum(w^2) * n * A^2
        double sw2 = 0;
        for (int i = 0; i < n; i++) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n); sw2 += w * w; }
        const double aSound = std::sqrt(pSound / ((double)n * sw2));
        const double expect = c.level * std::pow(10.0, f.soundRelDb / 20);
        printf("sound carrier amplitude %.4f (expected %.4f)\n", aSound, expect);
        CHECK(near(20 * std::log10(aSound / expect), 0, 0.7), "sound carrier level %.4f, expected %.4f", aSound, expect);
        // FM deviation: mix to zero, band-limit, differentiate the phase
        {
            std::vector<cf32> s(x.size());
            for (int i = 0; i < n; i++) { const double p = std::fmod(2 * M_PI * fsn * (double)i / kFs, 2 * M_PI); s[(size_t)i] = x[(size_t)i] * cf32((float)std::cos(p), (float)-std::sin(p)); }
            fft.forward(s.data());
            for (int k = 0; k < n; k++) { const double ff = (k < n / 2 ? k : k - n) * kFs / n; if (std::fabs(ff) > 120e3) s[(size_t)k] = 0; }
            fft.inverse(s.data());
            // the first 0.5 s is not available: measure the peak deviation and the tone frequency over the 87 ms
            double devMax = 0, devMin = 0;
            std::vector<double> inst;
            for (int i = 2000; i + 2000 < n; i += 8) {
                const cf32 a = s[(size_t)i - 8] / (float)n, b = s[(size_t)i] / (float)n;
                const double d = std::arg(std::complex<double>(b.real(), b.imag()) * std::conj(std::complex<double>(a.real(), a.imag()))) * kFs / 8 / (2 * M_PI);
                inst.push_back(d);
            }
            // a 1 kHz tone: remove the mean (offset) and measure the peak
            double mean = 0;
            for (double d : inst) mean += d;
            mean /= (double)inst.size();
            for (double d : inst) { devMax = std::max(devMax, d - mean); devMin = std::min(devMin, d - mean); }
            const double g50 = std::sqrt(1 + std::pow(2 * M_PI * 1000 * f.preEmphUs * 1e-6, 2));
            const double expDev = f.soundDevKhz * 1e3 * 0.5 * g50;
            printf("sound deviation +%.0f / %.0f Hz (expected +-%.0f), carrier %.1f Hz off\n", devMax, devMin, expDev, mean);
            CHECK(near(devMax, expDev, 0.06 * expDev) && near(-devMin, expDev, 0.06 * expDev), "FM deviation +%.0f / %.0f Hz, expected +-%.0f", devMax, devMin, expDev);
            CHECK(std::fabs(mean) < 150, "sound carrier %.1f Hz off its place", mean);
        }
    }
    // ---- noise: the carrier-to-noise ratio is what was asked for
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 25; c.rate = 12e6; c.sound = 3;
        const int n = 1 << 20;
        AtvGenerator g(c);
        std::vector<cf32> x;
        g.generate(n, x);
        Fft fft(n);
        std::vector<cf32> X = x;
        double sw2 = 0;
        for (int i = 0; i < n; i++) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n); X[(size_t)i] *= (float)w; sw2 += w * w; }
        fft.forward(X.data());
        std::vector<float> psd((size_t)n);
        for (int k = 0; k < n; k++) psd[(size_t)k] = std::norm(X[(size_t)k]);
        // an empty part of the band: 4 to 5.5 MHz above the centre (the sound is off)
        const double p = bandPower(psd, 12e6, 3.9e6, 5.4e6) / ((double)n * sw2) / 1.5e6 * 5e6;      // mean power in 5 MHz
        const double cnr = 10 * std::log10(c.level * c.level / p);
        printf("noise: C/N measured %.2f dB, asked 25\n", cnr);
        CHECK(near(cnr, 25, 0.5), "carrier-to-noise ratio %.2f dB, asked 25", cnr);
    }
    // ---- carrier offset and clock error
    {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 100; c.rate = 10e6; c.cfoHz = 7300; c.sroPpm = 100; c.sound = 3;
        const int n = 1 << 20;
        AtvGenerator g(c);
        std::vector<cf32> x;
        g.generate(n, x);
        Fft fft(n);
        std::vector<cf32> X = x;
        for (int i = 0; i < n; i++) X[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / n));
        fft.forward(X.data());
        int best = -1;
        for (int k = 0; k < n; k++) { const double f = (k < n / 2 ? k : k - n) * 10e6 / n; if (f < -3.5e6 || f > -2.0e6) continue; if (best < 0 || std::norm(X[(size_t)k]) > std::norm(X[(size_t)best])) best = k; }
        const double a = std::abs(X[(size_t)best - 1]), b = std::abs(X[(size_t)best]), d = std::abs(X[(size_t)best + 1]);
        const double delta = 0.5 * (a - d) / (a - 2 * b + d);
        const double fpk = ((best < n / 2 ? best : best - n) + delta) * 10e6 / n;
        const double expect = (-2.75e6 + 7300) / (1 + 100e-6);
        printf("carrier at %.1f Hz, expected %.1f\n", fpk, expect);
        CHECK(near(fpk, expect, 40), "carrier at %.1f Hz, expected %.1f", fpk, expect);
    }
    // ---- sample rates: the same signal at the rates a radio offers
    for (double rate : {8e6, 10e6, 12.5e6, 16e6, 20e6}) {
        AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 40; c.rate = rate;
        AtvGenerator g(c);
        std::vector<cf32> x;
        g.generate((size_t)(0.05 * rate), x);
        double r = 0, pk = 0;
        for (const auto& s : x) { r += std::norm(s); pk = std::max(pk, (double)std::abs(s)); }
        r = std::sqrt(r / x.size());
        CHECK(r > 0.15 && r < 0.30 && pk < 0.9, "rate %.1f Msps: rms %.3f peak %.3f", rate / 1e6, r, pk);
    }
    // ---- ghost: the echo is the signal delayed and scaled
    {
        AtvGenConfig a; a.sys = kAtvG; a.colour = kAtvPal; a.cnrDb = 100; a.rate = 10e6;
        AtvGenConfig b = a; b.echoDb = 12; b.echoDelayUs = 2.0; b.echoPhaseDeg = 40;
        AtvGenerator ga(a), gb(b);
        std::vector<cf32> xa, xb;
        ga.generate(100000, xa); gb.generate(100000, xb);
        const cf32 gg = std::polar((float)std::pow(10.0, -12.0 / 20), (float)(40 * M_PI / 180));
        double e = 0, p = 0;
        for (size_t i = 20; i < xa.size(); i++) { e += std::norm(xb[i] - xa[i] - gg * xa[i - 20]); p += std::norm(xa[i]); }
        CHECK(std::sqrt(e / p) < 1e-3, "ghost: 12 dB, 2 us, 40 degrees: error %.5f", std::sqrt(e / p));
    }
    // ---- live changes, bad options, determinism
    {
        SynthConfig s;
        s.mode = 10; s.snrDb = 30;
        auto m = makeAtvSynth(s, 10e6);
        CHECK(m != nullptr, "makeAtvSynth with the default options");
        if (m) {
            std::vector<cf32> x(1 << 16);
            m->generate(x.data(), x.size());
            s.snrDb = 15; s.cfoHz = 3000;
            CHECK(m->configure(s), "noise and carrier offset change while it plays");
            s.modeOpt[0] = 4;
            CHECK(!m->configure(s), "a change of system needs a new generator");
        }
        s.modeOpt[0] = 9;
        CHECK(makeAtvSynth(s, 10e6) == nullptr, "an invalid system gives no signal");
        AtvGenConfig a1, a2; a1.cnrDb = 20; a1.rate = 10e6; a2 = a1;
        AtvGenerator g1(a1), g2(a2);
        std::vector<cf32> x1, x2;
        g1.generate(50000, x1); g2.generate(50000, x2);
        bool same = true;
        for (size_t i = 0; i < x1.size(); i++) same &= x1[i] == x2[i];
        CHECK(same, "same configuration, same samples");
        // chunk sizes do not matter
        AtvGenerator g3(a1);
        std::vector<cf32> x3;
        size_t done = 0;
        const size_t sizes[] = {1, 7, 4096, 333, 65536, 12345};
        for (size_t k = 0; done < 50000; k++) { const size_t m = std::min<size_t>(sizes[k % 6], 50000 - done); g3.generate(m, x3); done += m; }
        same = true;
        for (size_t i = 0; i < x1.size(); i++) same &= x1[i] == x3[i];
        CHECK(same, "chunk sizes do not change the signal");
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
