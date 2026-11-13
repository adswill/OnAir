// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Objective evaluator and synthetic test speech for the KAAL voice codecs (see kaal_voice_eval.h). The synthesiser uses the codec's
// own deterministic math so the test signals (and the golden digests built on them) are the same on every platform.
#include "dect2/kaalvoice/kaal_voice_eval.h"
#include "kv_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace kaalvoice {
namespace eval {

using namespace detail;

namespace {

// ---------------- formant synthesiser ----------------
struct Phone {
    char id;
    double F[5], B[5];
    double av;          // voicing amplitude
    double an;          // frication amplitude
    double fc, fb;      // frication resonance (centre, bandwidth); fb = 0 -> broadband
    double asp;         // aspiration amplitude
};
// Adult male targets (Hz). Consonant formants are the loci the transitions move towards.
const Phone kPhones[] = {
    {'a', {730, 1090, 2440, 3300, 3750}, {80, 90, 120, 200, 250}, 1.0, 0, 0, 0, 0.02},
    {'e', {530, 1840, 2480, 3300, 3750}, {60, 100, 140, 200, 250}, 1.0, 0, 0, 0, 0.02},
    {'i', {270, 2290, 3010, 3500, 3900}, {50, 100, 150, 200, 250}, 0.85, 0, 0, 0, 0.02},
    {'o', {570, 840, 2410, 3300, 3750}, {70, 80, 130, 200, 250}, 1.0, 0, 0, 0, 0.02},
    {'u', {300, 870, 2240, 3300, 3750}, {60, 80, 120, 200, 250}, 0.85, 0, 0, 0, 0.02},
    {'A', {660, 1720, 2410, 3300, 3750}, {80, 100, 140, 200, 250}, 1.0, 0, 0, 0, 0.02},
    {'U', {640, 1190, 2390, 3300, 3750}, {80, 90, 130, 200, 250}, 1.0, 0, 0, 0, 0.02},
    {'R', {490, 1350, 1690, 3300, 3750}, {70, 100, 110, 200, 250}, 0.95, 0, 0, 0, 0.02},
    {'I', {390, 1990, 2550, 3300, 3750}, {60, 100, 140, 200, 250}, 0.9, 0, 0, 0, 0.02},
    {'m', {250, 1100, 2200, 3300, 3750}, {60, 200, 300, 300, 300}, 0.35, 0, 0, 0, 0.0},
    {'n', {250, 1600, 2600, 3300, 3750}, {60, 200, 300, 300, 300}, 0.35, 0, 0, 0, 0.0},
    {'l', {360, 1300, 2700, 3300, 3750}, {60, 120, 200, 250, 250}, 0.6, 0, 0, 0, 0.0},
    {'w', {300, 700, 2200, 3300, 3750}, {60, 80, 150, 250, 250}, 0.6, 0, 0, 0, 0.0},
    {'s', {400, 1700, 2600, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.22, 6000, 2500, 0.0},
    {'S', {400, 1800, 2400, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.28, 3000, 1500, 0.0},
    {'f', {400, 1100, 2400, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.06, 0, 0, 0.0},
    {'z', {300, 1700, 2600, 3300, 3750}, {100, 200, 250, 300, 300}, 0.3, 0.12, 6000, 2500, 0.0},
    {'h', {500, 1500, 2500, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.0, 0, 0, 0.12},
    {'t', {400, 1800, 2600, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.25, 4500, 3000, 0.05},
    {'k', {400, 1600, 2300, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.25, 2200, 1200, 0.05},
    {'p', {400, 900, 2300, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.18, 1200, 2000, 0.05},
    {'_', {500, 1500, 2500, 3300, 3750}, {200, 200, 250, 300, 300}, 0.0, 0.0, 0, 0, 0.0},
};
const Phone& phone(char c) {
    for (const Phone& p : kPhones)
        if (p.id == c) return p;
    return kPhones[sizeof(kPhones) / sizeof(kPhones[0]) - 1];
}

struct Reso {   // two-pole resonator, unity gain at DC
    double y1 = 0, y2 = 0;
    double run(double x, double F, double B, double fs) {
        const double r = kvExp(-kPi * B / fs), c = 2.0 * r * kvCos(2.0 * kPi * F / fs);
        const double y = x * (1.0 - c + r * r) + c * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};
struct Reso2 {  // band-pass (peak gain about 1)
    double y1 = 0, y2 = 0, x1 = 0;
    double run(double x, double F, double B, double fs) {
        const double r = kvExp(-kPi * B / fs), c = 2.0 * r * kvCos(2.0 * kPi * F / fs);
        const double y = (1.0 - r) * (x - x1) + c * y1 - r * r * y2;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

struct Voice { double f0, scale; };
const Voice kVoices[4] = {{112, 1.0}, {205, 1.16}, {275, 1.26}, {88, 0.95}};

struct Track { double F[5], B[5], av, an, fc, fb, asp, f0; };

// Renders a phone string (with per-phone durations in ms) at 16 kHz.
std::vector<double> render(const std::vector<std::pair<char, double>>& seq, const Voice& v, uint32_t seed, double f0Override) {
    const double fs = 16000.0;
    double total = 0;
    for (auto& s : seq) total += s.second;
    const int nms = (int)total + 1;
    std::vector<Track> tr((size_t)nms);
    // 1 ms targets
    int ms = 0;
    double t0 = 0;
    for (size_t k = 0; k < seq.size(); k++) {
        const Phone& p = phone(seq[k].first);
        const int n = (int)seq[k].second;
        for (int i = 0; i < n && ms < nms; i++, ms++) {
            Track& t = tr[(size_t)ms];
            for (int j = 0; j < 5; j++) { t.F[j] = p.F[j] * v.scale; t.B[j] = p.B[j] * (0.9 + 0.1 * v.scale); }
            t.av = p.av; t.an = p.an; t.fc = p.fc; t.fb = p.fb; t.asp = p.asp;
            // stops: closure (silence) then a 12 ms burst at the end
            if (p.id == 't' || p.id == 'k' || p.id == 'p') { if (i < n - 14) { t.an = 0; t.asp = 0; } }
            const double u = (t0 + i) / total;   // declination + a bump per phone
            const double bump = 1.0 + 0.06 * kvSin(kPi * (double)i / (double)n);
            t.f0 = (f0Override > 0 ? f0Override : v.f0 * (1.12 - 0.25 * u) * bump);
        }
        t0 += n;
    }
    for (; ms < nms; ms++) tr[(size_t)ms] = tr[(size_t)ms - 1];
    // smoothing: formants 25 ms, amplitudes 6 ms (moving averages)
    auto smooth = [&](auto get, int w) {
        std::vector<double> src((size_t)nms), dst((size_t)nms);
        for (int i = 0; i < nms; i++) src[(size_t)i] = get(tr[(size_t)i]);
        for (int i = 0; i < nms; i++) {
            double s = 0;
            int c = 0;
            for (int j = i - w / 2; j <= i + w / 2; j++) {
                const int jj = j < 0 ? 0 : (j >= nms ? nms - 1 : j);
                s += src[(size_t)jj];
                c++;
            }
            dst[(size_t)i] = s / c;
        }
        return dst;
    };
    std::vector<double> F[5], B[5];
    for (int j = 0; j < 5; j++) {
        F[j] = smooth([j](const Track& t) { return t.F[j]; }, 25);
        B[j] = smooth([j](const Track& t) { return t.B[j]; }, 25);
    }
    const auto av = smooth([](const Track& t) { return t.av; }, 8), an = smooth([](const Track& t) { return t.an; }, 6),
               asp = smooth([](const Track& t) { return t.asp; }, 8), f0 = smooth([](const Track& t) { return t.f0; }, 30);
    const int n = (int)(total * 16.0);
    std::vector<double> out((size_t)n);
    Rng rng;
    rng.s = seed * 2654435761u + 12345u;
    Reso vt[5];
    Reso2 fr1, fr2;
    double phase = 0, curF0 = f0[0], amp = 1.0, gPrev = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        const int m = i / 16;
        const double a = (i % 16) / 16.0;
        const int m1 = m + 1 < nms ? m + 1 : m;
        auto at = [&](const std::vector<double>& x) { return x[(size_t)m] * (1 - a) + x[(size_t)m1] * a; };
        phase += curF0 / fs;
        if (phase >= 1.0) {
            phase -= 1.0;
            curF0 = at(f0) * (1.0 + 0.006 * rng.gauss());
            amp = 1.0 + 0.04 * rng.gauss();
        }
        // glottal flow (Rosenberg-like), differentiated = lip radiation
        double g = 0;
        if (phase < 0.36) g = 0.5 * (1.0 - kvCos(kPi * phase / 0.36));
        else if (phase < 0.60) g = kvCos(0.5 * kPi * (phase - 0.36) / 0.24);
        const double dg = (g - gPrev) * amp;
        gPrev = g;
        const double nz = rng.gauss();
        double src = at(av) * dg * 6.0 + at(asp) * nz * (phase < 0.6 ? 1.0 : 0.4);
        for (int j = 0; j < 5; j++) {
            const double Fj = at(F[j]);
            if (Fj < 0.47 * fs) src = vt[j].run(src, Fj, at(B[j]), fs);
        }
        double fri = 0;
        const double ann = at(an);
        if (ann > 1e-4) {
            const Track& t = tr[(size_t)m];
            const double nz2 = rng.gauss();
            if (t.fb > 0) fri = fr1.run(nz2, t.fc * (0.95 + 0.05 * v.scale), t.fb, fs) * 2.5 + fr2.run(nz2, t.fc * 1.4 > 7600 ? 7600 : t.fc * 1.4, t.fb, fs);
            else { lp = 0.7 * lp + 0.3 * nz2; fri = nz2 - 0.5 * lp; }
            fri *= ann;
        }
        out[(size_t)i] = src + fri;
    }
    return out;
}

std::vector<int16_t> toPcm(const std::vector<double>& x, double rmsTarget) {
    double s = 0;
    size_t c = 0;
    for (double v : x) { if (std::fabs(v) > 1e-9) { s += v * v; c++; } }
    const double rms = c ? std::sqrt(s / (double)c) : 1.0;
    const double g = rms > 0 ? rmsTarget / rms : 1.0;
    std::vector<int16_t> y(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        double v = x[i] * g;
        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        y[i] = (int16_t)std::lround(v);
    }
    return y;
}

void addNoise(std::vector<int16_t>& x, double noiseDb, uint32_t seed) {
    double s = 0;
    size_t c = 0;
    for (int16_t v : x) { if (v) { s += (double)v * v; c++; } }
    const double rms = c ? std::sqrt(s / (double)c) : 1000.0;
    const double nrms = rms * kvPow10(noiseDb / 20.0);
    Rng r;
    r.s = seed ^ 0x9e3779b9u;
    double lp = 0;
    for (auto& v : x) {
        const double w = r.gauss();
        lp = 0.92 * lp + 0.08 * w;   // pink-ish: low-pass plus white
        const double nz = (3.0 * lp + 0.35 * w) * nrms / 0.85;
        double y = v + nz;
        y = y > 32767 ? 32767 : (y < -32768 ? -32768 : y);
        v = (int16_t)std::lround(y);
    }
}

const float* halfTaps() {   // 63-tap low-pass at 3.8 kHz for 16 kHz
    static float h[63];
    static bool init = false;
    if (!init) {
        double s = 0, hd[63];
        for (int k = 0; k < 63; k++) {
            const double t = k - 31;
            const double fc = 3800.0 / 16000.0;
            const double sinc = t == 0 ? 2 * fc : kvSin(2 * kPi * fc * t) / (kPi * t);
            const double w = 0.42 - 0.5 * kvCos(2 * kPi * k / 62.0) + 0.08 * kvCos(4 * kPi * k / 62.0);
            hd[k] = sinc * w;
            s += hd[k];
        }
        for (int k = 0; k < 63; k++) h[k] = (float)(hd[k] / s);
        init = true;
    }
    return h;
}

}  // namespace

std::vector<int16_t> upsample2(const std::vector<int16_t>& x) {
    const float* h = halfTaps();
    const size_t n = x.size() * 2;
    std::vector<int16_t> y(n);
    for (size_t i = 0; i < n; i++) {
        double s = 0;
        for (int k = 0; k < 63; k++) {
            const long j = (long)i + 31 - k;   // zero-phase
            if (j < 0 || j >= (long)n || (j & 1)) continue;
            s += h[k] * (double)x[(size_t)j / 2];
        }
        s *= 2.0;
        s = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
        y[i] = (int16_t)std::lround(s);
    }
    return y;
}

std::vector<int16_t> downsample2(const std::vector<int16_t>& x) {
    const float* h = halfTaps();
    const size_t n = x.size() / 2;
    std::vector<int16_t> y(n);
    for (size_t m = 0; m < n; m++) {
        double s = 0;
        for (int k = 0; k < 63; k++) {
            const long j = (long)(2 * m) + 31 - k;
            if (j < 0 || j >= (long)x.size()) continue;
            s += h[k] * (double)x[(size_t)j];
        }
        s = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
        y[m] = (int16_t)std::lround(s);
    }
    return y;
}

std::vector<int16_t> synthSpeech(int fs, int voice, double seconds, uint32_t seed, double noiseDb) {
    if (fs == 8000) return downsample2(synthSpeech(16000, voice, seconds, seed, noiseDb));
    const Voice& v = kVoices[voice & 3];
    Rng r;
    r.s = seed * 747796405u + 2891336453u;
    const char vowels[] = "aeiouAURI";
    const char cons[] = "smnlwSfzhtkp";
    std::vector<std::pair<char, double>> seq;
    seq.push_back({'_', 120});
    double t = 120;
    const double total = seconds * 1000.0;
    while (t < total - 300) {
        // a syllable: optional consonant, vowel, optional coda, sometimes a pause
        if ((r.next() >> 8) % 100 < 80) {
            const char c = cons[(r.next() >> 8) % 12];
            const double d = (c == 't' || c == 'k' || c == 'p') ? 60 : (c == 's' || c == 'S' || c == 'f' || c == 'z') ? 70 + (r.next() >> 8) % 60 : 50 + (r.next() >> 8) % 30;
            seq.push_back({c, d});
            t += d;
        }
        const char vw = vowels[(r.next() >> 8) % 9];
        const double dv = 90 + (r.next() >> 8) % 120;
        seq.push_back({vw, dv});
        t += dv;
        if ((r.next() >> 8) % 100 < 30) {
            const char c = cons[(r.next() >> 8) % 12];
            seq.push_back({c, 60});
            t += 60;
        }
        if ((r.next() >> 8) % 100 < 18) {
            const double dp = 150 + (r.next() >> 8) % 200;
            seq.push_back({'_', dp});
            t += dp;
        }
    }
    seq.push_back({'_', total - t > 0 ? total - t : 10});
    std::vector<int16_t> y = toPcm(render(seq, v, seed, 0), 3000.0);
    y.resize((size_t)(seconds * 16000));
    if (noiseDb > -150) addNoise(y, noiseDb, seed);
    return y;
}

std::vector<int16_t> synthSound(int fs, char sound, double f0, double seconds, uint32_t seed) {
    if (fs == 8000) return downsample2(synthSound(16000, sound, f0, seconds, seed));
    std::vector<std::pair<char, double>> seq = {{'_', 20}, {sound, seconds * 1000.0 - 40}, {'_', 20}};
    std::vector<int16_t> y = toPcm(render(seq, kVoices[0], seed, f0), 3000.0);
    y.resize((size_t)(seconds * 16000));
    return y;
}

// ---------------- metrics ----------------
Metrics compare(const std::vector<int16_t>& ref, const std::vector<int16_t>& dec, int fs, int delay, int maxSearch) {
    Metrics m;
    if (ref.empty() || dec.empty()) return m;
    if (delay < 0) {
        double best = -1e300;
        int bd = 0;
        const size_t n = ref.size();
        for (int d = 0; d <= maxSearch; d++) {
            double s = 0, e = 0;
            for (size_t i = 0; i + d < dec.size() && i < n; i += 2) {
                s += (double)ref[i] * dec[i + d];
                e += (double)dec[i + d] * dec[i + d];
            }
            const double c = e > 0 ? s / std::sqrt(e) : -1e300;
            if (c > best) { best = c; bd = d; }
        }
        delay = bd;
    }
    m.delay = delay;
    const size_t n = std::min(ref.size(), dec.size() > (size_t)delay ? dec.size() - delay : 0);
    if (n < (size_t)fs / 10) return m;
    auto r = [&](size_t i) { return (double)ref[i]; };
    auto y = [&](size_t i) { return (double)dec[i + delay]; };
    double sr = 0, se = 0;
    for (size_t i = 0; i < n; i++) { sr += r(i) * r(i); se += (r(i) - y(i)) * (r(i) - y(i)); }
    m.snr = 10.0 * std::log10((sr + 1e-9) / (se + 1e-9));
    // segmental SNR on 20 ms frames
    const int L = fs / 50;
    double emax = 0;
    for (size_t f = 0; (f + 1) * L <= n; f++) {
        double e = 0;
        for (int i = 0; i < L; i++) e += r(f * L + i) * r(f * L + i);
        emax = std::max(emax, e);
    }
    double ss = 0;
    int sc = 0;
    for (size_t f = 0; (f + 1) * L <= n; f++) {
        double e = 0, d = 0;
        for (int i = 0; i < L; i++) { e += r(f * L + i) * r(f * L + i); d += (r(f * L + i) - y(f * L + i)) * (r(f * L + i) - y(f * L + i)); }
        if (e < emax * 1e-4 || e < 1e3 * L) continue;
        double v = 10.0 * std::log10((e + 1e-9) / (d + 1e-9));
        ss += std::clamp(v, -10.0, 35.0);
        sc++;
    }
    m.segSnr = sc ? ss / sc : 0;
    m.snrLow = m.snr;
    m.segSnrLow = m.segSnr;
    if (fs == 16000) {
        const std::vector<int16_t> r8 = downsample2(std::vector<int16_t>(ref.begin(), ref.begin() + n));
        const std::vector<int16_t> y8 = downsample2(std::vector<int16_t>(dec.begin() + delay, dec.begin() + delay + n));
        const Metrics l = compare(r8, y8, 8000, 0);
        m.snrLow = l.snr;
        m.segSnrLow = l.segSnr;
    }
    // spectral measures, 32 ms Hann frames, hop 16 ms
    const int N = fs == 8000 ? 256 : 512, hop = N / 2;
    std::vector<float> win((size_t)N);
    for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
    std::vector<std::vector<double>> PR, PY;
    std::vector<double> fe;
    std::vector<float> re((size_t)N), im((size_t)N);
    for (size_t s = 0; s + N <= n; s += hop) {
        std::vector<double> pr((size_t)N / 2 + 1), py((size_t)N / 2 + 1);
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < N; i++) { re[(size_t)i] = (float)((pass ? y(s + i) : r(s + i)) * win[(size_t)i]); im[(size_t)i] = 0; }
            fft(re.data(), im.data(), N, false);
            auto& p = pass ? py : pr;
            for (int k = 0; k <= N / 2; k++) p[(size_t)k] = (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
        }
        double e = 0;
        for (double v : pr) e += v;
        fe.push_back(e);
        PR.push_back(pr);
        PY.push_back(py);
    }
    double fmax = 0, meanP = 0;
    for (double e : fe) fmax = std::max(fmax, e);
    for (double e : fe) meanP += e;
    meanP /= (fe.empty() ? 1 : fe.size()) * (N / 2 + 1.0);
    const double floorP = meanP * 1e-5;
    const double binHz = (double)fs / N;
    // bark bands
    auto bark = [](double f) { return 13.0 * std::atan(0.00076 * f) + 3.5 * std::atan((f / 7500.0) * (f / 7500.0)); };
    const int nb = (int)bark(fs / 2.0) + 1;
    std::vector<double> lr, ly;
    std::vector<int> act;
    double lsd = 0, lsdL = 0, lsdH = 0;
    int nl = 0;
    for (size_t f = 0; f < fe.size(); f++) {
        if (fe[f] < fmax * 1e-4) continue;
        double a = 0, al = 0, ah = 0;
        int ca = 0, cl = 0, ch = 0;
        std::vector<double> br((size_t)nb, 0.0), by((size_t)nb, 0.0);
        for (int k = 1; k < N / 2; k++) {
            const double hz = k * binHz;
            if (hz < 60 || hz > 7800) continue;
            const double d = 10.0 * std::log10((PR[f][(size_t)k] + floorP) / (PY[f][(size_t)k] + floorP));
            a += d * d; ca++;
            if (hz < 3700) { al += d * d; cl++; } else { ah += d * d; ch++; }
            const int b = (int)bark(hz);
            br[(size_t)b] += PR[f][(size_t)k];
            by[(size_t)b] += PY[f][(size_t)k];
        }
        lsd += std::sqrt(a / std::max(ca, 1));
        lsdL += std::sqrt(al / std::max(cl, 1));
        lsdH += ch ? std::sqrt(ah / ch) : 0.0;
        nl++;
        for (int b = 0; b < nb; b++) {
            lr.push_back(std::pow(br[(size_t)b] + floorP, 0.23));
            ly.push_back(std::pow(by[(size_t)b] + floorP, 0.23));
        }
    }
    m.frames = nl;
    if (nl) { m.lsd = lsd / nl; m.lsdLow = lsdL / nl; m.lsdHigh = lsdH / nl; }
    double mean = 0;
    for (double v : lr) mean += v;
    mean = lr.empty() ? 1 : mean / lr.size();
    double tot = 0;
    for (int f = 0; f < nl; f++) {
        double d2 = 0;
        for (int b = 0; b < nb; b++) {
            const double a = lr[(size_t)f * nb + b] / mean, c = ly[(size_t)f * nb + b] / mean;
            double d = std::fabs(a - c) - 0.25 * std::min(a, c);
            if (d < 0) d = 0;
            if (c > a) d *= 2.0;
            d2 += d * d;
        }
        tot += d2 / nb;
    }
    m.bld = nl ? std::sqrt(tot / nl) : 0;
    // frame level (loudness) error over the active frames: signed mean and mean absolute difference of the frame power in dB
    {
        double sb = 0, sa = 0;
        int c = 0;
        for (size_t f = 0; f < fe.size(); f++) {
            if (fe[f] < fmax * 1e-4) continue;
            double ey = 0;
            for (double v : PY[f]) ey += v;
            const double d = 10.0 * std::log10((ey + floorP) / (fe[f] + floorP));
            sb += d;
            sa += std::fabs(d);
            c++;
        }
        if (c) { m.levelBias = sb / c; m.levelErr = sa / c; }
    }
    // short-time envelope correlation in 15 third-octave bands from 150 Hz (the STOI idea of Taal et al. 2011, simplified: 32 ms
    // frames, 24-frame segments, clipping at -15 dB SDR); NOT the standard STOI implementation, an intelligibility proxy only
    {
        const int nbands = 15, seg = 24;
        std::vector<int> lo((size_t)nbands), hi((size_t)nbands);
        for (int j = 0; j < nbands; j++) {
            const double fc = 150.0 * std::pow(2.0, j / 3.0);
            lo[(size_t)j] = std::max(1, (int)std::lround(fc * std::pow(2.0, -1.0 / 6.0) / binHz));
            hi[(size_t)j] = std::min(N / 2, (int)std::lround(fc * std::pow(2.0, 1.0 / 6.0) / binHz));
        }
        double fmaxDb = 0;
        for (double e : fe) fmaxDb = std::max(fmaxDb, e);
        std::vector<std::vector<double>> XR, XY;
        for (size_t f = 0; f < fe.size(); f++) {
            if (fe[f] < fmaxDb * 1e-4) continue;   // 40 dB below the loudest frame: silence
            std::vector<double> a((size_t)nbands), b((size_t)nbands);
            for (int j = 0; j < nbands; j++) {
                double sr2 = 0, sy2 = 0;
                for (int k = lo[(size_t)j]; k < hi[(size_t)j]; k++) { sr2 += PR[f][(size_t)k]; sy2 += PY[f][(size_t)k]; }
                a[(size_t)j] = std::sqrt(sr2);
                b[(size_t)j] = std::sqrt(sy2);
            }
            XR.push_back(a);
            XY.push_back(b);
        }
        double acc = 0;
        int cnt = 0;
        const double clipK = std::pow(10.0, 15.0 / 20.0);
        for (size_t s0 = 0; s0 + seg <= XR.size(); s0++) {
            for (int j = 0; j < nbands; j++) {
                double er = 0, ey = 0;
                for (int t = 0; t < seg; t++) { er += XR[s0 + t][(size_t)j] * XR[s0 + t][(size_t)j]; ey += XY[s0 + t][(size_t)j] * XY[s0 + t][(size_t)j]; }
                const double g = ey > 0 ? std::sqrt(er / ey) : 0.0;
                double yv[64], xv[64], mx = 0, my = 0;
                for (int t = 0; t < seg; t++) {
                    xv[t] = XR[s0 + t][(size_t)j];
                    yv[t] = std::min(g * XY[s0 + t][(size_t)j], xv[t] * (1.0 + clipK));
                    mx += xv[t];
                    my += yv[t];
                }
                mx /= seg;
                my /= seg;
                double cxy = 0, cxx = 0, cyy = 0;
                for (int t = 0; t < seg; t++) { cxy += (xv[t] - mx) * (yv[t] - my); cxx += (xv[t] - mx) * (xv[t] - mx); cyy += (yv[t] - my) * (yv[t] - my); }
                if (cxx > 0 && cyy > 0) { acc += cxy / std::sqrt(cxx * cyy); cnt++; }
            }
        }
        m.stoi = cnt ? acc / cnt : 0.0;
    }
    return m;
}

// ---------------- WAV ----------------
bool readWav(const std::string& path, std::vector<int16_t>& pcm, int& fs) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + got);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    auto u16 = [&](size_t o) { return (unsigned)b[o] | (unsigned)b[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    size_t o = 12;
    int ch = 1, bits = 16;
    fs = 0;
    while (o + 8 <= b.size()) {
        const size_t len = u32(o + 4);
        if (!std::memcmp(b.data() + o, "fmt ", 4)) { ch = (int)u16(o + 10); fs = (int)u32(o + 12); bits = (int)u16(o + 22); }
        if (!std::memcmp(b.data() + o, "data", 4)) {
            if (bits != 16 || ch < 1) return false;
            const size_t n = std::min(len, b.size() - o - 8) / (2 * (size_t)ch);
            pcm.resize(n);
            for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)u16(o + 8 + i * 2 * ch);
            return fs > 0;
        }
        o += 8 + len + (len & 1);
    }
    return false;
}

bool writeWav(const std::string& path, const std::vector<int16_t>& pcm, int fs) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t dl = (uint32_t)pcm.size() * 2;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
    auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) h[o + i] = (uint8_t)(v >> (8 * i)); };
    p32(4, 36 + dl);
    p32(24, (uint32_t)fs);
    p32(28, (uint32_t)fs * 2);
    h[32] = 2; h[34] = 16;
    std::memcpy(h + 36, "data", 4);
    p32(40, dl);
    std::fwrite(h, 1, 44, f);
    for (int16_t v : pcm) { const uint8_t b2[2] = {(uint8_t)(v & 0xff), (uint8_t)((uint16_t)v >> 8)}; std::fwrite(b2, 1, 2, f); }
    std::fclose(f);
    return true;
}

}  // namespace eval
}  // namespace kaalvoice
}  // namespace dect2
