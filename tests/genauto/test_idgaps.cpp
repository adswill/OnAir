// Identifier gaps closed for General / Auto: DVB-T2 8k / 16k / 32k and DVB-T 2k / 8k captured at the native 9.14 MS/s of an 8 MHz channel (and
// off the centre), and an SSB voice signal (one sideband, no carrier). Each must come out as the right row with the right mode and options.
#include "dect2/common/stable_random.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"

static bool tvSignal(bool dvbt, int fftCode, int gi, double fs, size_t n, std::vector<cf32>& out) {
    DeviceInfo d;
    TuneSettings t;
    t.centerHz = 522e6; t.sampleRate = fs; t.bandwidthMhz = 8;
    t.synth.snrDb = 60; t.synth.pace = 40;
    t.synth.dvbt = dvbt;
    if (dvbt) t.synth.dvbtMode = fftCode;
    else { t.synth.tx.s2field1 = fftCode; t.synth.tx.giIdx = gi; }
    IqRing ring(1u << 24);
    auto src = makeSource(d);
    std::string why;
    if (!src->start(t, ring, why)) { printf("source: %s\n", why.c_str()); return false; }
    const size_t skip = (size_t)(fs * 0.1);
    std::vector<cf32> all(skip + n);
    size_t got = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (got < all.size() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
        const size_t k = ring.read(all.data() + got, all.size() - got);
        got += k;
        if (!k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    src->stop();
    if (got < all.size()) return false;
    out.assign(all.begin() + (long)skip, all.end());
    return true;
}

static void addNoise(std::vector<cf32>& x, double snrDb, uint32_t seed) {
    double p = 0; for (const cf32& v : x) p += std::norm(v); p /= x.size();
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0, (float)std::sqrt(p / std::pow(10.0, snrDb / 10) / 2));
    for (auto& v : x) v += cf32{nd(rng), nd(rng)};
}
static void shift(std::vector<cf32>& x, double fs, double hz) {
    double ph = 0; const double d = 2 * M_PI * hz / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + d, 2 * M_PI); }
}

// Speech-like single sideband: a voiced source (pitch 100 to 140 Hz with vibrato) shaped by three formants, syllables at about 4 Hz with pauses,
// turned into one sideband (analytic signal) with the suppressed carrier fcHz away from the centre; lower: LSB (the audio mirrored)
static std::vector<cf32> ssbVoice(double fs, size_t n, double fcHz, bool lower, uint32_t seed) {
    std::mt19937 rng(seed);
    const size_t N = [&] { size_t m = 1; while (m < n) m <<= 1; return m; }();
    std::vector<float> au(N, 0.f);
    double ph[40] = {};
    const double f0b = 100 + (seed % 30);
    double syl = 0, sylAmp = 0, formShift = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = i / fs;
        if (i % (size_t)(fs * 0.22) == 0) { syl = std::uniform_real_distribution<double>(0, 1)(rng); sylAmp = syl < 0.18 ? 0.02 : 0.4 + 0.6 * syl; formShift = std::uniform_real_distribution<double>(-0.25, 0.25)(rng); }
        const double f0 = f0b * (1 + 0.04 * std::sin(2 * M_PI * 5.3 * t));
        double v = 0;
        for (int h = 1; h < 40; h++) {
            const double fh = f0 * h;
            if (fh > 2900) break;
            ph[h] += 2 * M_PI * fh / fs;
            const double g = std::exp(-std::pow((fh - 550 * (1 + formShift)) / 220, 2)) + 0.6 * std::exp(-std::pow((fh - 1500 * (1 + formShift * 0.6)) / 300, 2)) + 0.35 * std::exp(-std::pow((fh - 2500) / 330, 2));
            v += g * std::sin(ph[h]) / std::sqrt((double)h);
        }
        au[i] = (float)(sylAmp * v * 0.25);
    }
    // the analytic signal of the audio: one sideband
    std::vector<cf32> X(N);
    for (size_t i = 0; i < N; i++) X[i] = cf32(au[i], 0);
    Fft F((int)N);
    F.forward(X.data());
    for (size_t k = 1; k < N / 2; k++) X[k] *= 2.f;
    for (size_t k = N / 2 + 1; k < N; k++) X[k] = cf32(0, 0);
    F.inverse(X.data());
    std::vector<cf32> y(n);
    double p = 0;
    for (size_t i = 0; i < n; i++) { y[i] = X[i] / (float)N; if (lower) y[i] = std::conj(y[i]); p += std::norm(y[i]); }
    p /= n;
    const float g = (float)(0.1 / std::sqrt(p));
    double c = 0; const double d = 2 * M_PI * fcHz / fs;
    for (size_t i = 0; i < n; i++) { y[i] *= g * cf32((float)std::cos(c), (float)std::sin(c)); c = std::fmod(c + d, 2 * M_PI); }
    return y;
}

int main() {
    const double fs = 64e6 / 7;   // 9.142857 MS/s: the native rate of an 8 MHz T2 / T channel
    struct C { const char* label; bool t; int fft, gi; };
    const C cases[] = {{"T2 8k", false, 1, 4}, {"T2 16k", false, 4, 4}, {"T2 32k", false, 5, 2}, {"T2 8k GI1/32", false, 1, 1}, {"T 8k", true, 1, 0}, {"T 2k", true, 0, 0}};
    for (const C& c : cases)
        for (double off : {0.0, 600e3}) {
            std::vector<cf32> x;
            const size_t n = (size_t)(fs * 0.45);
            if (!tvSignal(c.t, c.fft, c.gi, fs, n, x)) { CHECK(false, "%s: no signal", c.label); continue; }
            addNoise(x, 22, 3);
            if (off != 0) shift(x, fs, off);
            Options o; o.rfHz = 522e6; o.usePriors = false;
            const Result r = identify(x.data(), x.size(), fs, o);
            printf("%-14s off %+.0f kHz: %s | %s %.0f%% | Tu %.1f us | obw %.2f MHz\n", c.label, off / 1e3, r.summary.c_str(), r.cands.empty() ? "-" : r.cands[0].name.c_str(),
                   r.cands.empty() ? 0.0 : r.cands[0].confidence * 100, r.f.ofdmTuSec * 1e6, r.f.obwHz / 1e6);
            if (getenv("IDGAPS_TEXT")) printf("%s\n", toText(r).c_str());
            CHECK(r.f.valid && !r.cands.empty() && r.cands[0].name == "DVB-T / T2", "%s off %+.0f: top %s", c.label, off, r.cands.empty() ? "none" : r.cands[0].name.c_str());
            CHECK(r.verdict != VerdictUnknown, "%s off %+.0f: unknown", c.label, off);
            if (!r.cands.empty()) {
                const Memory m = toMemory(r.cands[0], r.f);
                CHECK(m.options.count("dvbBw") && m.options.at("dvbBw") == "0", "%s: channel width option %s", c.label, m.options.count("dvbBw") ? m.options.at("dvbBw").c_str() : "none");
            }
        }
    // ---- SSB voice, both sidebands, off the centre, with noise
    for (bool lower : {false, true})
        for (double fc : {0.0, 40e3}) {
            const double sfs = 250e3, rf = lower ? 7.1e6 : 14.2e6;
            std::vector<cf32> x = ssbVoice(sfs, (size_t)(sfs * 1.5), fc, lower, 5);
            addNoise(x, 26, 9);
            Options o; o.rfHz = rf; o.usePriors = false;
            const Result r = identify(x.data(), x.size(), sfs, o);
            printf("SSB %s fc %+.0f kHz: %s | %s %.0f%%\n", lower ? "LSB" : "USB", fc / 1e3, r.summary.c_str(), r.cands.empty() ? "-" : r.cands[0].name.c_str(), r.cands.empty() ? 0.0 : r.cands[0].confidence * 100);
            if (getenv("IDGAPS_TEXT")) printf("%s\n", toText(r).c_str());
            CHECK(r.f.valid && !r.cands.empty() && r.cands[0].name == "SSB voice", "SSB %s fc %+.0f: top %s", lower ? "LSB" : "USB", fc, r.cands.empty() ? "none" : r.cands[0].name.c_str());
            if (!r.cands.empty() && r.cands[0].name == "SSB voice") {
                const Memory m = toMemory(r.cands[0], r.f);
                CHECK(std::fabs((double)m.freqHz - (rf + fc)) < 900, "SSB %s: dial %.1f Hz from the carrier", lower ? "LSB" : "USB", (double)m.freqHz - (rf + fc));
                CHECK(m.mode == "general" && m.options.count("genDemod") && m.options.at("genDemod") == (lower ? "4" : "3"), "SSB %s: demodulator option %s", lower ? "LSB" : "USB", m.options.count("genDemod") ? m.options.at("genDemod").c_str() : "none");
            }
        }
    return testSummary("idgaps");
}
