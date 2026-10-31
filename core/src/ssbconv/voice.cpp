// Voice analysis of one demodulated SSB channel (complex baseband, the suppressed carrier at 0 Hz, the signal at 0 .. +3 kHz): 10 ms frame energies
// (voice activity with hang time, the syllabic depth and the 2.5 - 8 Hz modulation), pitch from the complex autocorrelation (a flat tone has the same
// correlation at every lag; a voice has peaks at the pitch period), the audio spectrum (single lines, band share, tilt). See ssbconv.h.
#include "dect2/ssbconv/ssbconv.h"
#include <algorithm>
#include <cmath>
#include "dsp_util.h"

namespace dect2::ssbconv {
using detail::fft;
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kHist = 800;   // frames (8 s)

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// pitch (Hz, 0 = not voiced) of one 256-sample stretch; flat = the correlation is high at every lag (a tone)
float pitchOf(const cf* x, size_t n, double rate, float* maxR) {
    const size_t lmin = (size_t)(rate * 0.0025), lmax = std::min((size_t)(rate * 0.0140), n / 2);
    if (lmax <= lmin + 2) return 0;
    double e = 0;
    for (size_t i = 0; i < n; i++) e += std::norm(x[i]);
    if (e <= 0) return 0;
    std::vector<float> r(lmax + 2, 0.f);
    double mean = 0; float best = 0; size_t bl = 0;
    for (size_t L = lmin; L <= lmax; L++) {
        cf acc(0, 0);
        for (size_t i = 0; i + L < n; i++) acc += x[i] * std::conj(x[i + L]);
        double e2 = 0; for (size_t i = L; i < n; i++) e2 += std::norm(x[i]);
        const float v = (float)(std::abs(acc) / std::sqrt(e * e2 + 1e-30) );
        r[L] = v; mean += v;
        if (v > best) { best = v; bl = L; }
    }
    mean /= (double)(lmax - lmin + 1);
    if (maxR) *maxR = best;
    if (best < 0.38f || best / (float)(mean + 1e-9) < 1.55f) return 0;
    // the first peak that is as high as the best (a multiple of the period correlates a little less, half of it clearly less)
    for (size_t L = lmin + 1; L < lmax; L++)
        if (r[L] >= 0.93f * best && r[L] >= r[L - 1] && r[L] >= r[L + 1]) { bl = L; break; }
    return (float)(rate / (double)bl);
}
}  // namespace

VoiceAnalyzer::VoiceAnalyzer() { reset(); }
void VoiceAnalyzer::reset() { fr_.clear(); f_ = VoiceFeatures(); tiltNow_ = 0; pitchNow_ = 0; hang_ = 0; sLo_ = sHi_ = 0; }

WindowVoice VoiceAnalyzer::process(const cf* x, size_t n, double rate, double noiseDb) {
    WindowVoice wv;
    const size_t F = (size_t)std::lround(rate / 100.0);
    if (F < 20 || n < F * 5) return wv;
    const size_t nf = n / F;
    size_t act = 0; double pw = 0; size_t pc = 0;
    std::vector<float> pitches;
    for (size_t k = 0; k < nf; k++) {
        const cf* p = x + k * F;
        double e = 0;
        for (size_t i = 0; i < F; i++) e += std::norm(p[i]);
        const float db = (float)(10 * std::log10(e / (double)F + 1e-20));
        const bool a = db > noiseDb + 6.0;
        Fr f{db, a, -1.f};
        if (a) {
            act++; pw += std::pow(10.0, db / 10.0); pc++;
            // pitch on a 256-sample stretch centred on the frame
            const size_t W = 256;
            if (n >= W) {
                size_t s0 = k * F + F / 2 > W / 2 ? k * F + F / 2 - W / 2 : 0;
                if (s0 + W > n) s0 = n - W;
                float mr = 0;
                f.pitch = pitchOf(x + s0, W, rate, &mr);
                if (f.pitch > 0) pitches.push_back(f.pitch);
            }
        }
        fr_.push_back(f);
        if (fr_.size() > kHist) fr_.pop_front();
    }
    // hang: voice stays on 0.35 s after the last active frame
    if (act > 0) hang_ = 35; else hang_ = std::max(0.0, hang_ - (double)nf);
    wv.activeShare = (float)act / (float)nf;
    wv.active = wv.activeShare >= 0.10f || hang_ > 0;
    wv.level = pc ? (float)(10 * std::log10(pw / (double)pc)) : -200.f;
    if (!pitches.empty()) { std::sort(pitches.begin(), pitches.end()); pitchNow_ = pitches[pitches.size() / 2]; } else pitchNow_ = 0;

    // audio spectrum of the window (complex, 256 points, Hann, averaged): single line, band share, tilt
    const size_t N = 256;
    std::vector<double> P(N, 0.0);
    int blocks = 0;
    for (size_t s = 0; s + N <= n; s += N / 2) {
        std::vector<cf> b(N);
        for (size_t i = 0; i < N; i++) b[i] = x[s + i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N));
        fft(b);
        for (size_t i = 0; i < N; i++) P[i] += std::norm(b[i]);
        blocks++;
    }
    float toneDb = 0, bandFrac = 0, tilt = 0;
    double eLo = 0, eHi = 0;
    if (blocks > 0) {
        const double bin = rate / (double)N;
        auto at = [&](double hz) { long k = std::lround(hz / bin); if (k < 0) k += (long)N; return (size_t)k % N; };
        double tot = 0, inb = 0, lo = 0, hi = 0;
        std::vector<std::pair<double, size_t>> pk;
        for (size_t i = 0; i < N; i++) {
            const double f = (i < N / 2 ? (double)i : (double)i - (double)N) * bin;
            tot += P[i];
            if (f >= 250 && f <= 2900) inb += P[i];
            if (f >= 300 && f <= 1100) lo += P[i];
            if (f >= 1900 && f <= 2700) hi += P[i];
        }
        (void)at;
        bandFrac = tot > 0 ? (float)(inb / tot) : 0;
        tilt = (lo > 0 && hi > 0) ? (float)(10 * std::log10(lo / hi)) : 0;
        eLo = lo; eHi = hi;
        // strongest line vs the next separate peak
        size_t mi = 0; for (size_t i = 1; i < N; i++) if (P[i] > P[mi]) mi = i;
        double second = 0;
        for (size_t i = 0; i < N; i++) {
            const size_t d = i > mi ? i - mi : mi - i;
            if (std::min(d, N - d) >= 5) second = std::max(second, P[i]);
        }
        toneDb = second > 0 ? (float)(10 * std::log10(P[mi] / second)) : 0;
    }
    tiltNow_ = wv.activeShare >= 0.15f ? tilt : 0;
    f_.toneDb = f_.toneDb == 0 ? toneDb : 0.7f * f_.toneDb + 0.3f * toneDb;   // smoothed over windows
    if (wv.activeShare >= 0.15f) {
        // energy-weighted over the last windows: a fricative-only window (energy up high) must not outvote the vowels
        sLo_ = 0.92 * sLo_ + eLo; sHi_ = 0.92 * sHi_ + eHi;
        f_.tiltDb = (sLo_ > 0 && sHi_ > 0) ? (float)(10 * std::log10(sLo_ / sHi_)) : 0.f;
        f_.bandFrac = f_.bandFrac == 0 ? bandFrac : 0.8f * f_.bandFrac + 0.2f * bandFrac;
    }
    recompute();
    return wv;
}

void VoiceAnalyzer::recompute() {
    if (fr_.size() < 100) { f_.score = 0; return; }
    // depth over the last 300 frames
    const size_t span = std::min<size_t>(fr_.size(), 300);
    std::vector<float> v; v.reserve(span);
    size_t act = 0, voiced = 0;
    std::vector<float> pit;
    for (size_t i = fr_.size() - span; i < fr_.size(); i++) {
        v.push_back(fr_[i].db);
        if (fr_[i].active) { act++; if (fr_[i].pitch >= 0) { if (fr_[i].pitch > 0) { voiced++; pit.push_back(fr_[i].pitch); } } }
    }
    // depth among the active frames only: keyed data (ALE bursts, on-off keying) is as deep as speech over the pauses, but flat while it is on
    std::vector<float> av;
    for (size_t i = fr_.size() - span; i < fr_.size(); i++) if (fr_[i].active) av.push_back(fr_[i].db);
    if (av.size() >= 20) { std::sort(av.begin(), av.end()); f_.depthDb = av[(size_t)(0.9 * (double)(av.size() - 1))] - av[(size_t)(0.1 * (double)(av.size() - 1))]; }
    else f_.depthDb = 0;
    f_.activeFrac = (float)act / (float)span;
    f_.voiced = act ? (float)voiced / (float)act : 0.f;
    if (!pit.empty()) { std::sort(pit.begin(), pit.end()); f_.pitchHz = pit[pit.size() / 2]; }
    // modulation: DFT of the dB envelope over the last 256 frames (bins 0.39 Hz apart)
    const size_t M = 256;
    if (fr_.size() >= M) {
        std::vector<double> e(M); double mean = 0;
        for (size_t i = 0; i < M; i++) { e[i] = std::max<double>(fr_[fr_.size() - M + i].db, -300); mean += e[i]; }
        mean /= (double)M;
        double pSyl = 0, pAll = 0;
        for (size_t k = 1; k <= 64; k++) {
            double re = 0, im = 0;
            for (size_t i = 0; i < M; i++) {
                const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)M);
                const double a = (e[i] - mean) * w, ph = 2 * kPi * (double)k * (double)i / (double)M;
                re += a * std::cos(ph); im += a * std::sin(ph);
            }
            const double p = re * re + im * im, f = (double)k * 100.0 / (double)M;
            if (f >= 0.4 && f <= 25) { pAll += p; if (f >= 2.5 && f <= 8) pSyl += p; }
        }
        f_.modRatio = pAll > 0 ? (float)(pSyl / pAll) : 0.f;
    }
    const float sDepth = clamp01((f_.depthDb - 3.f) / 5.f);
    const float sVoiced = clamp01((f_.voiced - 0.12f) / 0.2f);
    const float sMod = clamp01((f_.modRatio - 0.18f) / 0.15f);
    const float sTone = 1.f - clamp01((f_.toneDb - 18.f) / 8.f);
    const float sBand = clamp01((f_.bandFrac - 0.5f) / 0.3f);
    const float sTilt = clamp01((f_.tiltDb - 2.f) / 4.f);   // signed: voice has more energy near the carrier; the mirrored sideband does not
    const float sAct = clamp01(f_.activeFrac / 0.15f);
    f_.score = std::min(std::min(sDepth, sVoiced), std::min(sTone, sBand)) * (0.55f + 0.25f * sMod + 0.2f * sTilt) * sAct;
    if (f_.tiltDb < -4.f) f_.score *= 0.2f;   // wrong sideband assumed: the band is mirrored
}

}  // namespace dect2::ssbconv
