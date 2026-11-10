// VITS analyser (see vits.h).
#include "dect2/smart5/vits.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846;

std::vector<float> movingAvg(const std::vector<float>& x, int w) {
    const int n = (int)x.size();
    std::vector<float> y(n);
    std::vector<double> c(n + 1, 0);
    for (int i = 0; i < n; i++) c[i + 1] = c[i] + x[i];
    for (int i = 0; i < n; i++) {
        const int a = std::max(0, i - w / 2), b = std::min(n, a + w);
        y[i] = (float)((c[b] - c[a]) / (b - a));
    }
    return y;
}

struct Seg { int a, b; };

// least-squares fit of c + p cos(wk) + q sin(wk) over [a, b]; returns the phasor p - jq (x ~ Re(A e^{jwk}) + c)
std::complex<double> fitSine(const std::vector<float>& x, int a, int b, double f, double fs, double* dc = nullptr) {
    double S[3][3] = {{0}}, R[3] = {0};
    const double w = 2 * kPi * f / fs;
    for (int k = a; k <= b; k++) {
        const double v[3] = {1, std::cos(w * k), std::sin(w * k)};
        for (int i = 0; i < 3; i++) { R[i] += v[i] * x[k]; for (int j = 0; j < 3; j++) S[i][j] += v[i] * v[j]; }
    }
    // Gaussian elimination on the 3 x 3 normal equations
    for (int i = 0; i < 3; i++) {
        int piv = i;
        for (int r = i + 1; r < 3; r++) if (std::fabs(S[r][i]) > std::fabs(S[piv][i])) piv = r;
        for (int j = 0; j < 3; j++) std::swap(S[i][j], S[piv][j]);
        std::swap(R[i], R[piv]);
        if (std::fabs(S[i][i]) < 1e-12) return {0, 0};
        for (int r = i + 1; r < 3; r++) {
            const double m = S[r][i] / S[i][i];
            for (int j = i; j < 3; j++) S[r][j] -= m * S[i][j];
            R[r] -= m * R[i];
        }
    }
    double c[3];
    for (int i = 2; i >= 0; i--) { double t = R[i]; for (int j = i + 1; j < 3; j++) t -= S[i][j] * c[j]; c[i] = t / S[i][i]; }
    if (dc) *dc = c[0];
    return {c[1], -c[2]};
}

// flat runs of the smoothed signal: |ms - running mean| <= tol, at least minLen samples
std::vector<Seg> flatRuns(const std::vector<float>& ms, double tol, int minLen) {
    std::vector<Seg> out;
    const int n = (int)ms.size();
    int a = 0;
    while (a < n) {
        int b = a; double lo = ms[a], hi = ms[a];
        while (b + 1 < n) {
            const double v = ms[b + 1];
            if (std::max(hi, v) - std::min(lo, v) > tol) break;
            lo = std::min(lo, v); hi = std::max(hi, v); b++;
        }
        if (b - a + 1 >= minLen) { out.push_back({a, b}); a = b + 1; }
        else a++;
    }
    return out;
}
}

// the frequency that maximises the fitted amplitude within +-4 % of a first guess
double refineFreq(const std::vector<float>& x, int a, int b, double f0, double fs) {
    double best = f0, bestA = -1;
    for (int k = -40; k <= 40; k++) {
        const double f = f0 * (1 + 0.001 * k);
        const double A = std::abs(fitSine(x, a, b, f, fs));
        if (A > bestA) { bestA = A; best = f; }
    }
    double step = f0 * 0.0005;
    for (int it = 0; it < 12; it++, step *= 0.5)
        for (double f : {best - step, best + step}) {
            const double A = std::abs(fitSine(x, a, b, f, fs));
            if (A > bestA) { bestA = A; best = f; }
        }
    return best;
}

VitsResult analyseVitsLine(const std::vector<float>& x, double fs) {
    VitsResult r;
    const int n = (int)x.size();
    if (n < 64 || fs < 1e6) return r;
    const int us = (int)std::lround(fs * 1e-6);
    const std::vector<float> ms = movingAvg(movingAvg(x, us), us);   // two 1 us averages: remove the subcarrier, keep the steps
    std::vector<float> res(n);
    for (int i = 0; i < n; i++) res[i] = x[i] - ms[i];

    // ---- bar: the longest flat run at 60 IRE or more, at least 8 us, little high-frequency content
    Seg bar{-1, -1};
    for (const Seg& s : flatRuns(ms, 3.0, 6 * us)) {
        double m = 0, e = 0;
        for (int i = s.a; i <= s.b; i++) { m += x[i]; e += res[i] * res[i]; }
        m /= (s.b - s.a + 1); e = std::sqrt(e / (s.b - s.a + 1));
        if (m >= 60 && e < 2 && (bar.a < 0 || s.b - s.a > bar.b - bar.a)) bar = s;
    }
    if (bar.a >= 0) {
        const int len = bar.b - bar.a + 1, a = bar.a + len / 10, b = bar.b - len / 10;
        double m = 0; for (int i = a; i <= b; i++) m += x[i];
        r.bar = true; r.barIre = m / (b - a + 1);
        // tilt: the level 1 us in from each end, against the bar level
        double s0 = 0, s1 = 0;
        for (int i = 0; i < us; i++) { s0 += x[a + i]; s1 += x[b - i]; }
        r.barTiltPct = 100.0 * (s0 - s1) / us / r.barIre;
    }

    // ---- 2T pulse: a narrow peak (half-amplitude duration under 1 us) above half the bar, outside the bar
    {
        int best = -1; double pk = 0;
        for (int i = 1; i + 1 < n; i++) {
            if (bar.a >= 0 && i >= bar.a - us && i <= bar.b + us) continue;
            if (!(x[i] > pk && x[i] >= x[i - 1] && x[i] >= x[i + 1])) continue;
            // isolated: 1 us either side the line is back below a third of the peak (not a staircase or burst crest)
            if (i - us < 0 || i + us >= n || x[i - us] > x[i] / 3 || x[i + us] > x[i] / 3) continue;
            pk = x[i]; best = i;
        }
        if (best > 0 && pk > 40) {
            // sub-sample peak by parabola, half-amplitude crossings by linear interpolation
            const double y0 = x[best - 1], y1 = x[best], y2 = x[best + 1], den = y0 - 2 * y1 + y2;
            const double peak = den < 0 ? y1 - 0.125 * (y0 - y2) * (y0 - y2) / den : y1;
            const double base = 0, half = base + 0.5 * (peak - base);
            int l = best, h = best;
            while (l > 0 && x[l] > half) l--;
            while (h + 1 < n && x[h] > half) h++;
            const double tl = l + (half - x[l]) / (x[l + 1] - x[l]);
            const double th = (h - 1) + (x[h - 1] - half) / (x[h - 1] - x[h]);
            const double had = (th - tl) / fs * 1e9;
            if (had > 0 && had < 1000) {
                r.pulse2T = true; r.pulseIre = peak; r.hadNs = had;
                if (r.bar) r.kpbPct = 25.0 * std::fabs(r.barIre - peak) / peak;
            }
        }
    }

    // ---- staircase with subcarrier: five or more flat steps (2 us+) that carry a strong high-frequency component
    {
        std::vector<float> r2(n);
        for (int i = 0; i < n; i++) r2[i] = res[i] * res[i];
        const std::vector<float> env = movingAvg(r2, us);   // high-frequency power, 1 us
        std::vector<Seg> steps;
        for (const Seg& s : flatRuns(ms, 2.5, 2 * us)) {
            if (bar.a >= 0 && s.b >= bar.a && s.a <= bar.b) continue;
            // only the part of the step that carries the subcarrier
            int i = s.a;
            while (i <= s.b) {
                if (env[i] <= 9) { i++; continue; }
                int j = i;
                while (j + 1 <= s.b && env[j + 1] > 9) j++;
                if (j - i + 1 >= 2 * us) steps.push_back({i, j});
                i = j + 1;
            }
        }
        // a staircase climbs (or falls) step by step; packets on one pedestal (multiburst) do not
        bool stair = steps.size() >= 5;
        int dir = 0;
        for (size_t k = 1; stair && k < steps.size(); k++) {
            const double d = ms[(steps[k].a + steps[k].b) / 2] - ms[(steps[k - 1].a + steps[k - 1].b) / 2];
            const int sd = d >= 5 ? 1 : d <= -5 ? -1 : 0;
            if (sd == 0 || (dir != 0 && sd != dir)) stair = false;
            dir = sd;
        }
        if (stair) {
            // subcarrier frequency from the zero crossings of the first step
            const Seg& s0 = steps[0];
            int zc = 0; int first = -1, last = -1;
            for (int i = s0.a + us / 2; i < s0.b - us / 2; i++)
                if ((res[i] < 0) != (res[i + 1] < 0)) { zc++; if (first < 0) first = i; last = i; }
            if (zc >= 4) {
                double fsc = refineFreq(x, s0.a, s0.b, (zc - 1) / 2.0 / ((last - first) / fs), fs);
                // the staircase subcarrier is locked to the colour burst: snap to the standard it is near (PAL, NTSC, PAL-M, PAL-N)
                for (double std_ : {4.43361875e6, 3.579545e6, 3.57561149e6, 3.58205625e6})
                    if (std::fabs(fsc - std_) < 0.01 * std_) { fsc = std_; break; }
                std::vector<double> amp, ph;
                for (const Seg& s : steps) {
                    const int a = s.a + us / 2, b = s.b - us / 2;
                    if (b <= a) continue;
                    const std::complex<double> acc = fitSine(x, a, b, fsc, fs);
                    amp.push_back(std::abs(acc)); ph.push_back(std::arg(acc));
                }
                if (amp.size() >= 5) {
                    r.staircase = true; r.steps = (int)amp.size(); r.subcarrierHz = fsc;
                    const double amax = *std::max_element(amp.begin(), amp.end()), amin = *std::min_element(amp.begin(), amp.end());
                    r.dgPct = 100.0 * (amax - amin) / amp[0];
                    double pmin = 0, pmax = 0;
                    for (size_t k = 0; k < ph.size(); k++) {
                        double d = ph[k] - ph[0];
                        while (d > kPi) d -= 2 * kPi;
                        while (d < -kPi) d += 2 * kPi;
                        pmin = std::min(pmin, d); pmax = std::max(pmax, d);
                    }
                    r.dpDeg = (pmax - pmin) * 180 / kPi;
                }
            }
        }
    }

    // ---- multiburst: on a line without a bar, three or more packets of sine waves on a pedestal, rising in frequency
    if (!r.bar && !r.staircase) {
        std::vector<float> s(x.begin() + n / 10, x.end());
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const double ped = s[s.size() / 2];
        const int gapMin = (int)(0.4 * us) + 1;
        std::vector<Seg> pk;
        int i = 0;
        while (i < n) {
            if (std::fabs(x[i] - ped) <= 8) { i++; continue; }
            int a = i, b = i, gap = 0;
            for (int j = i + 1; j < n; j++) {
                if (std::fabs(x[j] - ped) > 8) { b = j; gap = 0; }
                else if (++gap > gapMin) break;
            }
            if (b - a > us) pk.push_back({a, b});
            i = b + 1;
        }
        std::vector<VitsBurst> bu;
        for (const Seg& p : pk) {
            // trim the edges, then count crossings of the pedestal and take the amplitude from a fit at that frequency
            const int a = p.a + (p.b - p.a) / 8, b = p.b - (p.b - p.a) / 8;
            int zc = 0, f0 = -1, f1 = -1;
            for (int k = a; k < b; k++)
                if ((x[k] < ped) != (x[k + 1] < ped)) { zc++; if (f0 < 0) f0 = k; f1 = k; }
            if (zc < 3) continue;
            const double f = refineFreq(x, a, b, (zc - 1) / 2.0 / ((f1 - f0) / fs), fs);
            bu.push_back({f, std::abs(fitSine(x, a, b, f, fs)), 0});
        }
        bool rising = bu.size() >= 3;
        for (size_t k = 1; k < bu.size(); k++) if (bu[k].freqHz <= bu[k - 1].freqHz) rising = false;
        if (rising) {
            for (auto& b : bu) b.relDb = 20 * std::log10(b.amp / bu[0].amp);
            r.multiburst = true; r.bursts = bu;
        }
    }
    return r;
}

std::string VitsResult::text() const {
    std::string s;
    char b[160];
    if (bar) { snprintf(b, sizeof b, "bar %.1f IRE, tilt %.1f %%; ", barIre, barTiltPct); s += b; }
    if (pulse2T) { snprintf(b, sizeof b, "2T pulse HAD %.0f ns, K-PB %.1f %%; ", hadNs, kpbPct); s += b; }
    if (staircase) { snprintf(b, sizeof b, "staircase %d steps, DG %.1f %%, DP %.1f deg; ", steps, dgPct, dpDeg); s += b; }
    if (multiburst) {
        s += "multiburst";
        for (auto& x : bursts) { snprintf(b, sizeof b, " %.1f MHz %+.1f dB", x.freqHz / 1e6, x.relDb); s += b; }
        s += "; ";
    }
    if (s.size() >= 2) s.resize(s.size() - 2);
    return s;
}

} // namespace smart5
} // namespace dect2
