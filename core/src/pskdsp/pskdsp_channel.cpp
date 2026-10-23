// PSK demodulator: find the strongest narrow signal in a block of samples (its carrier, widths and SNR), zooming in for narrow ones.
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>

namespace dect2::pskd {

namespace {

struct Region { size_t a = 0, b = 0; double power = 0; };

Carrier measure(const std::vector<double>& P, double fs, bool nearOnly, double nearHz, double maxDistHz, double bandHz, size_t* widthBins) {
    Carrier c;
    const size_t N = P.size();
    if (N < 32) return c;
    const double hzBin = fs / (double)N;
    std::vector<double> p = P;
    // a DC spike (one to three bins at 0 Hz standing well above both sides) is not a signal
    {
        const size_t m = N / 2;
        const double side = 0.5 * (p[m - 4] + p[m + 4]);
        for (size_t i = m - 1; i <= m + 1; i++)
            if (p[i] > 10 * side && side > 0) p[i] = side;
    }
    // light smoothing (5 bins) for the decisions, the raw values for the power sums
    std::vector<double> s(N, 0.0);
    for (size_t i = 0; i < N; i++) {
        double a = 0; int k = 0;
        for (long j = (long)i - 2; j <= (long)i + 2; j++) if (j >= 0 && j < (long)N) { a += p[(size_t)j]; k++; }
        s[i] = a / k;
    }
    // the bins that hold signal and noise: the outer 1/32 on each side holds the anti-alias roll-off, and a channeliser's stop band is not
    // a noise floor
    size_t i0 = N / 32, i1 = N - N / 32;
    if (bandHz > 0 && bandHz < fs) {
        const size_t h = (size_t)(0.5 * bandHz / hzBin * 0.97);
        i0 = std::max(i0, N / 2 - std::min(N / 2 - 1, h)); i1 = std::min(i1, N / 2 + std::min(N / 2 - 1, h));
    }
    // noise floor: the 30th percentile of the smoothed spectrum (a signal can fill more than half of a zoomed span)
    std::vector<double> sorted(s.begin() + (long)i0, s.begin() + (long)i1);
    std::nth_element(sorted.begin(), sorted.begin() + (long)(sorted.size() * 3 / 10), sorted.end());
    const double floor = std::max(1e-30, sorted[sorted.size() * 3 / 10]);
    const double thr = floor * 4.0;   // 6 dB
    std::vector<Region> regs;
    for (size_t i = i0; i < i1;) {
        if (s[i] <= thr) { i++; continue; }
        Region r; r.a = i;
        size_t gap = 0, j = i;
        for (; j < i1; j++) {
            if (s[j] > thr) gap = 0;
            else if (++gap > 3) break;
        }
        r.b = j - gap;
        for (size_t k = r.a; k <= r.b; k++) r.power += std::max(0.0, p[k] - floor);
        if (r.b > r.a) regs.push_back(r);
        i = j + 1;
    }
    if (regs.empty()) return c;
    const Region* best = nullptr;
    if (nearOnly) {
        double bestD = 1e300;
        for (const auto& r : regs) {
            const double fa = ((double)r.a - N / 2.0) * hzBin, fb = ((double)r.b - N / 2.0) * hzBin;
            const double d = nearHz < fa ? fa - nearHz : nearHz > fb ? nearHz - fb : 0;
            if (d < bestD && (maxDistHz <= 0 || d <= maxDistHz)) { bestD = d; best = &r; }
        }
    } else {
        for (const auto& r : regs) if (!best || r.power > best->power) best = &r;
    }
    if (!best) return c;
    const Region& r = *best;
    // widths on a heavier smoothing (a 24th of the region), so the ripple of a noisy flat top does not end the -3 dB width early
    {
        const long ws = std::max<long>(2, (long)(r.b - r.a) / 48);
        std::vector<double> cs(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) cs[i + 1] = cs[i] + p[i];
        for (size_t i = 0; i < N; i++) {
            const long a = std::max(0L, (long)i - ws), b = std::min((long)N - 1, (long)i + ws);
            s[i] = (cs[(size_t)b + 1] - cs[(size_t)a]) / (double)(b - a + 1);
        }
    }
    // centroid of the power over the region: the carrier of a symmetric PSK spectrum
    double sw = 0, swf = 0, top = 0;
    size_t pk = r.a;
    for (size_t k = r.a; k <= r.b; k++) {
        const double v = std::max(0.0, p[k] - floor);
        sw += v; swf += v * (double)k;
        if (s[k] > top) { top = s[k]; pk = k; }
    }
    const double cbin = sw > 0 ? swf / sw : (double)pk;
    auto widthAt = [&](double frac) {
        const double lvl = floor + (top - floor) * frac;
        size_t a = pk, b = pk;
        while (a > 0 && s[a - 1] > lvl) a--;
        while (b + 1 < N && s[b + 1] > lvl) b++;
        return (double)(b - a + 1) * hzBin;
    };
    c.found = true;
    c.offsetHz = (cbin - N / 2.0) * hzBin;
    c.bw3Hz = widthAt(0.5);
    c.bw10Hz = widthAt(0.1);
    // 99 % occupied band over the region and half its width again on both sides
    {
        const size_t ext = (r.b - r.a) / 2 + 2;
        const size_t a = r.a > ext ? r.a - ext : 0, b = std::min(N - 1, r.b + ext);
        double tot = 0;
        for (size_t k = a; k <= b; k++) tot += std::max(0.0, p[k] - floor);
        double acc = 0;
        size_t lo = a, hi = b;
        for (size_t k = a; k <= b; k++) { acc += std::max(0.0, p[k] - floor); if (acc >= 0.005 * tot) { lo = k; break; } }
        acc = 0;
        for (size_t k = b + 1; k-- > a;) { acc += std::max(0.0, p[k] - floor); if (acc >= 0.005 * tot) { hi = k; break; } }
        c.occHz = (double)(hi >= lo ? hi - lo + 1 : 1) * hzBin;
        const double nb = std::max(1.0, (double)(hi >= lo ? hi - lo + 1 : 1));
        c.snrDb = 10 * std::log10(std::max(1e-12, 0.99 * tot / (floor * nb)));
    }
    c.peakDb = 10 * std::log10(std::max(1e-12, top / floor));
    if (widthBins) *widthBins = r.b - r.a + 1;
    return c;
}

Carrier find(const cf* x, size_t n, double fs, bool nearOnly, double nearHz, double maxDistHz, double bandHz, int depth) {
    Carrier c;
    if (n < 64 || fs <= 0) return c;
    const size_t nfft = std::clamp<size_t>(signallab::nextPow2(n / 32), 256, 16384);
    const std::vector<double> P = signallab::psd(x, n, nfft);
    size_t wb = 0;
    c = measure(P, fs, nearOnly, nearHz, maxDistHz, bandHz, &wb);
    if (!c.found || depth >= 3) return c;
    // narrow against the span: zoom in around it so its shape is measured over enough bins
    const double width = std::max(c.occHz, c.bw10Hz);
    const double span = std::max(width * 6, 64 * fs / (double)nfft);
    if (wb < 40 && span < fs * 0.4 && n / (size_t)std::max(1.0, fs / span) >= 2048) {
        double fsz = fs;
        const std::vector<cf> in(x, x + n);
        const std::vector<cf> z = signallab::channelise(in, fs, c.offsetHz, span / 2, fsz);
        Carrier zc = find(z.data(), z.size(), fsz, true, 0, width, span, depth + 1);
        if (zc.found) {
            zc.offsetHz += c.offsetHz;
            return zc;
        }
    }
    return c;
}

} // namespace

Carrier findCarrier(const cf* x, size_t n, double fs, bool nearOnly, double nearHz, double maxDistHz, double bandHz) {
    return find(x, n, fs, nearOnly, nearHz, maxDistHz, bandHz, 0);
}

} // namespace dect2::pskd
