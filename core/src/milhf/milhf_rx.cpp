// Military / government HF waveform identifier (see milhf_rx.h). Clean-room: written from the published physical-layer parameters only.
#include "dect2/milhf/milhf_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace dect2::milhf {

namespace {
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

void fft(std::vector<cd>& a, bool inv) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / (double)len * (inv ? 1 : -1);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inv) for (auto& v : a) v /= (double)n;
}

// Welch power spectrum, Hann window, 50 % overlap (nfft/2+1 bins, linear power)
std::vector<double> welch(const float* x, size_t n, size_t nfft) {
    std::vector<double> p(nfft / 2 + 1, 0.0), w(nfft);
    for (size_t i = 0; i < nfft; i++) w[i] = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)nfft);
    int cnt = 0;
    std::vector<cd> b(nfft);
    for (size_t s = 0; s + nfft <= n; s += nfft / 2) {
        for (size_t i = 0; i < nfft; i++) b[i] = cd(x[s + i] * w[i], 0);
        fft(b, false);
        for (size_t k = 0; k <= nfft / 2; k++) p[k] += std::norm(b[k]);
        cnt++;
    }
    if (cnt) for (auto& v : p) v /= cnt;
    return p;
}

std::vector<double> boxSmooth(const std::vector<double>& v, int half) {
    std::vector<double> o(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        const size_t a = i >= (size_t)half ? i - half : 0, b = std::min(v.size() - 1, i + half);
        double s = 0;
        for (size_t k = a; k <= b; k++) s += v[k];
        o[i] = s / (double)(b - a + 1);
    }
    return o;
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

double closeTo(double x, double target, double tol) { return std::max(0.0, 1.0 - std::fabs(x - target) / tol); }

size_t pow2Floor(size_t n) { size_t p = 1; while (p * 2 <= n) p *= 2; return p; }
size_t pow2Ceil(size_t n) { size_t p = 1; while (p < n) p *= 2; return p; }

// largest |P|^2 over its mean for the M-th power of the symbols (a carrier line means the points sit on M phases)
double powerLine(const std::vector<cd>& s, int M) {
    const size_t K = s.size(), Kp = pow2Ceil(K);
    std::vector<cd> p(Kp, cd(0, 0));
    for (size_t i = 0; i < K; i++) {
        cd v(1, 0);
        for (int m = 0; m < M; m++) v *= s[i];
        p[i] = v;
    }
    fft(p, false);
    double mx = 0, sum = 0;
    for (auto& v : p) { const double e = std::norm(v); mx = std::max(mx, e); sum += e; }
    return sum > 0 ? mx / (sum / (double)Kp) : 0;
}

}  // namespace

void psdDb(const float* x, size_t n, double fs, int nfft, std::vector<float>& out) {
    (void)fs;
    out.clear();
    if ((int)n < nfft || nfft < 16) return;
    const std::vector<double> p = welch(x, n, (size_t)nfft);
    out.resize(p.size());
    for (size_t i = 0; i < p.size(); i++) out[i] = (float)(10 * std::log10(p[i] + 1e-20));
}

Result analyze(const float* x, size_t n, double fs) {
    Result r;
    Features& f = r.f;
    if (fs < 8000 || fs > 24500) return r;
    size_t N = pow2Floor(std::min<size_t>(n, 65536));
    if (N < 8192) return r;
    std::vector<float> y(x + (n - N), x + n);
    { double m = 0; for (float v : y) m += v; m /= (double)N; for (auto& v : y) v -= (float)m; }

    // 1. occupied band from the smoothed averaged spectrum
    const std::vector<double> P1 = welch(y.data(), N, 2048);
    const double bin1 = fs / 2048.0;
    const int k0 = (int)(50.0 / bin1), k1 = (int)(std::min(fs * 0.45, 6000.0) / bin1);
    const std::vector<double> sm = boxSmooth(P1, 3);
    std::vector<double> inRange(sm.begin() + k0, sm.begin() + k1);
    std::vector<double> sorted = inRange;
    std::sort(sorted.begin(), sorted.end());
    const double floorP = std::max(sorted[sorted.size() / 5], 1e-18);
    const double thr = floorP * std::pow(10.0, 0.4);
    int bestA = -1, bestB = -1;
    double bestPow = 0;
    for (int k = k0; k < k1;) {
        if (sm[k] <= thr) { k++; continue; }
        int a = k, b = k, gap = 0;
        double pw = 0;
        for (int j = k; j < k1; j++) {
            if (sm[j] > thr) { b = j; gap = 0; pw += sm[j] - floorP; }
            else if (++gap > 4) break;
        }
        if (pw > bestPow) { bestPow = pw; bestA = a; bestB = b; }
        k = b + 1;
    }
    bool bandFound = bestA >= 0 && (bestB - bestA + 1) * bin1 >= 150.0;
    if (bandFound) {
        f.loHz = bestA * bin1; f.hiHz = (bestB + 1) * bin1;
        f.bwHz = f.hiHz - f.loHz; f.centreHz = 0.5 * (f.loHz + f.hiHz);
        double m = 0;
        for (int k = bestA; k <= bestB; k++) m += sm[k];
        m /= (bestB - bestA + 1);
        f.snrDb = 10 * std::log10(std::max(m / floorP - 1.0, 1e-3));
    }

    // 2. narrow steady lines: a carrier, the Link 11 Doppler and sync tones
    const std::vector<double> P3 = welch(y.data(), N, 16384);
    const double bin3 = fs / 16384.0;
    {
        const int a = (int)(40.0 / bin3), b = (int)(std::min(fs * 0.45, 6000.0) / bin3);
        const int W = 80;
        for (int k = std::max(a, W); k < b && k + W < (int)P3.size(); k++) {
            bool isMax = true;
            for (int j = -3; j <= 3 && isMax; j++) if (j && P3[k + j] > P3[k]) isMax = false;
            if (!isMax) continue;
            std::vector<double> win(P3.begin() + (k - W), P3.begin() + (k + W + 1));
            const double med = std::max(medianOf(win), 1e-20);
            const double db = 10 * std::log10(P3[k] / med);
            const double hz = k * bin3;
            if (db >= 15.0 && db > f.carrierDb) { f.carrierDb = db; f.carrierHz = hz; }
            if (db >= 12.0 && std::fabs(hz - 605.0) <= 6.0 && db > f.pilot605Db) { f.pilot605 = true; f.pilot605Db = db; }
            if (db >= 12.0 && std::fabs(hz - 2915.0) <= 6.0) f.sync2915 = true;
        }
    }
    f.signal = bandFound || f.carrierDb > 0;
    if (!f.signal) return r;

    // 3. tone comb: periodicity of the spectrum inside the band
    if (bandFound && f.bwHz >= 300.0) {
        const std::vector<double> P2 = boxSmooth(welch(y.data(), N, 4096), 1);
        const double bin2 = fs / 4096.0;
        const int a = (int)(f.loHz / bin2), b = (int)(f.hiHz / bin2);
        const int len = b - a + 1;
        double mean = 0;
        for (int k = a; k <= b; k++) mean += P2[k];
        mean /= len;
        std::vector<double> v(len);
        double var = 0;
        for (int i = 0; i < len; i++) { v[i] = P2[a + i] - mean; var += v[i] * v[i]; }
        const int maxLag = std::min(140, len / 3);
        std::vector<double> ac(maxLag + 2, 0.0);
        for (int l = 1; l <= maxLag + 1 && l < len; l++) {
            double s = 0;
            for (int i = 0; i + l < len; i++) s += v[i] * v[i + l];
            ac[l] = var > 0 ? s / var : 0;
        }
        int bl = 0;
        double bv = 0;
        for (int l = 6; l <= maxLag; l++)
            if (ac[l] > ac[l - 1] && ac[l] >= ac[l + 1] && ac[l] > bv) { bv = ac[l]; bl = l; }
        if (bl > 0) {
            const double d = ac[bl - 1] - 2 * ac[bl] + ac[bl + 1];
            const double off = d < 0 ? 0.5 * (ac[bl - 1] - ac[bl + 1]) / d : 0;
            f.toneSpacingHz = (bl + off) * bin2;
            f.combScore = bv;
            // count the peaks that stand clear of the band average
            const int w = std::max(3, (int)(0.4 * bl));
            int cnt = 0;
            for (int k = a + w; k <= b - w; k++) {
                bool isMax = true;
                for (int j = -w; j <= w && isMax; j++) if (j && P2[k + j] > P2[k]) isMax = false;
                if (isMax && P2[k] > mean * 1.1) cnt++;
            }
            f.nTones = cnt;
        }
        // two-peak (FSK) signals: the two strongest separated peaks
        if (f.bwHz < 700.0) {
            int p1 = a;
            for (int k = a; k <= b; k++) if (P2[k] > P2[p1]) p1 = k;
            int p2 = -1;
            for (int k = a; k <= b; k++)
                if (std::abs(k - p1) * bin2 >= 60.0 && (p2 < 0 || P2[k] > P2[p2])) p2 = k;
            if (p2 >= 0 && P2[p2] > 0.35 * P2[p1]) f.shiftHz = std::abs(p2 - p1) * bin2;
        }
    }

    // 4. analytic signal in the band: envelope clock, slow envelope modulation, bursts
    std::vector<cd> z;
    std::vector<double> e;
    double fb = 0;
    if (bandFound) {
        std::vector<cd> X(N);
        for (size_t i = 0; i < N; i++) X[i] = cd(y[i], 0);
        fft(X, false);
        const size_t ka = (size_t)std::max(0.0, (f.loHz - 40.0) / fs * (double)N), kb = (size_t)std::min(fs / 2 * 0.98, f.hiHz + 40.0) / fs * N;
        for (size_t k = 0; k < N; k++) X[k] = (k >= ka && k <= kb) ? 2.0 * X[k] : cd(0, 0);
        fft(X, true);
        z.swap(X);
        e.resize(N);
        double m = 0;
        for (size_t i = 0; i < N; i++) { e[i] = std::norm(z[i]); m += e[i]; }
        m /= (double)N;
        const std::vector<double> e0 = e;
        for (auto& v : e) v -= m;
        std::vector<cd> E(N);
        for (size_t i = 0; i < N; i++) E[i] = cd(e[i], 0);
        fft(E, false);
        std::vector<double> pe(N / 2);
        for (size_t k = 0; k < N / 2; k++) pe[k] = std::norm(E[k]);
        const double binN = fs / (double)N;
        // clock line
        const int ca = (int)(40.0 / binN), cb = (int)(std::min(3500.0, fs * 0.4) / binN);
        int pk = ca;
        for (int k = ca; k < cb; k++) if (pe[k] > pe[pk]) pk = k;
        std::vector<double> win;
        for (int k = std::max(0, pk - 400); k < std::min((int)pe.size(), pk + 400); k++) if (std::abs(k - pk) > 8) win.push_back(pe[k]);
        double pkv = 0;
        for (int k = pk - 1; k <= pk + 1; k++) pkv += pe[k];
        pkv /= 3.0;
        const double med = std::max(medianOf(win), 1e-30);
        f.baudDb = 10 * std::log10(pkv / med);
        if (f.baudDb >= 10.0) {
            const double d = pe[pk - 1] - 2 * pe[pk] + pe[pk + 1];
            const double off = d < 0 ? 0.5 * (pe[pk - 1] - pe[pk + 1]) / d : 0;
            f.baudHz = (pk + off) * binN;
            fb = f.baudHz;
        }
        // slow envelope modulation
        {
            const int sa = std::max(2, (int)(0.5 / binN)), sb = (int)(40.0 / binN);
            int sp = sa;
            for (int k = sa; k < sb; k++) if (pe[k] > pe[sp]) sp = k;
            std::vector<double> ref(pe.begin() + sa, pe.begin() + (int)(200.0 / binN));
            const double rm = std::max(medianOf(ref), 1e-30);
            const double db = 10 * std::log10(pe[sp] / rm);
            if (db >= 12.0) { f.envModDb = db; f.envModHz = sp * binN; }
        }
        // bursts: 20 ms energy windows
        {
            const size_t w = std::max<size_t>(1, (size_t)(0.02 * fs));
            std::vector<double> en;
            for (size_t i = 0; i + w <= N; i += w) { double s = 0; for (size_t j = 0; j < w; j++) s += e0[i + j]; en.push_back(s); }
            std::vector<double> t = en;
            std::sort(t.begin(), t.end());
            const double p10 = t[t.size() / 10], p90 = t[t.size() * 9 / 10];
            if (p90 > 2.0 * std::max(p10, 1e-30)) {
                const double mid = 0.5 * (p10 + p90);
                int on = 0; bool was = false;
                for (double v : en) { const bool s = v > mid; on += s; if (s && !was) f.bursts++; was = s; }
                f.duty = (double)on / (double)en.size();
            }
        }
    }

    // 5. single carrier: symbols at the clock, constellation order, repetition period
    const bool combLike = f.combScore >= 0.30 && f.nTones >= 3;
    if (fb >= 100.0 && fb >= f.bwHz / 2.0 && fb <= f.bwHz * 1.1 && !z.empty()) {
        const double T = fs / fb;
        double ph = 0;
        {
            cd acc(0, 0);
            for (size_t i = 0; i < N; i++) acc += e[i] * std::polar(1.0, -2 * kPi * fb * (double)i / fs);
            ph = -std::arg(acc) / (2 * kPi);   // symbol phase in fractions of a period
        }
        std::vector<cd> w(N);
        for (size_t i = 0; i < N; i++) w[i] = z[i] * std::polar(1.0, -2 * kPi * f.centreHz * (double)i / fs);
        const size_t K = std::min<size_t>((size_t)((double)(N - 4 * T) / T), 32768);
        std::vector<cd> best;
        double bestScore = -1;
        int bestOrder = 99;
        double bestLine = 0;
        for (int q = 0; q < 4; q++) {
            std::vector<cd> s(K);
            for (size_t k = 0; k < K; k++) {
                const double t = ((double)k + ph + 0.25 * q) * T + 2 * T;
                const size_t i0 = (size_t)t;
                const double fr = t - (double)i0;
                cd v = w[i0] * (1 - fr) + w[i0 + 1] * fr;
                const double a = std::abs(v);
                s[k] = a > 1e-12 ? v / a : cd(0, 0);
            }
            double l2 = powerLine(s, 2), l4 = powerLine(s, 4), l8 = powerLine(s, 8);
            const double lmax = std::max({l2, l4, l8});
            const double thrL = std::max(40.0, 0.4 * lmax);
            int ord = 99;
            double line = lmax;
            if (l2 >= thrL) ord = 2;
            else if (l4 >= thrL) ord = 4;
            else if (l8 >= thrL) ord = 8;
            if (line > bestScore) { bestScore = line; best = std::move(s); bestOrder = ord; bestLine = line; }
        }
        f.order = bestOrder;
        f.orderScore = bestLine;
        // repetition: autocorrelation of the symbol stream
        const size_t Kp = pow2Ceil(2 * K);
        std::vector<cd> S(Kp, cd(0, 0));
        double r0 = 0;
        for (size_t i = 0; i < K; i++) { S[i] = best[i]; r0 += std::norm(best[i]); }
        fft(S, false);
        for (auto& v : S) v = cd(std::norm(v), 0);
        fft(S, true);
        const size_t Lmax = std::min<size_t>(2048, K / 4);
        std::vector<double> mag;
        for (size_t L = 8; L <= Lmax; L++) mag.push_back(std::abs(S[L]));
        const double mmed = std::max(medianOf(mag), 1e-30);
        double mx = 0;
        for (double v : mag) mx = std::max(mx, v);
        if (r0 > 0 && mx / r0 >= 0.08 && mx >= 5.0 * mmed) {
            for (size_t i = 0; i < mag.size(); i++)
                if (mag[i] >= 0.7 * mx) {
                    f.frameLagSym = (int)(i + 8);
                    f.frameCorr = mag[i] / r0;
                    f.frameLagMs = 1000.0 * f.frameLagSym / fb;
                    break;
                }
        }
    }

    // 6. candidates
    auto add = [&](const char* name, double conf, const std::string& basis, const char* note, const char* url) {
        if (conf < 0.15) return;
        Candidate c;
        c.name = name; c.conf = (float)std::min(conf, 0.95); c.basis = basis; c.note = note; c.url = url;
        r.cands.push_back(c);
    };
    char b[200];
    const char* kSigid4285 = "https://www.sigidwiki.com/wiki/STANAG_4285";
    const bool single = f.baudHz > 0 && f.baudHz >= f.bwHz / 2.0 && f.baudHz <= f.bwHz * 1.1 && f.bwHz >= 1000.0;
    if (single) {
        const double base = 0.40 * closeTo(f.baudHz, 2400.0, 80.0) + 0.20 * closeTo(f.centreHz, 1800.0, 200.0) +
                            0.15 * (f.bwHz >= 2000.0 && f.bwHz <= 3300.0 ? 1.0 : 0.0) + 0.15 * ((f.order == 2 || f.order == 4 || f.order == 8) ? 1.0 : 0.0) +
                            0.10 * (f.baudDb >= 15.0 ? 1.0 : 0.0);
        std::snprintf(b, sizeof b, "%.0f Bd, band %.0f - %.0f Hz (centre %.0f), order %s, repeat %s", f.baudHz, f.loHz, f.hiHz, f.centreHz,
                      f.order == 99 ? "high/unclear" : std::to_string(f.order).c_str(), f.frameLagSym ? (std::to_string(f.frameLagSym) + " sym").c_str() : "none");
        const std::string basis = b;
        if (closeTo(f.baudHz, 2400.0, 80.0) > 0) {
            const bool l256 = std::abs(f.frameLagSym - 256) <= 1, l48 = f.frameLagSym == 48 || f.frameLagSym == 96;
            add("2400 Bd serial-tone PSK family", base * 0.9, basis, "Single-carrier 2400 symbols/s on a 1800 Hz sub-carrier: STANAG 4285, MIL-STD-188-110A/B, STANAG 4539 and others share this physical layer; the repeat period and constellation separate them.", kSigid4285);
            add("STANAG 4285", base * (l256 ? 1.0 : 0.45), basis, "HF modem, 2400 Bd, BPSK/QPSK/8-PSK, 256-symbol frames (80 known + data) at 106.7 ms. STANAG 5066 data-link users often ride on it (identification of the PHY only).", kSigid4285);
            add("MIL-STD-188-110A serial tone", base * (l48 ? 1.0 : 0.40), basis, "US HF serial-tone modem, 2400 Bd 8-PSK, known probe symbols between data blocks. Also used under STANAG 5066 (PHY only).", "https://en.wikipedia.org/wiki/MIL-STD-188");
            add("MIL-STD-188-110B / STANAG 4539 high rate", base * (f.order == 99 ? 0.80 : 0.22), basis, "Same 2400 Bd tone with higher-order QAM for up to 12.8 kbit/s: a constellation that is not 2/4/8-PSK points here.", "https://en.wikipedia.org/wiki/MIL-STD-188");
            add("STANAG 4481 burst (unverified match)", base * (f.duty < 0.7 ? 0.35 : 0.12), basis, "NATO HF burst waveform; only the single-tone burst structure is checked here.", kSigid4285);
            add("3G-ALE burst (8-PSK, 2400 Bd)", base * (f.bursts >= 1 && f.duty < 0.85 ? 0.70 : 0.10), basis, "Third-generation ALE uses the same 2400 Bd 8-PSK in short bursts; see the HF ALE mode for 2G-ALE.", "https://en.wikipedia.org/wiki/Automatic_link_establishment");
        }
    }
    if (f.pilot605 || (combLike && std::fabs(f.toneSpacingHz - 110.0) < 8.0)) {
        const double c = 0.35 * (f.pilot605 ? 1.0 : 0.0) + 0.30 * closeTo(f.toneSpacingHz, 110.0, 12.0) + 0.15 * (f.nTones >= 12 && f.nTones <= 22 ? 1.0 : 0.0) +
                         0.10 * (combLike ? 1.0 : 0.0) + 0.10 * (f.sync2915 ? 1.0 : 0.0);
        std::snprintf(b, sizeof b, "605 Hz tone %s (%.0f dB), %d peaks, spacing %.0f Hz%s", f.pilot605 ? "yes" : "no", f.pilot605Db, f.nTones, f.toneSpacingHz, f.sync2915 ? ", 2915 Hz tone" : "");
        add("Link 11 CLEW (parallel tones)", c, b, "HF Link 11 conventional waveform: a comb of DPSK tones with a Doppler tone at 605 Hz and a sync tone at 2915 Hz; the single-tone SLEW variant is a different, serial waveform. Tone count and baud are estimates.", "https://en.wikipedia.org/wiki/Link_11");
    }
    if (combLike || f.shiftHz > 0) {
        if (combLike && std::fabs(f.toneSpacingHz - 250.0) < 35.0) {
            const double c = 0.40 * closeTo(f.toneSpacingHz, 250.0, 35.0) + 0.25 * (f.nTones >= 6 && f.nTones <= 9 ? 1.0 : 0.0) + 0.15 * (f.bwHz >= 1500 && f.bwHz <= 2400 ? 1.0 : 0.0) + 0.20 * (f.bursts >= 1 ? 1.0 : 0.0);
            std::snprintf(b, sizeof b, "%d tones, spacing %.0f Hz, %d bursts, band %.0f Hz", f.nTones, f.toneSpacingHz, f.bursts, f.bwHz);
            add("2G-ALE (8-FSK, 125 Bd)", c, b, "Automatic link establishment calls and soundings: 8 tones, 250 Hz apart. The decoder is the HF ALE mode.", "https://en.wikipedia.org/wiki/Automatic_link_establishment");
        }
        if (combLike && f.nTones >= 28 && std::fabs(f.toneSpacingHz - 40.0) < 8.0) {
            std::snprintf(b, sizeof b, "%d tones, spacing %.0f Hz, band %.0f Hz", f.nTones, f.toneSpacingHz, f.bwHz);
            add("CIS-36-50 (36-tone MFSK)", 0.35 + 0.35 * closeTo(f.toneSpacingHz, 40.0, 8.0) + 0.2 * closeTo(f.nTones, 36, 10), b, "Russian-origin 36-tone, 50 Bd MFSK; identified here by tone count and spacing only (unverified figures).", "https://www.sigidwiki.com/wiki/CIS-36-50");
        } else if (combLike && f.nTones >= 8 && f.nTones <= 16 && std::fabs(f.toneSpacingHz - 110.0) >= 8.0 && std::fabs(f.toneSpacingHz - 250.0) >= 35.0) {
            std::snprintf(b, sizeof b, "%d tones, spacing %.0f Hz, band %.0f Hz", f.nTones, f.toneSpacingHz, f.bwHz);
            add("Multi-tone MFSK (CIS-12 / Chinese-style, unverified)", 0.20 + 0.45 * std::min(1.0, f.combScore), b, "A regular set of 8 to 16 tones. Several state MFSK systems look like this; tone spacing and rate are the only evidence.", "https://www.sigidwiki.com/wiki/Category:Military_Signals");
        }
        if (f.shiftHz > 0 && f.bwHz < 700.0) {
            std::snprintf(b, sizeof b, "two peaks %.0f Hz apart, band %.0f Hz", f.shiftHz, f.bwHz);
            add("PACTOR-I / 200 Bd FSK (200 Hz shift)", 0.55 * closeTo(f.shiftHz, 200.0, 40.0) + 0.1, b, "Two-tone FSK with a 200 Hz shift (PACTOR-I is 100 or 200 Bd); identification by shift only. Open the PACTOR-I mode (HF UTILITY, experimental) to try to read it; PACTOR-II, -III and -IV are proprietary and not supported.", "https://en.wikipedia.org/wiki/PACTOR");
            if (f.baudHz > 0 && std::fabs(f.baudHz - 100.0) < 15.0)
                add("PACTOR-II-like (100 Bd DPSK, narrow)", 0.30 + 0.2 * closeTo(f.baudHz, 100.0, 15.0), b, "A 100 baud clock in a band of a few hundred Hz; PACTOR-II uses two DPSK carriers 100 Hz apart (unverified match).", "https://en.wikipedia.org/wiki/PACTOR");
        }
    }
    if (f.carrierDb >= 15.0 && !single && !combLike) {
        std::snprintf(b, sizeof b, "carrier %.0f Hz (%.0f dB over its surroundings), envelope modulation %s", f.carrierHz, f.carrierDb,
                      f.envModDb > 0 ? (std::to_string((int)std::lround(f.envModHz)) + " Hz").c_str() : "none");
        if (f.envModDb >= 12.0 && f.envModHz >= 1.0)
            add("Buzzer (UVB-76 style: carrier with a periodic buzz)", 0.30 + 0.30 * std::min(1.0, (f.carrierDb - 15.0) / 20.0) + 0.15 * std::min(1.0, (f.envModDb - 12.0) / 15.0), b,
                "AM/USB voice-channel carrier with a steady repetitive buzz or tone; only the carrier and the buzz rate are measured, nothing is decoded.", "https://www.sigidwiki.com/wiki/UVB-76");
        add("AM voice / numbers-station style carrier", 0.30 + 0.2 * std::min(1.0, (f.carrierDb - 15.0) / 20.0), b, "A steady carrier with voice-band energy and no digital clock. Tones only; voice content is not decoded.", "https://www.sigidwiki.com/wiki/Category:Military_Signals");
    }
    if (r.cands.empty() && f.signal) {
        std::snprintf(b, sizeof b, "band %.0f - %.0f Hz (%.0f Hz wide), SNR %.0f dB", f.loHz, f.hiHz, f.bwHz, f.snrDb);
        Candidate c;
        c.name = "Unclassified signal"; c.conf = 0.10f; c.basis = b; c.note = "Something is there, but none of the known physical-layer patterns match.";
        r.cands.push_back(c);
    }
    std::stable_sort(r.cands.begin(), r.cands.end(), [](const Candidate& a, const Candidate& c) { return a.conf > c.conf; });
    if (r.cands.size() > 6) r.cands.resize(6);
    return r;
}

}  // namespace dect2::milhf
