// The signal identifier (dect2/engine/sig_ident.h) on a synthetic evaluation set: signals of about thirty modes made here from their published
// parameters (symbol rate, deviation, pulse shape, frame syncs, OFDM symbol and guard lengths, burst timing), not with the app's own generators,
// so this is not the data the signature table was measured on. Each one is impaired (a random frequency offset, noise at 10 / 15 / 25 dB SNR over
// the signal's width, a symbol-rate and deviation error, the bandwidth variants of a mode as separate cases), the region a click near its
// middle selects is identified, and the result is scored: a confusion matrix of the top-1 answers, top-1 and top-3 per mode and per SNR, the
// share the verify step confirmed, and the time. Deterministic (fixed seeds, cases run in parallel but each on its own); a few seconds.
//
//   test_specx_ideval          the evaluation and its thresholds (docs/IDENTIFIER.md has the numbers)
//   test_specx_ideval --v      every case: what came out
//   test_specx_ideval --mode X only the cases whose label contains X (thresholds not checked)
//   FEAT=1 (with --v) adds the measured features and the best three scores to each line; JOB=k runs the k-th run alone
#include "dect2/common/stable_random.h"
#include "dect2/dmr/dmr_proto.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/pager/pager_proto.h"
#include "../common/cpu_report.h"
#include "../common/ci_slack.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rng {
    std::mt19937 g;
    explicit Rng(uint32_t s) : g(s) {}
    double u(double a, double b) { return dect2::uniform_real_distribution<double>(a, b)(g); }
    double n() { return dect2::normal_distribution<double>(0, 1)(g); }
    int i(int k) { return std::min(k - 1, (int)u(0, k)); }
};

using Sig = std::vector<cf32>;

// ---------------------------------------------------------------------------------------------------- modulators

// FSK from symbol values (deviation per unit), the frequency smoothed by two boxcars of `smooth` symbols (a Gaussian-like filter), the phase integrated
Sig fsk(const std::vector<float>& sym, double rs, double devPerUnit, double fs, double smooth, size_t n) {
    const double sps = fs / rs;
    std::vector<double> fr(n);
    for (size_t i = 0; i < n; i++) { const size_t k = std::min(sym.size() - 1, (size_t)(i / sps)); fr[i] = sym[k] * devPerUnit; }
    const size_t L = std::max<size_t>(1, (size_t)std::lround(smooth * sps));
    for (int pass = 0; pass < 2; pass++) {
        std::vector<double> pre(n + 1, 0.0);
        for (size_t i = 0; i < n; i++) pre[i + 1] = pre[i] + fr[i];
        for (size_t i = 0; i < n; i++) { const size_t a = i >= L / 2 ? i - L / 2 : 0, b = std::min(n, a + L); fr[i] = (pre[b] - pre[a]) / (double)(b - a); }
    }
    Sig x(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) { ph += 2 * kPi * fr[i] / fs; if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi; x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    return x;
}

double rrcPulse(double t, double b) {
    if (std::fabs(t) < 1e-8) return 1 - b + 4 * b / kPi;
    if (b > 0 && std::fabs(std::fabs(t) - 1 / (4 * b)) < 1e-8) return b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
    return (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / (kPi * t * (1 - 16 * b * b * t * t));
}

// linear modulation: complex symbols through a root raised cosine of roll-off beta (6 symbols each side)
Sig linear(const std::vector<cf32>& sym, double rs, double fs, double beta, size_t n) {
    const double sps = fs / rs;
    Sig x(n, cf32(0, 0));
    for (size_t i = 0; i < n; i++) {
        const double t = i / sps;
        const long k0 = (long)std::floor(t) - 6, k1 = (long)std::floor(t) + 6;
        cf32 s(0, 0);
        for (long k = std::max<long>(0, k0); k <= k1 && k < (long)sym.size(); k++) s += (float)rrcPulse(t - (double)k, beta) * sym[(size_t)k];
        x[i] = s;
    }
    return x;
}

// OFDM: Nfft carriers at fs, `used` of them (centred, DC left out), a guard of cp samples; pilots: every pilotStep-th carrier boosted (0 none)
Sig ofdm(int Nfft, int used, int cp, size_t n, Rng& r, int pilotStep = 0) {
    Sig x;
    x.reserve(n + (size_t)(Nfft + cp));
    Fft F(Nfft);
    std::vector<cf32> b((size_t)Nfft);
    int sym = 0;
    while (x.size() < n) {
        std::fill(b.begin(), b.end(), cf32(0, 0));
        for (int k = -used / 2; k <= used / 2; k++) {
            if (k == 0) continue;
            const int idx = (k + Nfft) % Nfft;
            const bool pilot = pilotStep > 0 && ((k + used / 2) + 3 * (sym % 4)) % pilotStep == 0;
            const bool neg = ((uint32_t)(k + Nfft) * 2654435761u >> 13) & 1;   // a pilot's sign: a fixed pseudo-random sequence over the carriers
            b[(size_t)idx] = pilot ? cf32(neg ? -1.33f : 1.33f, 0) : cf32(r.u(0, 1) < 0.5 ? -0.7071f : 0.7071f, r.u(0, 1) < 0.5 ? -0.7071f : 0.7071f);
        }
        F.inverse(b.data());
        for (int i = Nfft - cp; i < Nfft; i++) x.push_back(b[(size_t)i]);
        for (int i = 0; i < Nfft; i++) x.push_back(b[(size_t)i]);
        sym++;
    }
    x.resize(n);
    return x;
}

// on / off: a burst of onSec every periodSec (first at startSec), ramps of a twentieth of a millisecond
void gateBursts(Sig& x, double fs, double onSec, double periodSec, double startSec) {
    const double ramp = std::max(2.0, 5e-5 * fs);
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs - startSec;
        if (t < 0) { x[i] = 0; continue; }
        const double ph = std::fmod(t, periodSec) * fs, on = onSec * fs;
        const double g = ph >= on ? 0.0 : std::min(1.0, std::min(ph, on - ph) / ramp);
        x[i] *= (float)g;
    }
}

std::vector<float> randSym4(Rng& r, size_t n) { std::vector<float> s(n); for (auto& v : s) v = (float)(2 * r.i(4) - 3); return s; }
std::vector<float> randSym2(Rng& r, size_t n) { std::vector<float> s(n); for (auto& v : s) v = r.u(0, 1) < 0.5 ? -1.f : 1.f; return s; }
template <class T> void put(std::vector<float>& s, size_t at, const T* p, int len) { for (int j = 0; j < len && at + (size_t)j < s.size(); j++) s[at + (size_t)j] = (float)p[j]; }

// ---------------------------------------------------------------------------------------------------- the cases

struct Case {
    std::string label;                // "DMR", "NXDN48", ...
    std::vector<std::string> truth;   // the candidate name that is right, then the names of its group (the same signal on air: a group-level hit)
    double fs = 48e3, sec = 0.6, bwHz = 10e3;   // sample rate, length, the width the SNR is measured over
    double mhz = 0;                   // its usual frequency (the "frequency known" pass)
    std::function<Sig(Rng&, size_t, double, double)> make;   // (rng, samples, rate factor, deviation factor)
};

std::vector<Case> cases() {
    std::vector<Case> v;
    auto add = [&](const char* label, std::vector<std::string> truth, double fs, double sec, double bw, double mhz, std::function<Sig(Rng&, size_t, double, double)> mk) {
        Case c; c.label = label; c.truth = std::move(truth); c.fs = fs; c.sec = sec; c.bwHz = bw; c.mhz = mhz; c.make = std::move(mk); v.push_back(c);
    };
    // ---- 4FSK digital voice, with their frame syncs at their frame spacing
    add("DMR", {"DMR"}, 48e3, 0.6, 9e3, 446.1, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 4800 * kr;
        auto s = randSym4(r, (size_t)(n / (48e3 / rs)) + 8);
        int8_t p[24]; dmr::syncSymbols(dmr::kSyncBsVoice, p);
        for (size_t at = 54; at + 24 < s.size(); at += 144) put(s, at, p, 24);
        return fsk(s, rs, 648 * kd, 48e3, 0.5, n);
    });
    add("P25", {"P25 Phase 1"}, 48e3, 0.6, 8e3, 851, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 4800 * kr;
        auto s = randSym4(r, (size_t)(n / (48e3 / rs)) + 8);
        for (size_t at = 10; at + 24 < s.size(); at += 864) put(s, at, p25::kSync, p25::kSyncSym);
        return fsk(s, rs, 600 * kd, 48e3, 0.5, n);
    });
    add("NXDN48", {"NXDN48", "dPMR"}, 24e3, 1.0, 5e3, 450, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 2400 * kr;
        auto s = randSym4(r, (size_t)(n / (24e3 / rs)) + 8);
        for (size_t at = 5; at + 10 < s.size(); at += nxdn::kFrameSym) put(s, at, nxdn::kFsw, nxdn::kFswSym);
        return fsk(s, rs, 350 * kd, 24e3, 0.5, n);
    });
    add("NXDN96", {"NXDN96", "DMR", "P25 Phase 1"}, 48e3, 0.6, 10e3, 450, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 4800 * kr;
        auto s = randSym4(r, (size_t)(n / (48e3 / rs)) + 8);
        for (size_t at = 5; at + 10 < s.size(); at += nxdn::kFrameSym) put(s, at, nxdn::kFsw, nxdn::kFswSym);
        return fsk(s, rs, 800 * kd, 48e3, 0.5, n);
    });
    add("dPMR", {"dPMR", "NXDN48"}, 24e3, 1.0, 5e3, 446.1, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 2400 * kr;
        auto s = randSym4(r, (size_t)(n / (24e3 / rs)) + 8);
        for (size_t at = 0; at + 24 < s.size(); at += 4 * dpmr::kFrameSym) put(s, at, dpmr::kFs1, dpmr::kFsLong);
        for (size_t at = 2 * dpmr::kFrameSym; at + 12 < s.size(); at += 2 * dpmr::kFrameSym) put(s, at, dpmr::kFs2, dpmr::kFsShort);
        return fsk(s, rs, 350 * kd, 24e3, 0.5, n);
    });
    // ---- 2FSK / GMSK
    add("POCSAG", {"POCSAG", "FLEX"}, 48e3, 1.0, 11e3, 466.075, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 1200 * kr;
        std::vector<float> s;
        for (int i = 0; i < 576; i++) s.push_back(i % 2 ? 1.f : -1.f);
        while (s.size() < (size_t)(n / (48e3 / rs)) + 8) {
            for (int b = 31; b >= 0; b--) s.push_back(((pager::kPocsagSync >> b) & 1) ? -1.f : 1.f);   // a 1 is the lower frequency
            for (int w = 0; w < 16 * 32; w++) s.push_back(r.u(0, 1) < 0.5 ? -1.f : 1.f);
        }
        return fsk(s, rs, 4500 * kd, 48e3, 0.2, n);
    });
    add("AIS", {"AIS"}, 48e3, 1.0, 10e3, 162.025, [](Rng& r, size_t n, double kr, double kd) {
        const double rs = 9600 * kr;
        const size_t ns = (size_t)(n / (48e3 / rs)) + 8;
        std::vector<float> s(ns, 0.f);
        for (size_t at = 900; at + 256 < ns; at += 2400) {   // a 256-bit slot every 250 ms: training, flag, data, flag (NRZI: a 0 changes the level)
            std::vector<int> bits;
            for (int i = 0; i < 24; i++) bits.push_back(i % 2);
            const int flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
            for (int b : flag) bits.push_back(b);
            for (int i = 0; i < 184; i++) bits.push_back(r.i(2));
            for (int b : flag) bits.push_back(b);
            float lvl = 1;
            for (size_t j = 0; j < bits.size(); j++) { if (!bits[j]) lvl = -lvl; s[at + j] = lvl; }
        }
        Sig x = fsk(s, rs, 2400 * kd, 48e3, 0.45, n);
        for (size_t i = 0; i < n; i++) { const size_t k = (size_t)(i / (48e3 / rs)); if (k >= ns || s[k] == 0.f) x[i] = 0; }
        return x;
    });
    add("RTTY", {"RTTY", "NAVTEX", "DSC"}, 8e3, 2.0, 300, 14.08, [](Rng& r, size_t n, double kr, double kd) { return fsk(randSym2(r, (size_t)(n / (8e3 / (45.45 * kr))) + 4), 45.45 * kr, 85 * kd, 8e3, 0.1, n); });
    add("NAVTEX", {"NAVTEX", "DSC", "RTTY"}, 8e3, 2.0, 300, 0.518, [](Rng& r, size_t n, double kr, double kd) { return fsk(randSym2(r, (size_t)(n / (8e3 / (100 * kr))) + 4), 100 * kr, 85 * kd, 8e3, 0.1, n); });
    add("D-STAR", {"D-STAR"}, 48e3, 0.6, 5e3, 145.5, [](Rng& r, size_t n, double kr, double kd) { return fsk(randSym2(r, (size_t)(n / (48e3 / (4800 * kr))) + 4), 4800 * kr, 1200 * kd, 48e3, 0.6, n); });
    add("Radiosonde", {"Radiosonde"}, 48e3, 0.8, 8e3, 403, [](Rng& r, size_t n, double kr, double kd) { return fsk(randSym2(r, (size_t)(n / (48e3 / (4800 * kr))) + 4), 4800 * kr, 2100 * kd, 48e3, 0.4, n); });
    add("GSM", {"GSM-R / GSM"}, 1e6, 0.3, 250e3, 925, [](Rng& r, size_t n, double kr, double kd) { return fsk(randSym2(r, (size_t)(n / (1e6 / (270833 * kr))) + 4), 270833 * kr, 67708 * kd, 1e6, 0.6, n); });
    add("APRS", {"APRS / packet"}, 48e3, 1.0, 9e3, 144.8, [](Rng& r, size_t n, double kr, double kd) {   // AFSK 1200 Bd (1200 / 2200 Hz) on FM, +-3 kHz
        (void)kr;
        Sig x(n);
        double ph = 0, ta = 0;
        int bit = 0; double nextBit = 0;
        for (size_t i = 0; i < n; i++) {
            const double t = i / 48e3;
            if (t >= nextBit) { bit = r.i(2); nextBit += 1 / 1200.0; }
            ta += 2 * kPi * (bit ? 1200.0 : 2200.0) / 48e3;
            ph += 2 * kPi * 3000 * kd * std::sin(ta) / 48e3;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        gateBursts(x, 48e3, 0.35, 0.5, 0.05);
        return x;
    });
    // ---- linear modulation
    add("PSK31", {"PSK31"}, 8e3, 2.0, 60, 14.07, [](Rng& r, size_t n, double kr, double) {   // phase reversals with a cosine envelope
        const double rs = 31.25 * kr, sps = 8e3 / rs;
        std::vector<float> s((size_t)(n / sps) + 2, 1.f);
        for (size_t k = 1; k < s.size(); k++) s[k] = r.u(0, 1) < 0.5 ? -s[k - 1] : s[k - 1];
        Sig x(n);
        for (size_t i = 0; i < n; i++) {
            const double t = i / sps; const size_t k = (size_t)t; const double fr = t - (double)k;
            const float a = s[k], p = k ? s[k - 1] : a;
            x[i] = cf32(a == p ? a : (float)(p * std::cos(kPi * fr)), 0);
        }
        return x;
    });
    add("Inmarsat-C", {"Inmarsat-C", "Inmarsat Aero"}, 24e3, 1.0, 2e3, 1537.7, [](Rng& r, size_t n, double kr, double) {
        const double rs = 1200 * kr; std::vector<cf32> s((size_t)(n / (24e3 / rs)) + 8);
        for (auto& v : s) v = cf32(r.u(0, 1) < 0.5 ? -1.f : 1.f, 0);
        return linear(s, rs, 24e3, 0.6, n);
    });
    add("HFDL", {"HFDL"}, 12e3, 1.0, 2.2e3, 8.977, [](Rng& r, size_t n, double kr, double) {
        const double rs = 1800 * kr; std::vector<cf32> s((size_t)(n / (12e3 / rs)) + 8);
        for (auto& v : s) v = cf32(r.u(0, 1) < 0.5 ? -1.f : 1.f, 0);
        return linear(s, rs, 12e3, 0.2, n);
    });
    add("TETRA", {"TETRA"}, 100e3, 0.6, 21e3, 390.0125, [](Rng& r, size_t n, double kr, double) {
        const double rs = 18000 * kr; std::vector<cf32> s((size_t)(n / (100e3 / rs)) + 8);
        double ph = kPi / 4;
        for (auto& v : s) { ph += kPi / 4 * (2 * r.i(4) + 1); v = cf32((float)std::cos(ph), (float)std::sin(ph)); }
        return linear(s, rs, 100e3, 0.35, n);
    });
    add("Meteor", {"Meteor-M LRPT"}, 250e3, 0.5, 97e3, 137.9, [](Rng& r, size_t n, double kr, double) {
        const double rs = 72000 * kr; std::vector<cf32> s((size_t)(n / (250e3 / rs)) + 8);
        for (auto& v : s) v = cf32(r.u(0, 1) < 0.5 ? -0.7071f : 0.7071f, r.u(0, 1) < 0.5 ? -0.7071f : 0.7071f);
        return linear(s, rs, 250e3, 0.6, n);
    });
    add("VDL2", {"VDL Mode 2"}, 50e3, 1.0, 12e3, 136.975, [](Rng& r, size_t n, double kr, double) {
        const double rs = 10500 * kr; std::vector<cf32> s((size_t)(n / (50e3 / rs)) + 8);
        double ph = 0;
        for (auto& v : s) { ph += kPi / 4 * r.i(8); v = cf32((float)std::cos(ph), (float)std::sin(ph)); }
        Sig x = linear(s, rs, 50e3, 0.6, n);
        gateBursts(x, 50e3, 0.03, 0.12, 0.02);
        return x;
    });
    // ---- OFDM
    add("DAB", {"DAB / DAB+"}, 2.048e6, 0.3, 1.54e6, 227.36, [](Rng& r, size_t n, double, double) { return ofdm(2048, 1536, 504, n, r); });
    add("DRM", {"DRM", "HamDRM"}, 48e3, 2.0, 10e3, 6.095, [](Rng& r, size_t n, double, double) { return ofdm(1024, 206, 256, n, r, 20); });
    add("DVB-T", {"DVB-T / T2"}, 64e6 / 7, 0.12, 7.6e6, 522, [](Rng& r, size_t n, double, double) { return ofdm(2048, 1704, 512, n, r, 12); });
    // ---- analog
    add("FM broadcast", {"FM broadcast"}, 1e6, 0.4, 180e3, 98.5, [](Rng& r, size_t n, double, double kd) {
        Sig x(n);
        double ph = 0, f1 = r.u(300, 3000), f2 = r.u(3000, 12000), p1 = 0, p2 = 0, pp = 0;
        for (size_t i = 0; i < n; i++) {
            p1 += 2 * kPi * f1 / 1e6; p2 += 2 * kPi * f2 / 1e6; pp += 2 * kPi * 19e3 / 1e6;
            const double l = std::sin(p1), rr = std::sin(p2);
            const double mpx = 0.45 * (l + rr) / 2 + 0.09 * std::sin(pp) + 0.45 * (l - rr) / 2 * std::sin(2 * pp);
            ph += 2 * kPi * 75e3 * kd * mpx / 1e6;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        return x;
    });
    add("AM broadcast", {"AM broadcast", "Airband voice"}, 48e3, 1.0, 9e3, 0.999, [](Rng& r, size_t n, double, double) {
        Sig x(n);
        double lp1 = 0, lp2 = 0;
        for (size_t i = 0; i < n; i++) {   // speech-like: noise low-passed to about 4 kHz
            lp1 += 0.45 * (r.n() - lp1); lp2 += 0.45 * (lp1 - lp2);
            x[i] = cf32((float)(1 + 0.5 * std::max(-1.0, std::min(1.0, 1.6 * lp2))), 0);
        }
        return x;
    });
    add("CW", {"CW", "HF beacons"}, 8e3, 2.0, 100, 7.03, [](Rng& r, size_t n, double, double) {   // 20 wpm: a dot 60 ms
        Sig x(n);
        std::vector<int> on;
        while (on.size() * 480 < n) { const int el = r.i(3); for (int k = 0; k < (el == 2 ? 3 : 1); k++) on.push_back(el == 0 ? 0 : 1); on.push_back(0); }
        double env = 0;
        for (size_t i = 0; i < n; i++) { const double tgt = on[i / 480]; env += (tgt - env) * 0.01; x[i] = cf32((float)env, 0); }
        return x;
    });
    // ---- chirps, pulses, MFSK
    add("LoRa SF7", {"LoRa", "LoRaWAN", "LoRa APRS", "Meshtastic", "MeshCore"}, 500e3, 0.5, 125e3, 868.1, [](Rng& r, size_t n, double, double) {
        const double B = 125e3, T = 128 / B;
        Sig x(n);
        double ph = 0;
        std::vector<double> off;
        for (size_t i = 0; i < n; i++) {
            const double t = i / 500e3; const size_t k = (size_t)(t / T);
            while (off.size() <= k) off.push_back(off.size() < 8 ? 0.0 : r.u(0, 1));
            double fr = std::fmod(t / T - (double)k + off[k], 1.0);
            ph += 2 * kPi * (-B / 2 + B * fr) / 500e3;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        gateBursts(x, 500e3, 0.2, 0.3, 0.03);
        return x;
    });
    add("LoRa SF9 250k", {"LoRa", "Meshtastic", "LoRaWAN", "LoRa APRS", "MeshCore"}, 1e6, 0.5, 250e3, 869.525, [](Rng& r, size_t n, double, double) {
        const double B = 250e3, T = 512 / B;
        Sig x(n);
        double ph = 0;
        std::vector<double> off;
        for (size_t i = 0; i < n; i++) {
            const double t = i / 1e6; const size_t k = (size_t)(t / T);
            while (off.size() <= k) off.push_back(off.size() < 8 ? 0.0 : r.u(0, 1));
            double fr = std::fmod(t / T - (double)k + off[k], 1.0);
            ph += 2 * kPi * (-B / 2 + B * fr) / 1e6;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        gateBursts(x, 1e6, 0.25, 0.35, 0.02);
        return x;
    });
    add("ADS-B", {"ADS-B / Mode S"}, 4e6, 0.25, 2.6e6, 1090, [](Rng& r, size_t n, double, double) {   // 1 Mbit/s PPM, 0.5 us pulses, 112-bit squitters
        Sig x(n, cf32(0, 0));
        for (double t0 = 1e-3; t0 + 130e-6 < n / 4e6; t0 += r.u(0.4e-3, 2.5e-3)) {
            std::vector<double> pulses = {0, 1e-6, 3.5e-6, 4.5e-6};
            for (int b = 0; b < 112; b++) pulses.push_back(8e-6 + b * 1e-6 + (r.i(2) ? 0 : 0.5e-6));
            for (double p : pulses) { const size_t a = (size_t)((t0 + p) * 4e6); for (size_t i = a; i < a + 2 && i < n; i++) x[i] = cf32(1, 0); }
        }
        return x;
    });
    add("Olivia", {"Olivia", "Contestia"}, 8e3, 2.0, 1000, 14.072, [](Rng& r, size_t n, double kr, double) {   // 32 tones 31.25 Hz apart, 31.25 Bd
        const double rs = 31.25 * kr;
        std::vector<float> s((size_t)(n / (8e3 / rs)) + 4);
        for (auto& v : s) v = (float)(r.i(32) - 15.5);
        return fsk(s, rs, 31.25, 8e3, 0.15, n);
    });
    add("FT8", {"FT8", "FT4", "FT2", "JS8Call"}, 4e3, 2.0, 50, 14.074, [](Rng& r, size_t n, double, double) {   // 8 tones 6.25 Hz apart, 6.25 Bd
        std::vector<float> s((size_t)(n / (4e3 / 6.25)) + 4);
        for (auto& v : s) v = (float)(r.i(8) - 3.5);
        return fsk(s, 6.25, 6.25, 4e3, 0.3, n);
    });
    return v;
}

struct Outcome {
    std::string label, top1;
    double snr = 0; int known = 0;
    int rank = -1, groupRank = -1;    // where the truth (the group) is among the first three, -1 not there
    bool called = false;              // the verdict was not "unknown"
    double conf = 0, ms = 0;
    int verify = 0;                   // the verify verdict of the truth when it is a candidate
    std::string summary;
};

uint32_t fnv(const std::string& s) { uint32_t h = 2166136261u; for (unsigned char ch : s) { h ^= ch; h *= 16777619u; } return h; }   // the same on every platform (std::hash is not)

Outcome runOne(const Case& c, double snrDb, int known, int trial) {
    Rng r(0x51ED27u ^ fnv(c.label) ^ (uint32_t)(snrDb * 131) ^ (uint32_t)(trial * 7919));   // the same signal with and without the frequency known
    const size_t n = (size_t)(c.fs * c.sec);
    const double kr = 1 + r.u(-1e-4, 1e-4), kd = 1 + r.u(-0.05, 0.05);   // a crystal off by up to 100 ppm, the deviation by 5 %
    Sig x = c.make(r, n, kr, kd);
    double p = 0;
    for (const cf32& v : x) p += std::norm(v);
    p /= std::max<size_t>(1, x.size());   // the average power, as a spectrum shows it (a sparse burst is stronger while it is on)
    const double lim = std::max(0.0, 0.42 * c.fs - 0.5 * c.bwHz);
    const double off = lim > 0 ? r.u(-std::min(lim, 0.3 * c.fs), std::min(lim, 0.3 * c.fs)) : 0;
    const double sigma = std::sqrt(p / std::pow(10.0, snrDb / 10) * (c.fs / c.bwHz) / 2);
    double ph = r.u(0, 2 * kPi);
    const double d = 2 * kPi * off / c.fs;
    for (auto& v : x) {
        v *= cf32((float)std::cos(ph), (float)std::sin(ph));
        ph = std::fmod(ph + d, 2 * kPi);
        v += cf32((float)(sigma * r.n()), (float)(sigma * r.n()));
    }
    const double click = off + r.u(-0.15, 0.15) * std::min(c.bwHz, 25e3);
    const Region rg = regionAt(x.data(), x.size(), c.fs, click);
    Options o;
    o.loHz = rg.loHz; o.hiHz = rg.hiHz;
    o.rfHz = known ? c.mhz * 1e6 - off : 0;
    o.usePriors = known != 0;
    const Result res = identify(x.data(), x.size(), c.fs, o);
    Outcome out;
    out.label = c.label; out.snr = snrDb; out.known = known; out.ms = res.ms;
    out.called = res.verdict != VerdictUnknown && !res.cands.empty();
    out.top1 = out.called ? res.cands[0].name : "(unknown)";
    out.conf = res.cands.empty() ? 0 : res.cands[0].confidence;
    out.summary = res.summary;
    if (getenv("FEAT")) {   // the measurement in one line (for tuning)
        const Features& f = res.f;
        char b[400];
        snprintf(b, sizeof b, " || kurt %.2f car %.0f lines %.0f/%.0f/%.0f/%.0f tu %.0fus corr %.2f tones %d sp %.1f comb %.0f/%.2f const %d rings %d coh %.2f/%.2f/%.2f roll %.2f snr %.1f",
                 f.envKurt, f.carrierDb, f.lineDb[0], f.lineDb[1], f.lineDb[2], f.lineDb[3], f.ofdmTuSec * 1e6, f.ofdmCorr, f.tones, f.toneSpacingHz, f.combHz, f.combStrength, f.constOrder, f.ampRings, f.phaseCoh[0], f.phaseCoh[1], f.phaseCoh[2], f.rolloff, f.snrDb);
        out.summary += b;
        snprintf(b, sizeof b, " region [%+.0f %+.0f] off %+.0f centre %+.0f", rg.loHz, rg.hiHz, off, f.centreHz);
        out.summary += b;
        for (size_t k = 0; k < res.cands.size() && k < 3; k++) out.summary += " | " + res.cands[k].name + " " + std::to_string((int)res.cands[k].score);
    }
    for (size_t k = 0; k < res.cands.size(); k++) {
        const std::string& nm = res.cands[k].name;
        if (nm == c.truth[0]) { if (k < 3 && out.rank < 0) out.rank = (int)k; out.verify = res.cands[k].verify; }
        if (k < 3 && out.groupRank < 0 && std::find(c.truth.begin(), c.truth.end(), nm) != c.truth.end()) out.groupRank = (int)k;
    }
    return out;
}

struct Tally { int n = 0, top1 = 0, top3 = 0, group1 = 0, group3 = 0, wrongSure = 0, unknown = 0; };

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    std::string only;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--v")) verbose = true;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) only = argv[++i];
    }
    (void)signatureCount();
    const std::vector<Case> cs = cases();
    const double snrs[3] = {10, 15, 25};
    struct Job { size_t c; double snr; int known; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < cs.size(); i++) {
        if (!only.empty() && cs[i].label.find(only) == std::string::npos) continue;
        for (double s : snrs) jobs.push_back({i, s, 0});
        jobs.push_back({i, 15, 1});   // and once with the frequency known (the database, the band plans, the modes' usual frequencies)
    }
    if (const char* j = getenv("JOB")) { const size_t k = (size_t)atoi(j); if (k < jobs.size()) jobs = {jobs[k]}; }   // one run alone (for tuning)
    std::vector<Outcome> res(jobs.size());
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::atomic<size_t> next{0};
        const unsigned nt = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
        std::vector<std::thread> th;
        for (unsigned t = 0; t < nt; t++) th.emplace_back([&] { for (size_t j; (j = next++) < jobs.size();) res[j] = runOne(cs[jobs[j].c], jobs[j].snr, jobs[j].known, 0); });
        for (auto& t : th) t.join();
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::map<double, Tally> bySnr; Tally knownT;
    std::map<std::string, Tally> byMode;
    std::map<std::string, std::map<std::string, int>> conf;   // blind, all SNRs: truth -> top-1
    int verN = 0, verOk = 0, verBad = 0;
    double sumMs = 0, maxMs = 0;
    for (const Outcome& o : res) {
        Tally& t = o.known ? knownT : bySnr[o.snr];
        Tally* ts[2] = {&t, o.known ? nullptr : &byMode[o.label]};
        for (Tally* p : ts) {
            if (!p) continue;
            p->n++;
            p->top1 += o.called && o.rank == 0; p->top3 += o.called && o.rank >= 0;
            p->group1 += o.called && o.groupRank == 0; p->group3 += o.called && o.groupRank >= 0;
            p->unknown += !o.called;
            p->wrongSure += o.called && o.groupRank != 0 && o.conf >= 0.5;
        }
        if (!o.known) conf[o.label][o.top1]++;
        if (o.rank >= 0) { verN++; verOk += o.verify == VerifyConfirmed; verBad += o.verify == VerifyFailed; }
        sumMs += o.ms; maxMs = std::max(maxMs, o.ms);
        if (verbose) printf("%-14s snr %2.0f %s -> %-22s %3.0f %% %s  rank %d group %d verify %+d | %s\n", o.label.c_str(), o.snr, o.known ? "freq " : "blind", o.top1.c_str(), o.conf * 100, o.called ? "" : "(unknown)", o.rank, o.groupRank, o.verify, o.summary.c_str());
    }
    auto pct = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
    printf("==== signal identifier, synthetic evaluation: %zu cases, %zu runs, %.2f s (%.0f ms per identify on average, %.0f at worst) ====\n", cs.size(), res.size(), wall, res.empty() ? 0 : sumMs / res.size(), maxMs);
    for (const auto& kv : bySnr)
        printf("blind, SNR %2.0f dB: top-1 %5.1f %%, top-3 %5.1f %% (group-level %5.1f / %5.1f %%), unknown %4.1f %%, wrong with >= 50 %%: %4.1f %%  (n %d)\n", kv.first, pct(kv.second.top1, kv.second.n), pct(kv.second.top3, kv.second.n),
               pct(kv.second.group1, kv.second.n), pct(kv.second.group3, kv.second.n), pct(kv.second.unknown, kv.second.n), pct(kv.second.wrongSure, kv.second.n), kv.second.n);
    printf("frequency known, SNR 15 dB: top-1 %5.1f %%, top-3 %5.1f %% (group-level %5.1f / %5.1f %%)  (n %d)\n", pct(knownT.top1, knownT.n), pct(knownT.top3, knownT.n), pct(knownT.group1, knownT.n), pct(knownT.group3, knownT.n), knownT.n);
    printf("verify step: of %d runs with the truth among the first three, it confirmed the truth %d times and failed it %d times\n", verN, verOk, verBad);
    printf("\n---- per mode (blind, three SNRs): top-1 / top-3 / group top-1\n");
    for (const auto& kv : byMode) printf("  %-14s %d/%d  %d/%d  %d/%d\n", kv.first.c_str(), kv.second.top1, kv.second.n, kv.second.top3, kv.second.n, kv.second.group1, kv.second.n);
    printf("\n---- confusion matrix (blind, three SNRs): truth -> top-1 answers\n");
    for (const auto& kv : conf) {
        printf("  %-14s", kv.first.c_str());
        for (const auto& p : kv.second) printf(" %s x%d;", p.first.c_str(), p.second);
        printf("\n");
    }
    // determinism: the same case twice gives the same answer
    if (!cs.empty() && only.empty()) {
        const Outcome a = runOne(cs[0], 15, 0, 0), b = runOne(cs[0], 15, 0, 0);
        CHECK(a.top1 == b.top1 && a.conf == b.conf && a.summary == b.summary, "not deterministic: %s %.4f / %s %.4f", a.top1.c_str(), a.conf, b.top1.c_str(), b.conf);
    }
    if (!only.empty() || verbose) { printf("(partial or verbose run: thresholds not checked)\n"); return 0; }
    // the thresholds: what this set measured (docs/IDENTIFIER.md) less a margin for the platforms' floating point
    const Tally& m15 = bySnr[15], &m25 = bySnr[25], &m10 = bySnr[10];
    CHECK(pct(m15.top3, m15.n) >= 76, "top-3 at 15 dB SNR %.1f %% (>= 76)", pct(m15.top3, m15.n));
    CHECK(pct(m15.group1, m15.n) >= 70, "group-level top-1 at 15 dB %.1f %% (>= 70)", pct(m15.group1, m15.n));
    CHECK(pct(m25.top3, m25.n) >= 80, "top-3 at 25 dB SNR %.1f %% (>= 80)", pct(m25.top3, m25.n));
    CHECK(pct(m10.top3, m10.n) >= 56, "top-3 at 10 dB SNR %.1f %% (>= 56)", pct(m10.top3, m10.n));
    CHECK(pct(knownT.top1, knownT.n) >= pct(m15.top1, m15.n) - 7, "the frequency known made top-1 worse: %.1f %% against %.1f %% blind", pct(knownT.top1, knownT.n), pct(m15.top1, m15.n));
    CHECK(pct(m15.wrongSure, m15.n) <= 12, "wrong with confidence >= 50 %% at 15 dB: %.1f %% (<= 12)", pct(m15.wrongSure, m15.n));
    CHECK(verN == 0 || pct(verBad, verN) <= 15, "the verify step failed the truth in %.1f %% of the runs (<= 15)", pct(verBad, verN));
    cpuCal("specx_ideval", "identify() on average", sumMs / std::max<size_t>(1, res.size()), "ms", '<', 250 * ciSlack());
    CHECK(sumMs / std::max<size_t>(1, res.size()) < 250 * ciSlack(), "identify() took %.0f ms on average (limit %.0f)", sumMs / std::max<size_t>(1, res.size()), 250 * ciSlack());
    return testSummary("specx_ideval");
}
