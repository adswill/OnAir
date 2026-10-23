// Finding the strongest narrow signal in a span (carrier, bandwidth, SNR), zooming in for narrow signals in wide captures.
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::pskd {

namespace {
struct Region { int lo = 0, hi = 0; double power = 0; };

struct Spec {
    std::vector<double> s;   // smoothed, linear, DC in the middle
    double noise = 0;
    size_t nfft = 0;
};

Spec spectrum(const cf* x, size_t n, size_t nfft, double fill) {
    Spec r;
    r.nfft = nfft;
    std::vector<double> p = signallab::psd(x, n, nfft);
    const size_t c = nfft / 2;
    // a DC spike (one or two bins at 0 Hz) is replaced by the neighbours
    double nb = 0; int k = 0;
    for (size_t d = 4; d <= 8; d++) { nb += p[c - d] + p[c + d]; k += 2; }
    nb /= k;
    for (int d = -1; d <= 1; d++) { double& v = p[(size_t)((long)c + d)]; if (v > 6 * nb) v = nb; }
    r.s.assign(nfft, 0);
    const int h = 2;
    for (size_t i = 0; i < nfft; i++) {
        double a = 0; int m = 0;
        for (int d = -h; d <= h; d++) { long j = (long)i + d; if (j >= 0 && j < (long)nfft) { a += p[(size_t)j]; m++; } }
        r.s[i] = a / m;
    }
    // the part of the span that holds samples (the rest is a filter's empty edge, not noise)
    long lo = 0, hi = (long)nfft - 1;
    if (fill < 0.98) {
        const long half = (long)(std::max(0.05, fill) * (double)nfft / 2);
        lo = std::max(0L, (long)c - half + 2); hi = std::min((long)nfft - 1, (long)c + half - 2);
    }
    std::vector<double> sorted(r.s.begin() + lo, r.s.begin() + hi + 1);
    std::sort(sorted.begin(), sorted.end());
    r.noise = std::max(1e-30, sorted[sorted.size() / 5] * 1.15);   // 20th percentile: robust while the signal fills less than 80 % of the span
    for (long i = 0; i < lo; i++) r.s[(size_t)i] = 0;
    for (long i = hi + 1; i < (long)nfft; i++) r.s[(size_t)i] = 0;
    return r;
}

std::vector<Region> regions(const Spec& sp, double thr) {
    std::vector<Region> out;
    const int n = (int)sp.nfft, gap = std::max(2, n / 100);
    int i = 0;
    while (i < n) {
        if (sp.s[(size_t)i] < thr * sp.noise) { i++; continue; }
        Region r; r.lo = i;
        int last = i, j = i;
        while (j < n && j - last <= gap) { if (sp.s[(size_t)j] >= thr * sp.noise) last = j; j++; }
        r.hi = last;
        for (int k = r.lo; k <= r.hi; k++) r.power += std::max(0.0, sp.s[(size_t)k] - sp.noise);
        out.push_back(r);
        i = last + 1;
    }
    return out;
}

size_t fftSize(size_t n) {
    size_t f = 1;
    while (f * 2 <= n / 4 && f < 8192) f *= 2;
    return std::max<size_t>(f, 256);
}
} // namespace

Carrier findCarrier(const cf* x, size_t n, double fs, bool nearOnly, double nearHz, double maxDistHz, double fill) {
    Carrier c;
    if (n < 1024 || fs <= 0) return c;
    size_t nfft = fftSize(n);
    std::vector<cf> work;
    const cf* cur = x; size_t curN = n; double curFs = fs, curOff = 0, curFill = fill;   // curOff: this block's centre from the original centre
    Spec sp; Region best;
    for (int pass = 0; pass < 4; pass++) {
        sp = spectrum(cur, curN, nfft, curFill);
        auto regs = regions(sp, 2.5);
        if (regs.empty()) return c;
        const double bin = curFs / (double)nfft;
        int pick = -1; double bestScore = -1;
        for (size_t i = 0; i < regs.size(); i++) {
            const double f0 = curOff + (regs[i].lo - (double)nfft / 2) * bin, f1 = curOff + (regs[i].hi - (double)nfft / 2) * bin;
            double score = regs[i].power;
            if (nearOnly) {
                const double d = (nearHz >= f0 && nearHz <= f1) ? 0 : std::min(std::fabs(nearHz - f0), std::fabs(nearHz - f1));
                if (maxDistHz > 0 && d > maxDistHz) continue;
                score = 1.0 / (1.0 + d);
            }
            if (score > bestScore) { bestScore = score; pick = (int)i; }
        }
        if (pick < 0) return c;
        best = regs[(size_t)pick];
        const int widthBins = best.hi - best.lo + 1;
        if (widthBins >= 40 || pass == 3) break;
        const double cen = curOff + ((best.lo + best.hi) / 2.0 - (double)nfft / 2) * bin;
        const double bw = std::max(widthBins * bin, 4 * bin);
        const double want = bw * 8;
        if (want * 1.5 >= curFs) break;
        double fsOut = 0;
        std::vector<cf> tmp(cur, cur + curN);
        std::vector<cf> z = signallab::channelise(tmp, curFs, cen - curOff, want / 2, fsOut);
        if (z.size() < 1024 || fsOut >= curFs * 0.9) break;
        work.swap(z);
        curOff = cen; curFill = std::min(1.0, 0.54 * want / fsOut); curFs = fsOut; cur = work.data(); curN = work.size();
        nfft = fftSize(curN);
    }
    const double bin = curFs / (double)nfft;
    const int lo = std::max(0, best.lo - 2), hi = std::min((int)nfft - 1, best.hi + 2);
    double top = 0, sum = 0, mom = 0;
    for (int k = lo; k <= hi; k++) {
        const double v = std::max(0.0, sp.s[(size_t)k] - sp.noise);
        top = std::max(top, v); sum += v; mom += v * k;
    }
    if (sum <= 0) return c;
    c.found = true;
    c.offsetHz = curOff + (mom / sum - (double)nfft / 2) * bin;
    auto widthAt = [&](double frac) {
        const double thr = frac * top;
        int l = best.lo, r = best.hi;
        for (int k = best.lo; k <= best.hi; k++) if (sp.s[(size_t)k] - sp.noise >= thr) { l = k; break; }
        for (int k = best.hi; k >= best.lo; k--) if (sp.s[(size_t)k] - sp.noise >= thr) { r = k; break; }
        return (r - l + 1) * bin;
    };
    c.bw3Hz = widthAt(0.5);
    c.bw10Hz = widthAt(0.1);
    double acc = 0; int l = lo, r = hi;   // 99 % occupied: drop 0.5 % of the power from each end
    for (int k = lo; k <= hi; k++) { acc += std::max(0.0, sp.s[(size_t)k] - sp.noise); if (acc >= 0.005 * sum) { l = k; break; } }
    acc = 0;
    for (int k = hi; k >= lo; k--) { acc += std::max(0.0, sp.s[(size_t)k] - sp.noise); if (acc >= 0.005 * sum) { r = k; break; } }
    c.occHz = (r - l + 1) * bin;
    c.snrDb = 10 * std::log10(std::max(1e-9, sum / (sp.noise * (hi - lo + 1))));
    c.peakDb = 10 * std::log10(std::max(1.0, (top + sp.noise) / sp.noise));
    return c;
}

} // namespace dect2::pskd
