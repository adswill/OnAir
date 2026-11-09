// RTTY auto-lock (hfdig_rtty.h, rttyAutoAnalyse): the two spectral lines give the pair of tones and the shift, then the decoder itself is run over
// the audio for each speed and both polarities, and the guess that gives the best text wins; the others are kept as runner-ups.
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

namespace {

constexpr double kFs = kHfdigAudioRate;
constexpr int kNfft = 2048, kHopA = 1024;
constexpr double kBin = kFs / kNfft;
constexpr int kLo = 77, kHi = 844;   // 300 .. 3300 Hz

struct Pair { double low = 0; int shiftIdx = 0; double score = 0; };

// the audio's average power spectrum, 2048-point Hann frames
std::vector<double> avgSpectrum(const float* x, size_t n) {
    std::vector<double> p(kNfft / 2, 0.0);
    std::vector<float> win((size_t)kNfft);
    for (int i = 0; i < kNfft; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * dect2::math::kPi * i / kNfft));
    Fft f(kNfft);
    std::vector<cf32> buf((size_t)kNfft);
    int frames = 0;
    for (size_t s = 0; s + kNfft <= n; s += kHopA) {
        for (int i = 0; i < kNfft; i++) buf[(size_t)i] = cf32(x[s + (size_t)i] * win[(size_t)i], 0.f);
        f.forward(buf.data());
        for (int k = 0; k < kNfft / 2; k++) p[(size_t)k] += std::norm(buf[(size_t)k]);
        frames++;
    }
    if (frames) for (auto& v : p) v /= frames;
    return p;
}

// score of the string of text: letters, digits, spaces and the usual marks count, controls a little, the rest half
float textScore(const std::string& t) {
    float g = 0;
    for (char c : t) {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') g += 1.0f;
        else if (c == '\r' || c == '\n') g += 0.3f;
        else g += 0.5f;
    }
    return g;
}

}   // namespace

std::vector<RttyAutoHyp> rttyAutoAnalyse(const float* audio, size_t n, int maxHyps) {
    std::vector<RttyAutoHyp> res;
    if (n < (size_t)(3 * kFs)) return res;
    const auto pw = avgSpectrum(audio, n);
    if (pw.empty()) return res;
    // the floor: the median over the band
    std::vector<double> sc(pw.begin() + kLo, pw.begin() + kHi + 1);
    std::nth_element(sc.begin(), sc.begin() + (long)sc.size() / 2, sc.end());
    const double fl = std::max(1e-12, sc[sc.size() / 2]);
    // the lines: local maxima of the spectrum well above the floor, their places refined by a parabola in the log spectrum; the pairs: two lines
    // a table shift apart (within 10 Hz), scored by the weaker of the two (both tones are sent, an idle line only the mark)
    struct Line { double hz, p; };
    std::vector<Line> lines;
    for (int k = kLo + 1; k < kHi; k++) {
        const double a = pw[(size_t)k];
        if (a < pw[(size_t)k - 1] || a <= pw[(size_t)k + 1] || a < 4.0 * fl) continue;
        const double l0 = std::log(pw[(size_t)k - 1] + 1e-30), l1 = std::log(a), l2 = std::log(pw[(size_t)k + 1] + 1e-30);
        const double den = l0 - 2 * l1 + l2;
        const double off = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (l0 - l2) / den)) : 0.0;
        lines.push_back({(k + off) * kBin, a});
    }
    std::vector<Pair> pairs;
    for (size_t i = 0; i < lines.size(); i++)
        for (size_t j = i + 1; j < lines.size(); j++) {
            const double d = lines[j].hz - lines[i].hz;
            if (d > kRttyShiftTable[kRttyShifts - 1] + 12) break;
            int si = -1;
            double bestErr = 10.0;
            for (int q = 0; q < kRttyShifts; q++)
                if (std::fabs(d - kRttyShiftTable[q]) < bestErr) { bestErr = std::fabs(d - kRttyShiftTable[q]); si = q; }
            if (si < 0) continue;
            Pair p; p.low = lines[i].hz; p.shiftIdx = si; p.score = std::min(lines[i].p, lines[j].p) / fl;
            pairs.push_back(p);
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score > b.score; });
    // the strongest few, not the same lower tone twice with the same shift
    std::vector<Pair> pick;
    for (const auto& p : pairs) {
        bool dup = false;
        for (const auto& q : pick) dup |= q.shiftIdx == p.shiftIdx && std::fabs(q.low - p.low) < 25.0;
        if (!dup && p.score >= 0.15 * pairs[0].score) pick.push_back(p);   // the weaker pairs are side lobes of the lines of the stronger ones
        if (pick.size() >= 4) break;
    }
    // the decoder over the audio for each guess; the last 20 s of it at most
    const size_t take = std::min(n, (size_t)(20 * kFs));
    const float* x = audio + (n - take);
    std::vector<RttyAutoHyp> all;
    for (const auto& p : pick)
        for (int bi = 0; bi < kRttyBauds; bi++)
            for (int rv = 0; rv < 2; rv++) {
                auto d = makeRttyDecoder();
                d->setBaudIndex(bi); d->setShiftIndex(p.shiftIdx); d->setReverse(rv != 0); d->setFixedPair(p.low);
                d->reset();
                for (size_t i = 0; i < take; i += 4096) d->feedAudio(x + i, std::min<size_t>(4096, take - i));
                HfdigRttyTelemetry t;
                d->telemetry(t);
                RttyAutoHyp h;
                h.lowHz = p.low; h.shiftIdx = p.shiftIdx; h.shiftHz = kRttyShiftTable[p.shiftIdx]; h.baudIdx = bi; h.baud = kRttyBaudTable[bi];
                h.reverse = rv != 0; h.chars = (uint32_t)t.chars; h.errors = (uint32_t)t.framingErrors;
                h.score = textScore(t.text) - 2.5f * (float)t.framingErrors;
                all.push_back(h);
            }
    std::sort(all.begin(), all.end(), [](const RttyAutoHyp& a, const RttyAutoHyp& b) { return a.score > b.score; });
    for (const auto& h : all) {
        if ((int)res.size() >= maxHyps) break;
        if (h.score < 8.0f || h.chars < 8) break;
        res.push_back(h);
    }
    return res;
}

}   // namespace dect2
