// ISCAT-A / ISCAT-B (WSJT): 42 tones, a 24-symbol frame = 4 Costas tones (0 1 3 2), 2 length tones (L and L+5 mod 42, L = message length + 1), 18 data tones
// that run through the characters "@" + message and wrap. ISCAT-A: 512 samples at 11025 Hz per symbol (46.4 ms), tone spacing 21.53 Hz, first tone 47 x
// spacing = 1012 Hz. ISCAT-B: half the symbol, double the spacing, first tone 13 x spacing = 560 Hz.
// The receiver builds a grid of tone energies at quarter-symbol steps and quarter-spacing frequency steps, finds symbol phase, tuning error and frame
// start from the Costas tones, reads the message length, then adds up (folds) the energy of every character position over all the frames.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
const char kSet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /.?@-";   // 42
const int kCostas[4] = {0, 1, 3, 2};
struct Par { double nsps, df, f0; };
Par par(int v, double fs) {
    Par p;
    const double n11025 = v == 0 ? 512.0 : 256.0;
    p.nsps = fs * n11025 / 11025.0;
    p.df = 11025.0 / n11025;
    p.f0 = (v == 0 ? 47.0 : 13.0) * p.df;
    return p;
}
int charIndex(char c) {
    c = (char)std::toupper((unsigned char)c);
    for (int i = 0; i < 42; i++) if (kSet[i] == c) return i;
    return 36;
}
}  // namespace

std::vector<float> iscatGenerate(const std::string& msg, int variant, double fs, double seconds, double offsetHz, double amp, double startSec) {
    const Par p = par(variant, fs);
    std::vector<int> imsg;
    imsg.push_back(40);
    for (char c : msg) imsg.push_back(charIndex(c));
    const int L = (int)imsg.size();
    const size_t n = (size_t)(seconds * fs);
    std::vector<float> y(n, 0.f);
    const long nsym = (long)std::floor((seconds - startSec) * fs / p.nsps);
    long k = 0;
    double ph = 0;
    size_t pos = 0;
    const size_t first = (size_t)(startSec * fs);
    for (; pos < first && pos < n; pos++) y[pos] = 0;
    for (long s = 0; s < nsym; s++) {
        const int j = (int)(s % 24);
        int tone;
        if (j < 4) tone = kCostas[j];
        else if (j == 4) tone = L % 42;
        else if (j == 5) tone = (L + 5) % 42;
        else { tone = imsg[(size_t)(k % L)]; k++; }
        const size_t a = first + (size_t)std::llround((double)s * p.nsps), b = std::min(n, first + (size_t)std::llround((double)(s + 1) * p.nsps));
        const double w = 2 * kPi * (p.f0 + tone * p.df + offsetHz) / fs;
        for (size_t i = a; i < b; i++) { ph += w; y[i] = (float)(amp * std::sin(ph)); }
    }
    return y;
}

bool iscatDecode(const float* x, size_t n, double fs, int variant, IscatResult& out, double tolHz) {
    const Par p = par(variant, fs);
    const double hop = p.nsps / 4.0;
    const size_t wlen = (size_t)std::llround(p.nsps);
    if (n < (size_t)(24 * 2 * p.nsps + wlen)) return false;
    const size_t N = nextPow2(wlen) * 4;
    const double binHz = fs / (double)N;
    const double fineHz = p.df / 4.0;
    const int sMax = (int)std::ceil(2 * tolHz / fineHz);            // shift steps
    const int G = sMax + 4 * 41 + 1;                                // fine grid length
    const double gridLo = p.f0 - tolHz;
    const size_t nh = (size_t)((double)(n - wlen) / hop);
    std::vector<float> E(nh * (size_t)G);
    std::vector<cd> a(N);
    std::vector<double> win(wlen);
    for (size_t i = 0; i < wlen; i++) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)wlen);   // Hann: lower leakage between the tones
    // bin index of every fine grid point
    std::vector<int> bin((size_t)G);
    for (int g = 0; g < G; g++) bin[(size_t)g] = (int)std::lround((gridLo + g * fineHz) / binHz);
    for (size_t h = 0; h < nh; h++) {
        const size_t st = (size_t)std::floor((double)h * hop);
        for (size_t i = 0; i < N; i++) a[i] = i < wlen ? cd((double)x[st + i] * win[i], 0) : cd(0, 0);
        fft(a);
        for (int g = 0; g < G; g++) {
            const int b = bin[(size_t)g];
            E[h * (size_t)G + (size_t)g] = (b > 0 && (size_t)b < N / 2) ? (float)std::norm(a[(size_t)b]) : 0.f;
        }
    }
    // symbols per phase
    double bestScore = 0, bestR = 0;
    int bestPh = 0, bestS = 0, bestFo = 0;
    size_t bestNf = 0;
    for (int ph = 0; ph < 4; ph++) {
        const long nsym = ((long)nh - ph) / 4;
        if (nsym < 48) continue;
        // mean energy over the 42 tones of every symbol for every shift
        std::vector<float> mean((size_t)nsym * (size_t)(sMax + 1));
        for (long s = 0; s < nsym; s++) {
            const float* row = &E[((size_t)(4 * s + ph)) * (size_t)G];
            for (int sh = 0; sh <= sMax; sh++) {
                double m = 0;
                for (int k = 0; k < 42; k++) m += row[sh + 4 * k];
                mean[(size_t)s * (size_t)(sMax + 1) + (size_t)sh] = (float)(m / 42.0);
            }
        }
        {   // a floor under the mean: quiet stretches of a noise-free signal must not give huge ratios
            std::vector<float> t(mean);
            std::nth_element(t.begin(), t.begin() + (long)t.size() / 2, t.end());
            const float fl = 0.05f * t[t.size() / 2] + 1e-30f;
            for (float& v : mean) v += fl;
        }
        for (int sh = 0; sh <= sMax; sh++)
            for (int fo = 0; fo < 24; fo++) {
                double lg = 0;
                size_t nf = 0;
                for (long s0 = fo; s0 + 24 <= nsym; s0 += 24, nf++)
                    for (int i = 0; i < 4; i++) {
                        const long s = s0 + i;
                        lg += std::log1p((double)E[((size_t)(4 * s + ph)) * (size_t)G + (size_t)(sh + 4 * kCostas[i])] / (double)mean[(size_t)s * (size_t)(sMax + 1) + (size_t)sh]);
                    }
                if (nf < 2) continue;
                // the geometric mean of the four ratios: a lock must hit the tone in every one of the four symbols
                const double r = std::expm1(lg / (4.0 * (double)nf));
                const double score = (r - 1.0) * std::sqrt((double)nf);
                if (score > bestScore) { bestScore = score; bestR = r; bestPh = ph; bestS = sh; bestFo = fo; bestNf = nf; }
            }
    }
    if (std::getenv("HS_DEBUG")) std::fprintf(stderr, "iscat: v%d best r %.2f nf %zu ph %d s %d fo %d\n", variant, bestR, bestNf, bestPh, bestS, bestFo);
    if (bestNf < 2 || bestR < 2.2) return false;
    // read the frames
    const long nsym = ((long)nh - bestPh) / 4;
    auto row = [&](long s) { return &E[((size_t)(4 * s + bestPh)) * (size_t)G + (size_t)bestS]; };
    auto symMean = [&](long s) { double m = 0; const float* r = row(s); for (int k = 0; k < 42; k++) m += r[4 * k]; return m / 42.0 + 1e-30; };
    if (std::getenv("HS_DEBUG")) {
        for (long s0 = bestFo; s0 < bestFo + 48 && s0 < nsym; s0++) {
            int bk = 0; float bv = 0;
            for (int g = 0; g < G - 0 && g < 4 * 42 + sMax; g++) { const float v = E[((size_t)(4 * s0 + bestPh)) * (size_t)G + (size_t)g]; if (v > bv) { bv = v; bk = g; } }
            std::fprintf(stderr, "%d ", bk);
        }
        std::fprintf(stderr, "\n");
    }
    // length
    double bestLs = 0;
    int L = 0;
    for (int cand = 2; cand <= 41; cand++) {
        double sc = 0;
        for (long s0 = bestFo; s0 + 24 <= nsym; s0 += 24) {
            sc += row(s0 + 4)[4 * (cand % 42)] / symMean(s0 + 4);
            sc += row(s0 + 5)[4 * ((cand + 5) % 42)] / symMean(s0 + 5);
        }
        if (sc > bestLs) { bestLs = sc; L = cand; }
    }
    if (std::getenv("HS_DEBUG")) std::fprintf(stderr, "iscat: L %d score %.1f nf %zu\n", L, bestLs, bestNf);
    if (L < 2 || bestLs < 3.0 * (double)bestNf) return false;   // an average of 1.5 times the mean on both length symbols
    // fold
    std::vector<double> fold((size_t)L * 42, 0.0);
    long m = 0;
    for (long s0 = bestFo; s0 + 24 <= nsym; s0 += 24, m++)
        for (int i = 0; i < 18; i++) {
            const long s = s0 + 6 + i;
            const int pos = (int)((18 * m + i) % L);
            const float* r = row(s);
            const double mn = symMean(s);
            for (int k = 0; k < 42; k++) fold[(size_t)pos * 42 + (size_t)k] += r[4 * k] / mn;
        }
    // where is the "@"?
    int rot = 0;
    double bestAt = -1;
    for (int r0 = 0; r0 < L; r0++) {
        const double v = fold[(size_t)r0 * 42 + 40];
        if (v > bestAt) { bestAt = v; rot = r0; }
    }
    std::string text;
    int weak = 0;
    double confSum = 0;
    for (int q = 1; q < L; q++) {
        const double* f = &fold[(size_t)((rot + q) % L) * 42];
        int bk = 0;
        double mx = 0, sum = 0;
        for (int k = 0; k < 42; k++) { sum += f[k]; if (f[k] > mx) { mx = f[k]; bk = k; } }
        const double conf = mx / std::max(1e-9, (sum - mx) / 41.0);
        confSum += conf;
        if (conf < 3.0) { text += '?'; weak++; } else text += kSet[bk];
    }
    {   // the "@" must stand out too, and most characters must be clear
        const double* f = &fold[(size_t)rot * 42];
        double sum = 0;
        for (int k = 0; k < 42; k++) sum += f[k];
        const double conf = f[40] / std::max(1e-9, (sum - f[40]) / 41.0);
        if (conf < 3.0) return false;
    }
    if (weak * 5 > (L - 1) * 2) return false;    // more than 40 % unclear
    out.text = text;
    out.variant = variant;
    out.freqOffsetHz = gridLo + bestS * fineHz - p.f0;
    out.snrDb = 10 * std::log10(std::max(1e-6, bestR - 1.0));
    out.sync = bestR;
    out.frames = (int)bestNf;
    out.startSec = ((double)bestPh * hop + (double)bestFo * p.nsps) / fs;
    out.weakChars = weak;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ streaming
IscatDecoder::IscatDecoder(double fs, int variant) : fs_(fs), variant_(variant) { reset(); }
void IscatDecoder::reset() { buf_.clear(); total_ = nextTry_ = 0; last_.clear(); level_ = -120; out_.clear(); }
std::vector<IscatResult> IscatDecoder::take() { std::vector<IscatResult> r; r.swap(out_); return r; }

void IscatDecoder::feed(const float* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    total_ += n;
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
    level_ = 10 * std::log10(std::max(1e-12, e / (double)std::max<size_t>(1, n)));
    const size_t maxN = (size_t)(24 * fs_);
    if (buf_.size() > maxN) buf_.erase(buf_.begin(), buf_.begin() + (long)(buf_.size() - maxN));
    if (total_ < nextTry_ || buf_.size() < (size_t)(6 * fs_)) return;
    nextTry_ = total_ + (size_t)(4 * fs_);
    IscatResult best;
    bool ok = false;
    for (int v = 0; v < 2; v++) {
        if (variant_ != 2 && variant_ != v) continue;
        IscatResult r;
        if (iscatDecode(buf_.data(), buf_.size(), fs_, v, r) && (!ok || r.sync > best.sync)) { best = r; ok = true; }
    }
    if (ok && best.text != last_) {
        best.startSec += (double)(total_ - buf_.size()) / fs_;
        out_.push_back(best);
        last_ = best.text;
    }
}

}  // namespace hamsci
}  // namespace dect2
