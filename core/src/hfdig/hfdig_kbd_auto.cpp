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
    // the start of a decode is noise (the signal search, the interleaver filling): judge the rest
    const size_t skip = t.text.size() / 3;
    h.readable = autoparam::readableFraction(t.text.substr(skip));
    h.score = (float)((double)t.chars * h.readable * h.readable);
    return h;
}

// how a set fits the shape measured from the spectrum: 0 is perfect
double setDistance(const autoparam::ToneComb& comb, double bw) {   // the pitch of the tones is not usable: orthogonal tones overlap in the spectrum, there is no comb to see
    return comb.occupiedHz > 0 ? std::fabs(std::log(bw / std::max(1.0, comb.occupiedHz))) : 0;
}
struct Cand { KbdConfig c; double dist; };

std::vector<Cand> oliviaSets(int family, const autoparam::ToneComb& comb) {
    std::vector<Cand> v;
    for (int bw : kKbdBwList)
        for (int tones : kKbdTonesList) {
            const double pitch = (double)bw / tones;
            if (pitch < 7.8 || pitch > 500) continue;   // beyond the sets stations use
            KbdConfig c; c.family = family; c.tones = tones; c.bandwidth = bw;
            v.push_back({c, setDistance(comb, bw)});
        }
    return v;
}

std::vector<Cand> variantSets(int family, const autoparam::ToneComb& comb) {
    std::vector<Cand> v;
    for (int i = 0; i < kbdVariantCount(family); i++) {
        KbdConfig c; c.family = family; c.variant = i;
        const double bw = kbdBandwidthHz(c);
        v.push_back({c, setDistance(comb, bw)});
    }
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
    const int nHell = (int)(sizeof kHell / sizeof *kHell);
    if (amKeyed) {   // on/off keying: the narrow ones (about 390 Hz) and the two fast ones (over 1 kHz); the keying line separates the pair
        for (int i = 0; i < nHell; i++) {
            if (kHell[i].fsk || (comb.occupiedHz > 900) != (i == 3 || i == 4)) continue;
            const double v = lineAt(env, kHell[i].pps, kFs);
            if (v > bestV) { bestV = v; best = i; }
        }
        if (best < 0 || bestV < 0.008) return out;
    } else {         // frequency keying: the occupied width follows the deviation
        double bestD = 1e9;
        for (int i = 0; i < nHell; i++) {
            if (!kHell[i].fsk) continue;
            const double nominal = kHell[i].dev * 2 + 180;
            const double d = std::fabs(comb.occupiedHz - nominal);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0 || bestD > 0.4 * (kHell[best].dev * 2 + 180) || comb.snrDb < 12) return out;
        bestV = comb.snrDb;
    }
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
    auto sets = family == KbdOlivia || family == KbdContestia ? oliviaSets(family, comb) : variantSets(family, comb);
    std::stable_sort(sets.begin(), sets.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (sets.size() > 5) sets.resize(5);
    const size_t take = std::min(n, (size_t)(45 * kFs));
    const float* x = audio + (n - take);
    // the shape of the signal decides; the decoder only has to agree. A wrong set often still gives letters (noise decodes to the short codes), so the text alone cannot choose.
    double firstDist = -1;
    std::vector<KbdAutoHyp> all;
    for (auto& k : sets) {
        k.c.centreHz = comb.centreHz > 0 ? comb.centreHz : 1500;
        auto h = trial(x, take, k.c);
        if (h.chars < 8 || h.readable < 0.85) continue;
        if (firstDist < 0) firstDist = k.dist;
        if (k.dist > firstDist + 0.25) break;
        all.push_back(h);
    }
    // among sets that fit the band equally, the cleanest text; only a clear tie goes to the one with the most of it
    std::stable_sort(all.begin(), all.end(), [](const KbdAutoHyp& a, const KbdAutoHyp& b) { return std::fabs(a.readable - b.readable) > 0.02 ? a.readable > b.readable : a.score > b.score; });
    for (const auto& h : all) { if ((int)res.size() >= maxHyps) break; res.push_back(h); }
    return res;
}

}  // namespace dect2
