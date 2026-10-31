// Speech-like SSB test signals (deterministic): voiced syllables on a pitch with vibrato and formants, fricative bursts, word gaps; stations that talk
// and pause; drift and fading; decoys (ALE 8-FSK, a multi-tone modem, RTTY, PSK31, FT8, AM, band noise, a carrier). Used by the unit tests and by the demo
// of the panel (screenshots without a radio). Nothing here is a model of real speech; it only has the properties the detector looks at.
#include "dect2/ssbconv/ssbconv.h"
#include <algorithm>
#include <cmath>

namespace dect2::ssbconv::synth {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAr = 8000.0;   // audio generator rate

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() { s += 0x9E3779B97F4A7C15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    double u() { return (double)(next() >> 11) / 9007199254740992.0; }
    double g() { double a = u() + 1e-12, b = u(); return std::sqrt(-2 * std::log(a)) * std::cos(2 * kPi * b); }
};

struct Vowel { double f1, f2, f3; };
const Vowel kV[] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {660, 1720, 2410}, {440, 1020, 2240}};

// complex analytic audio (one sided: tones at +f), unit-ish power while talking
std::vector<cf> voiceAnalytic(double seconds, double f0, int seed, double talkSec, double silenceSec, double startSec) {
    const size_t n = (size_t)(seconds * kAr);
    std::vector<cf> out(n, cf(0, 0));
    Rng r((uint64_t)seed);
    double t = 0, phase = 0;
    double vib1 = r.u() * 2 * kPi, vib2 = r.u() * 2 * kPi;
    size_t i = 0;
    // syllable plan
    while (i < n) {
        const double durS = 0.17 + 0.09 * r.u();
        const size_t len = std::min(n - i, (size_t)(durS * kAr));
        const Vowel v = kV[r.next() % 6];
        const double f0s = f0 * (0.9 + 0.2 * r.u());
        const bool voiced = r.u() < 0.85;
        const double amp = 0.55 + 0.45 * r.u();
        // formant weights per harmonic
        const int hmax = (int)(2800 / f0s);
        std::vector<double> a((size_t)hmax + 1, 0.0);
        for (int h = 1; h <= hmax; h++) {
            const double f = h * f0s;
            if (f < 300 || f > 2700) continue;
            auto lor = [&](double c, double bw) { const double d = (f - c) / bw; return 1.0 / (1.0 + d * d); };
            const double tilt = std::pow(300.0 / f, 1.3);
            a[(size_t)h] = (1.0 * lor(v.f1, 90) + 0.55 * lor(v.f2, 130) + 0.3 * lor(v.f3, 180)) * tilt;
        }
        double ff[10], fp[10], fa[10];
        for (int q = 0; q < 10; q++) { ff[q] = 1800 + 900 * r.u(); fp[q] = 2 * kPi * r.u(); fa[q] = 1; }
        for (size_t k = 0; k < len; k++, i++) {
            const double x = (double)k / (double)len;
            const double env = amp * std::pow(std::sin(kPi * x), 1.5);   // syllable envelope
            const double tt = t + (double)k / kAr;
            const double fi = f0s * (1 + 0.06 * std::sin(2 * kPi * 0.8 * tt + vib1) + 0.025 * std::sin(2 * kPi * 3.3 * tt + vib2));
            phase += 2 * kPi * fi / kAr;
            cf s(0, 0);
            if (voiced) {
                for (int h = 1; h <= hmax; h++) {
                    if (a[(size_t)h] == 0) continue;
                    const double ph = h * phase;
                    s += cf((float)(a[(size_t)h] * std::cos(ph)), (float)(a[(size_t)h] * std::sin(ph)));
                }
                s *= 0.6f;
            } else {   // fricative: a cluster of tones at 1.8 - 2.7 kHz with random slow amplitudes (one sided, like the SSB filter leaves it)
                const double tg = (double)i / kAr;
                for (int q = 0; q < 10; q++) {
                    if ((i & 63) == 0) fa[(size_t)q] = 0.7 * fa[(size_t)q] + 0.3 * std::fabs(r.g());
                    const double ph = 2 * kPi * ff[(size_t)q] * tg + fp[(size_t)q];
                    s += cf((float)(fa[(size_t)q] * std::cos(ph)), (float)(fa[(size_t)q] * std::sin(ph)));
                }
                s *= 0.12f;
            }
            out[i] = s * (float)env;
        }
        t += durS;
        if (r.u() < 0.22) {   // a word gap
            const size_t gap = std::min(n - i, (size_t)((0.12 + 0.3 * r.u()) * kAr));
            i += gap; t += (double)gap / kAr;
        }
    }
    // talk / silence schedule with 40 ms ramps
    if (silenceSec > 0 || startSec > 0) {
        const double period = talkSec + silenceSec;
        for (size_t k = 0; k < n; k++) {
            const double tt = (double)k / kAr - startSec;
            double g = 0;
            if (tt >= 0) {
                const double ph = std::fmod(tt, period);
                g = ph < talkSec ? std::min(1.0, std::min(ph / 0.04, (talkSec - ph) / 0.04)) : 0.0;
            }
            out[k] *= (float)g;
        }
    }
    return out;
}

// normalise to unit mean power over the talking part
void normalise(std::vector<cf>& x) {
    double p = 0; size_t c = 0;
    for (const cf& s : x) { const double e = std::norm(s); if (e > 1e-9) { p += e; c++; } }
    if (c == 0) return;
    const float g = (float)(1.0 / std::sqrt(p / (double)c));
    for (cf& s : x) s *= g;
}

std::vector<cf> gate(std::vector<cf> x, double talk, double sil, double start) {
    const double period = talk + sil;
    for (size_t k = 0; k < x.size(); k++) {
        const double tt = (double)k / kAr - start;
        double g = 0;
        if (tt >= 0) { const double ph = std::fmod(tt, period); g = ph < talk ? std::min(1.0, std::min(ph / 0.04, (talk - ph) / 0.04)) : 0.0; }
        x[k] *= (float)g;
    }
    return x;
}

std::vector<cf> fskTones(double seconds, double baud, const std::vector<double>& tones, int seed) {
    const size_t n = (size_t)(seconds * kAr);
    std::vector<cf> o(n);
    Rng r((uint64_t)seed);
    double ph = 0; size_t sym = (size_t)(kAr / baud); int cur = 0;
    for (size_t i = 0; i < n; i++) {
        if (i % sym == 0) cur = (int)(r.next() % tones.size());
        ph += 2 * kPi * tones[(size_t)cur] / kAr;
        o[i] = cf((float)std::cos(ph), (float)std::sin(ph));
    }
    return o;
}

std::vector<cf> bandNoise(double seconds, double lo, double hi, int seed) {
    const size_t n = (size_t)(seconds * kAr);
    Rng r((uint64_t)seed);
    // complex white -> lowpass of (hi-lo)/2 by a windowed sinc -> shift to the band centre
    const double bw = hi - lo, fc = bw / 2 / kAr;
    const int taps = 255;
    std::vector<float> h((size_t)taps);
    double sum = 0;
    for (int k = 0; k < taps; k++) {
        const double m = k - (taps - 1) / 2.0;
        const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (taps - 1)) + 0.08 * std::cos(4 * kPi * k / (taps - 1));
        h[(size_t)k] = (float)(s * w); sum += s * w;
    }
    std::vector<cf> w(n + (size_t)taps), o(n);
    for (cf& s : w) s = cf((float)r.g(), (float)r.g());
    const double mid = (lo + hi) / 2;
    for (size_t i = 0; i < n; i++) {
        cf a(0, 0);
        for (int k = 0; k < taps; k++) a += w[i + (size_t)k] * h[(size_t)k];
        const double ph = 2 * kPi * mid * (double)i / kAr;
        o[i] = a * (float)(1.0 / sum) * cf((float)std::cos(ph), (float)std::sin(ph));
    }
    return o;
}
}  // namespace

std::vector<float> voiceAudio(double seconds, double f0, int seed, double talkSec, double silenceSec) {
    std::vector<cf> a = voiceAnalytic(seconds, f0, seed, talkSec, silenceSec, 0);
    normalise(a);
    std::vector<float> o(a.size());
    for (size_t i = 0; i < a.size(); i++) o[i] = a[i].real();
    return o;
}

std::vector<cf> make(double rateHz, double seconds, const std::vector<Station>& st, double noisePower, int seed) {
    const size_t n = (size_t)(seconds * rateHz);
    std::vector<cf> out(n, cf(0, 0));
    const int L = (int)std::lround(rateHz / kAr);
    Rng nr((uint64_t)seed);
    // interpolation filter (analytic baseband 0..3.2 kHz at 8 kHz): windowed sinc, cutoff 3.9 kHz
    const int tpp = 24;                     // taps per phase
    const int taps = tpp * L;
    std::vector<float> h((size_t)taps);
    {
        const double fc = 3900.0 / rateHz;
        double s = 0;
        for (int k = 0; k < taps; k++) {
            const double m = k - (taps - 1) / 2.0;
            const double v = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (taps - 1)) + 0.08 * std::cos(4 * kPi * k / (taps - 1));
            h[(size_t)k] = (float)(v * w); s += v * w;
        }
        for (float& v : h) v = (float)(v * L / s);
    }
    for (const Station& S : st) {
        const double secs = seconds + 1.0;
        std::vector<cf> a;
        switch (S.kind) {
        case Station::Voice: a = voiceAnalytic(secs, S.f0, S.seed, S.talkSec, S.silenceSec, S.startSec); normalise(a); break;
        case Station::Ale8Fsk: { std::vector<double> t; for (int k = 0; k < 8; k++) t.push_back(750 + 250.0 * k); a = gate(fskTones(secs, 125, t, S.seed), S.talkSec, S.silenceSec, S.startSec); break; }
        case Station::Rtty: a = gate(fskTones(secs, 45.45, {1500, 1670}, S.seed), S.talkSec, S.silenceSec, S.startSec); break;
        case Station::Ft8: { std::vector<double> t; for (int k = 0; k < 8; k++) t.push_back(1000 + 6.25 * k); a = gate(fskTones(secs, 6.25, t, S.seed), S.talkSec, S.silenceSec, S.startSec); break; }
        case Station::Psk31: {
            const size_t m = (size_t)(secs * kAr); a.assign(m, cf(0, 0)); Rng r((uint64_t)S.seed);
            const size_t sym = (size_t)(kAr / 31.25); double prev = 1, cur = 1;
            for (size_t i = 0; i < m; i++) {
                if (i % sym == 0) { prev = cur; cur = (r.next() & 1) ? cur : -cur; }
                const double x = (double)(i % sym) / (double)sym;
                const double w = 0.5 - 0.5 * std::cos(kPi * x);
                const double amp = prev + (cur - prev) * w;
                const double ph = 2 * kPi * 1000.0 * (double)i / kAr;
                a[i] = cf((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
            }
            a = gate(std::move(a), S.talkSec, S.silenceSec, S.startSec); break; }
        case Station::MultiTone: {
            const size_t m = (size_t)(secs * kAr); a.assign(m, cf(0, 0)); Rng r((uint64_t)S.seed);
            const int tones = 80; std::vector<double> f((size_t)tones), p((size_t)tones);
            for (int k = 0; k < tones; k++) { f[(size_t)k] = 300 + (S.bwHz) * (k + 0.5) / tones; p[(size_t)k] = r.u() * 2 * kPi; }
            for (size_t i = 0; i < m; i++) { cf s(0, 0); for (int k = 0; k < tones; k++) { const double ph = 2 * kPi * f[(size_t)k] * (double)i / kAr + p[(size_t)k]; s += cf((float)std::cos(ph), (float)std::sin(ph)); } a[i] = s * (float)(1.0 / std::sqrt((double)tones)); }
            a = gate(std::move(a), S.talkSec, S.silenceSec, S.startSec); break; }
        case Station::Noise: a = gate(bandNoise(secs, 300, 300 + S.bwHz, S.seed), S.talkSec, S.silenceSec, S.startSec); break;
        case Station::Tone: { const size_t m = (size_t)(secs * kAr); a.assign(m, cf(1, 0)); break; }
        case Station::Am: {
            std::vector<cf> v = voiceAnalytic(secs, S.f0, S.seed, 1e9, 0, 0); normalise(v);
            a.resize(v.size());
            for (size_t i = 0; i < v.size(); i++) a[i] = cf(1.0f + 0.7f * v[i].real() * 0.6f, 0);   // carrier + double sideband audio (centre at 0)
            break; }
        }
        // upsample, shift, drift, fade, gain
        Rng fr((uint64_t)S.seed * 31 + 5);
        const double fp1 = fr.u() * 2 * kPi, fp2 = fr.u() * 2 * kPi;
        double ph = 0;
        const double dt = 1.0 / rateHz;
        const size_t endIdx = S.endSec >= 1e8 ? n : (size_t)(S.endSec * rateHz);
        for (size_t i = 0; i < n && i < endIdx; i++) {
            const size_t phase = i % (size_t)L, base = i / (size_t)L;
            cf s(0, 0);
            for (int k = 0; k < tpp; k++) {
                if (base < (size_t)k) break;
                const size_t idx = base - (size_t)k;
                if (idx >= a.size()) continue;
                s += a[idx] * h[phase + (size_t)k * (size_t)L];
            }
            if (S.side == Side::Lsb && S.kind == Station::Voice) s = std::conj(s);
            if (S.side == Side::Lsb && (S.kind == Station::Ale8Fsk || S.kind == Station::Noise || S.kind == Station::MultiTone)) s = std::conj(s);
            const double tt = (double)i * dt;
            ph += 2 * kPi * (S.offsetHz + S.driftHzPerSec * tt) * dt;
            double g = S.gain;
            if (S.fadeDepthDb > 0) {
                const double f = 0.5 * (std::sin(2 * kPi * S.fadeRateHz * tt + fp1) + 0.6 * std::sin(2 * kPi * S.fadeRateHz * 2.3 * tt + fp2)) / 1.3 + 0.5;   // 0..1
                g *= std::pow(10.0, -S.fadeDepthDb * (1 - f) / 20.0);
            }
            out[i] += s * cf((float)(g * std::cos(ph)), (float)(g * std::sin(ph)));
        }
    }
    const float sg = (float)std::sqrt(noisePower / 2);
    for (size_t i = 0; i < n; i++) out[i] += cf((float)nr.g() * sg, (float)nr.g() * sg);
    return out;
}

Demo demo() {
    Demo d;
    auto v = [](double off, Side sd, double f0, int seed, double talk, double sil, double start, double gain) {
        Station s; s.offsetHz = off; s.side = sd; s.f0 = f0; s.seed = seed; s.talkSec = talk; s.silenceSec = sil; s.startSec = start; s.gain = gain; return s;
    };
    Station a = v(20000, Side::Usb, 115, 11, 9, 11, 0, 0.45); a.fadeDepthDb = 8; d.stations.push_back(a);          // the QSO
    Station b = v(20055, Side::Usb, 190, 12, 9, 11, 10, 0.40); b.fadeDepthDb = 8; b.fadeRateHz = 0.4; d.stations.push_back(b);
    Station c = v(-40000, Side::Lsb, 135, 13, 12, 6, 3, 0.55); c.driftHzPerSec = 1.5; d.stations.push_back(c);    // LSB on 20 m, against the convention, drifting
    const double pitches[4] = {105, 160, 210, 135};                                                           // a net on 14.233: four voices in turn until 55 s
    for (int k = 0; k < 4; k++) { Station n = v(33000 + 15.0 * k, Side::Usb, pitches[k], 20 + k, 3, 9, 3.0 * k, 0.35); n.endSec = 55; d.stations.push_back(n); }
    Station one = v(-25000, Side::Usb, 125, 31, 7, 99, 18, 0.5); one.endSec = 30; d.stations.push_back(one);      // a single call, gone after 30 s
    Station weak = v(-14000, Side::Usb, 145, 32, 10, 8, 5, 0.26); weak.fadeDepthDb = 5; d.stations.push_back(weak);
    auto dec = [](Station::Kind k, double off, int seed, double gain, double talk = 10, double sil = 3) { Station s; s.kind = k; s.offsetHz = off; s.seed = seed; s.gain = gain; s.talkSec = talk; s.silenceSec = sil; return s; };
    d.stations.push_back(dec(Station::Ale8Fsk, 41000, 41, 0.35));
    d.stations.push_back(dec(Station::MultiTone, -8000, 42, 0.25));
    d.stations.push_back(dec(Station::Rtty, 8000, 43, 0.3));
    d.stations.push_back(dec(Station::Psk31, -31000, 44, 0.3));
    Station am; am.kind = Station::Am; am.offsetHz = 0 + 3500; am.seed = 45; am.gain = 0.35; am.f0 = 130; d.stations.push_back(am);
    return d;
}

}  // namespace dect2::ssbconv::synth
