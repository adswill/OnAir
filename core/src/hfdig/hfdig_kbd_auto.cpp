// Auto for the keyboard modes (hfdig_kbd.h, kbdAutoAnalyse): the shape of the signal in the spectrum (occupied band, tone pitch) narrows the sets worth trying,
// then the decoder itself runs on the audio for each set and the one whose text reads best wins. Hell has no text: its pixel rate and keying pick the variant.
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/autoparam/autoparam.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace dect2 {

namespace {
constexpr double kFs = kHfdigAudioRate;
using dect2::math::kPi;

KbdAutoHyp trial(const float* x, size_t n, const KbdConfig& base) {
    KbdConfig c = base;
    c.autoCentre = true;
    c.autoParams = false;
    auto d = makeKbdDecoder();
    d->setConfig(c);
    for (size_t i = 0; i < n; i += 2048) d->feedAudio(x + i, std::min<size_t>(2048, n - i));
    HfdigKbdTelemetry t;
    d->telemetry(t);
    KbdAutoHyp h;
    h.tones = c.tones; h.bandwidth = c.bandwidth; h.variant = c.variant; h.centreHz = t.centreHz;
    h.chars = (uint32_t)t.chars;
    h.readable = autoparam::readableFraction(t.text);
    h.score = (float)((double)t.chars * h.readable * h.readable);
    return h;
}

// the sets of tones and bandwidth worth trying for this occupied band and pitch; Olivia and Contestia share the grid
std::vector<KbdConfig> oliviaSets(int family, const autoparam::ToneComb& comb) {
    std::vector<KbdConfig> v;
    for (int bw : kKbdBwList) {
        if (comb.found && std::fabs(comb.occupiedHz - bw) > 0.45 * bw) continue;
        for (int tones : kKbdTonesList) {
            const double pitch = (double)bw / tones;
            if (pitch < 7.8 || pitch > 500) continue;   // beyond the sets stations use: 500 Hz / 64 is the slowest tried
            if (comb.spacingHz > 0 && (pitch > comb.spacingHz * 2.3 || pitch < comb.spacingHz * 0.43)) continue;
            KbdConfig c; c.family = family; c.tones = tones; c.bandwidth = bw;
            v.push_back(c);
        }
    }
    if (v.empty())
        for (int bw : kKbdBwList) for (int tones : {8, 16, 32}) { KbdConfig c; c.family = family; c.tones = tones; c.bandwidth = bw; v.push_back(c); }
    return v;
}

// Hell: keying rate from the envelope (on/off) or the frequency (FSK variants). Numbers in pixels per second, keying and deviation from hfdig_kbd_hell.cpp.
struct HellSpec { double pps; bool fsk; double dev; };
const HellSpec kHell[] = {{245, false, 0}, {30.625, false, 0}, {490, true, 150}, {1225, false, 0}, {2205, false, 0}, {245, true, 61.25}, {245, true, 27.5}};

double lineAt(const std::vector<double>& s, double pps, double fsamp) {
    std::complex<double> a(0, 0), b(0, 0);
    double e = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const double t = (double)i / fsamp;
        a += s[i] * std::complex<double>(std::cos(2 * kPi * pps * t), -std::sin(2 * kPi * pps * t));
        b += s[i] * std::complex<double>(std::cos(kPi * pps * t), -std::sin(kPi * pps * t));   // a pixel is sent twice: the line at half the rate
        e += std::fabs(s[i]);
    }
    return e > 0 ? (std::abs(a) + std::abs(b)) / e : 0;
}

std::vector<KbdAutoHyp> hellAnalyse(const float* x, size_t n, int maxHyps) {
    std::vector<KbdAutoHyp> out;
    const auto comb = autoparam::analyseToneComb(x, n, kFs);
    if (!comb.found || comb.snrDb < 7) return out;
    // analytic signal around the signal's middle, by FFT
    int N = 1;
    while ((size_t)N < n) N <<= 1;
    Fft f(N);
    std::vector<cf32> z((size_t)N, cf32(0, 0));
    for (size_t i = 0; i < n; i++) z[i] = cf32(x[i], 0);
    f.forward(z.data());
    const int c = (int)std::lround(comb.centreHz / kFs * N), w = (int)std::lround(1400.0 / kFs * N);
    std::vector<cf32> y((size_t)N, cf32(0, 0));
    for (int k = -w; k <= w; k++) y[(size_t)((k + N) % N)] = z[(size_t)std::max(0, std::min(N - 1, c + k))];
    f.inverse(y.data());
    std::vector<double> env(n), dfq(n, 0.0);
    double envMean = 0;
    for (size_t i = 0; i < n; i++) { env[i] = std::abs(y[i]) / N; envMean += env[i]; }
    envMean /= (double)n;
    for (size_t i = 1; i < n; i++) dfq[i] = std::arg(y[i] * std::conj(y[i - 1])) * kFs / (2 * kPi);
    double m = 0;
    for (double d : dfq) m += d;
    m /= (double)n;
    double var = 0, q = 0;
    for (size_t i = 0; i < n; i++) { var += (env[i] - envMean) * (env[i] - envMean); dfq[i] -= m; q += std::fabs(dfq[i]); }
    const double cv = std::sqrt(var / (double)n) / std::max(1e-9, envMean);
    q /= (double)n;
    const bool amKeyed = cv > 0.55;
    for (auto& v : env) v -= envMean;
    int best = -1;
    double bestV = 0;
    for (int i = 0; i < (int)(sizeof kHell / sizeof *kHell); i++) {
        if (kHell[i].fsk == amKeyed) continue;
        double v = lineAt(kHell[i].fsk ? dfq : env, kHell[i].pps, kFs);
        if (kHell[i].fsk) v *= 1.0 / (1.0 + std::fabs(q - kHell[i].dev) / std::max(20.0, kHell[i].dev));
        if (v > bestV) { bestV = v; best = i; }
    }
    if (best < 0 || bestV < 0.01) return out;
    KbdAutoHyp h;
    h.variant = best; h.centreHz = comb.centreHz; h.score = (float)bestV; h.readable = 1; h.chars = 0;
    out.push_back(h);
    (void)maxHyps;
    return out;
}
}  // namespace

std::vector<KbdAutoHyp> kbdAutoAnalyse(const float* audio, size_t n, int family, int maxHyps) {
    std::vector<KbdAutoHyp> res;
    if (n < (size_t)(4 * kFs)) return res;
    if (family == KbdHell) return hellAnalyse(audio, n, maxHyps);
    const auto comb = autoparam::analyseToneComb(audio, n, kFs);
    if (!comb.found || comb.snrDb < 4) return res;   // nothing stands out: no decoder would find anything
    std::vector<KbdConfig> sets;
    if (family == KbdOlivia || family == KbdContestia) sets = oliviaSets(family, comb);
    else {
        for (int v = 0; v < kbdVariantCount(family); v++) {
            KbdConfig c; c.family = family; c.variant = v;
            const double bw = kbdBandwidthHz(c);
            if (comb.found && (comb.occupiedHz < bw * 0.35 || comb.occupiedHz > bw * 3.0)) continue;
            sets.push_back(c);
        }
        if (sets.empty()) for (int v = 0; v < kbdVariantCount(family); v++) { KbdConfig c; c.family = family; c.variant = v; sets.push_back(c); }
    }
    if (sets.size() > 8) sets.resize(8);
    const size_t take = std::min(n, (size_t)(24 * kFs));
    const float* x = audio + (n - take);
    std::vector<KbdAutoHyp> all;
    for (auto& c : sets) {
        c.centreHz = comb.centreHz > 0 ? comb.centreHz : 1500;
        auto h = trial(x, take, c);
        if (h.chars >= 8 && h.readable >= 0.8) all.push_back(h);
    }
    std::sort(all.begin(), all.end(), [](const KbdAutoHyp& a, const KbdAutoHyp& b) { return a.score > b.score; });
    for (const auto& h : all) { if ((int)res.size() >= maxHyps) break; res.push_back(h); }
    return res;
}

}  // namespace dect2
