// See autoparam.h.
#include "dect2/autoparam/autoparam.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>

namespace dect2::autoparam {

namespace {
using dect2::math::kPi;

std::vector<double> powerSpectrum(const float* x, size_t n, int nfft, int hop) {
    std::vector<double> p((size_t)nfft / 2, 0.0);
    std::vector<float> win((size_t)nfft);
    for (int i = 0; i < nfft; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / nfft));
    Fft f(nfft);
    std::vector<cf32> buf((size_t)nfft);
    int frames = 0;
    for (size_t s = 0; s + (size_t)nfft <= n; s += (size_t)hop) {
        for (int i = 0; i < nfft; i++) buf[(size_t)i] = cf32(x[s + (size_t)i] * win[(size_t)i], 0.f);
        f.forward(buf.data());
        for (int k = 0; k < nfft / 2; k++) p[(size_t)k] += std::norm(buf[(size_t)k]);
        frames++;
    }
    if (frames) for (auto& v : p) v /= frames;
    return p;
}
}  // namespace

std::vector<FskPair> findFskPairs(const float* x, size_t n, double fs, const double* shifts, int nShifts, int maxPairs, double tolHz) {
    std::vector<FskPair> out;
    constexpr int kN = 2048;
    if (n < (size_t)kN || nShifts <= 0) return out;
    const double bin = fs / kN;
    const int lo = (int)std::ceil(300 / bin), hi = std::min(kN / 2 - 2, (int)(3300 / bin));
    const auto pw = powerSpectrum(x, n, kN, kN / 2);
    std::vector<double> sc(pw.begin() + lo, pw.begin() + hi + 1);
    std::nth_element(sc.begin(), sc.begin() + (long)sc.size() / 2, sc.end());
    const double fl = std::max(1e-12, sc[sc.size() / 2]);
    struct Line { double hz, p; };
    std::vector<Line> lines;
    for (int k = lo + 1; k < hi; k++) {
        const double a = pw[(size_t)k];
        if (a < pw[(size_t)k - 1] || a <= pw[(size_t)k + 1] || a < 4.0 * fl) continue;
        const double l0 = std::log(pw[(size_t)k - 1] + 1e-30), l1 = std::log(a), l2 = std::log(pw[(size_t)k + 1] + 1e-30);
        const double den = l0 - 2 * l1 + l2;
        const double off = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (l0 - l2) / den)) : 0.0;
        lines.push_back({(k + off) * bin, a});
    }
    double maxShift = 0;
    for (int q = 0; q < nShifts; q++) maxShift = std::max(maxShift, shifts[q]);
    std::vector<FskPair> pairs;
    for (size_t i = 0; i < lines.size(); i++)
        for (size_t j = i + 1; j < lines.size(); j++) {
            const double d = lines[j].hz - lines[i].hz;
            if (d > maxShift + tolHz + 2) break;
            int si = -1;
            double bestErr = tolHz;
            for (int q = 0; q < nShifts; q++)
                if (std::fabs(d - shifts[q]) < bestErr) { bestErr = std::fabs(d - shifts[q]); si = q; }
            if (si < 0) continue;
            FskPair p;
            p.lowHz = lines[i].hz; p.shiftIdx = si; p.shiftHz = shifts[si]; p.score = std::min(lines[i].p, lines[j].p) / fl;
            pairs.push_back(p);
        }
    std::sort(pairs.begin(), pairs.end(), [](const FskPair& a, const FskPair& b) { return a.score > b.score; });
    for (const auto& p : pairs) {
        bool dup = false;
        for (const auto& q : out) dup |= q.shiftIdx == p.shiftIdx && std::fabs(q.lowHz - p.lowHz) < 25.0;
        if (!dup && p.score >= 0.15 * pairs[0].score) out.push_back(p);   // the weaker pairs are side lobes of the lines of the stronger ones
        if ((int)out.size() >= maxPairs) break;
    }
    return out;
}

FskBaud measureFskBaud(const float* x, size_t n, double fs, double lowHz, double shiftHz, const double* bauds, int nBauds) {
    FskBaud r;
    if (n < (size_t)(fs * 2) || nBauds <= 0) return r;
    const int len = std::max(8, (int)std::lround(fs * 0.0035));   // 3.5 ms: under a third of the shortest bit of these tables
    std::vector<std::complex<float>> ba((size_t)len), bb((size_t)len);
    std::complex<float> sa(0, 0), sb(0, 0);
    double pa = 0, pb = 0;
    const double wa = 2 * kPi * lowHz / fs, wb = 2 * kPi * (lowHz + shiftHz) / fs;
    std::vector<int> runs;
    int cur = 0, run = 0, bi = 0;
    for (size_t i = 0; i < n; i++) {
        const std::complex<float> ma(x[i] * (float)std::cos(pa), -x[i] * (float)std::sin(pa));
        const std::complex<float> mb(x[i] * (float)std::cos(pb), -x[i] * (float)std::sin(pb));
        pa = std::fmod(pa + wa, 2 * kPi); pb = std::fmod(pb + wb, 2 * kPi);
        sa += ma - ba[(size_t)bi]; ba[(size_t)bi] = ma;
        sb += mb - bb[(size_t)bi]; bb[(size_t)bi] = mb;
        if (++bi >= len) bi = 0;
        const int d = std::norm(sa) > std::norm(sb) ? 1 : -1;
        if (i < (size_t)len) { cur = d; continue; }
        if (d == cur) { run++; continue; }
        if (run > 0) runs.push_back(run);
        cur = d; run = 1;
    }
    // short flips are noise: fold them into their neighbours
    std::vector<double> sec;
    double acc = 0;
    const double minLen = 0.4 / 110.0;
    for (int v : runs) {
        const double s = v / fs;
        if (s < minLen) { acc += s; continue; }
        sec.push_back(s + acc); acc = 0;
    }
    if (sec.size() < 8) return r;
    // Bit times and half bit times (the stop bit is 1.5 long, so the grid is half a bit)
    std::vector<double> sc((size_t)nBauds, 0.0);
    for (int b = 0; b < nBauds; b++) {
        double c = 0, w = 0;
        for (double s : sec) { const double bits = s * bauds[b] * 2; c += std::cos(2 * kPi * bits); w += 1; }
        sc[(size_t)b] = c / w;
    }
    int i1 = 0, i2 = -1;
    for (int b = 1; b < nBauds; b++) if (sc[(size_t)b] > sc[(size_t)i1]) i1 = b;
    for (int b = 0; b < nBauds; b++) if (b != i1 && (i2 < 0 || sc[(size_t)b] > sc[(size_t)i2])) i2 = b;
    r.best = i1; r.second = i2;
    r.confidence = i2 < 0 ? 1.0 : std::max(0.0, std::min(1.0, (sc[(size_t)i1] - sc[(size_t)i2]) / std::max(0.05, std::fabs(sc[(size_t)i1]))));
    return r;
}

ToneComb analyseToneComb(const float* x, size_t n, double fs) {
    ToneComb r;
    constexpr int kN = 4096;
    if (n < (size_t)kN * 2) return r;
    const double bin = fs / kN;
    const auto pw = powerSpectrum(x, n, kN, kN / 2);
    const int lo = (int)(200 / bin), hi = std::min(kN / 2 - 2, (int)(3600 / bin));
    std::vector<double> sc(pw.begin() + lo, pw.begin() + hi + 1);
    std::nth_element(sc.begin(), sc.begin() + (long)sc.size() / 5, sc.end());
    const double fl = std::max(1e-12, sc[sc.size() / 5] * 1.6);   // the floor: a low percentile, a wide signal fills most of the band
    // smooth over about 10 Hz; a tone stands out against the floor
    std::vector<double> sm((size_t)(hi - lo + 1), 0.0);
    const int w = std::max(1, (int)std::lround(5 / bin));
    for (int k = lo; k <= hi; k++) {
        double s = 0; int c = 0;
        for (int j = std::max(0, k - w); j <= std::min(kN / 2 - 1, k + w); j++) { s += pw[(size_t)j]; c++; }
        sm[(size_t)(k - lo)] = s / c;
    }
    const double thr = 3.0 * fl;
    // the clusters of bins above the threshold, joined across gaps of up to 300 Hz, and the one with the most power
    int bestA = -1, bestB = -1;
    double bestP = 0;
    int a = -1, b = -1;
    double acc = 0;
    const int gap = (int)(300 / bin);
    for (int k = 0; k < (int)sm.size(); k++) {
        if (sm[(size_t)k] < thr) continue;
        if (a < 0 || k - b > gap) { if (a >= 0 && acc > bestP) { bestP = acc; bestA = a; bestB = b; } a = k; acc = 0; }
        b = k; acc += sm[(size_t)k];
    }
    if (a >= 0 && acc > bestP) { bestP = acc; bestA = a; bestB = b; }
    if (bestA < 0) return r;
    r.loHz = (lo + bestA) * bin; r.hiHz = (lo + bestB) * bin;
    r.occupiedHz = r.hiHz - r.loHz; r.centreHz = (r.loHz + r.hiHz) / 2;
    double maxV = 0;
    for (int k = bestA; k <= bestB; k++) maxV = std::max(maxV, sm[(size_t)k]);
    r.snrDb = 10 * std::log10(maxV / fl);
    // pitch: autocorrelation of the log spectrum inside the band
    const int L = bestB - bestA + 1;
    if (L > 8) {
        std::vector<double> v((size_t)L);
        double mean = 0;
        for (int k = 0; k < L; k++) { v[(size_t)k] = std::log(pw[(size_t)(lo + bestA + k)] + fl * 0.1); mean += v[(size_t)k]; }
        mean /= L;
        for (auto& d : v) d -= mean;
        double r0 = 0;
        for (double d : v) r0 += d * d;
        const int minLag = std::max(3, (int)(7.0 / bin)), maxLag = std::min(L / 2, (int)(520.0 / bin));
        std::vector<double> ac((size_t)maxLag + 2, 0.0);
        for (int lag = minLag - 1; lag <= maxLag + 1 && lag + 1 < L; lag++) {
            double c = 0;
            for (int k = 0; k + lag < L; k++) c += v[(size_t)k] * v[(size_t)(k + lag)];
            ac[(size_t)lag] = c / ((double)(L - lag) / L);
        }
        double best = 0;
        for (int lag = minLag; lag <= maxLag; lag++) best = std::max(best, ac[(size_t)lag]);
        if (best > 0.2 * r0)
            for (int lag = minLag; lag <= maxLag; lag++)   // the first local peak that is nearly as strong as the strongest: the pitch, not a multiple of it
                if (ac[(size_t)lag] > ac[(size_t)lag - 1] && ac[(size_t)lag] >= ac[(size_t)lag + 1] && ac[(size_t)lag] >= 0.6 * best) { r.spacingHz = lag * bin; break; }
    }
    r.found = r.snrDb >= 4.0;
    return r;
}

double readableFraction(const std::string& t) {
    if (t.empty()) return 0;
    size_t g = 0;
    for (char c : t) {
        const auto u = (unsigned char)c;
        if (std::isalnum(u) || c == ' ' || c == '\n' || c == '\r' || c == '.' || c == ',' || c == '-' || c == '/' || c == '?' || c == '\'' || c == ':') g++;
    }
    return (double)g / (double)t.size();
}

}  // namespace dect2::autoparam
