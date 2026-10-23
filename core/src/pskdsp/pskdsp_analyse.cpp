// Blind analysis of a PSK signal: carrier, symbol rate, modulation order, pulse shape and quality, from a block of samples.
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;

size_t pow2Floor(size_t n, size_t cap, size_t floorv) {
    size_t f = 1;
    while (f * 2 <= n && f * 2 <= cap) f *= 2;
    return std::max(f, floorv);
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
    return v[v.size() / 2];
}

struct Line { double pos = 0; double db = 0; double val = 0; bool ok = false; };   // pos: fractional bin index

// the strongest line in p[lo, hi) against the median of that range
Line strongest(const std::vector<double>& p, size_t lo, size_t hi, size_t slo = 0, size_t shi = 0) {
    Line l;
    if (hi <= lo + 4) return l;
    std::vector<double> seg(p.begin() + (long)lo, p.begin() + (long)hi);
    const double med = std::max(1e-30, medianOf(seg));
    if (shi == 0) { slo = lo; shi = hi; }
    size_t bi = slo;
    for (size_t i = slo; i < shi; i++) if (p[i] > p[bi]) bi = i;
    l.val = p[bi];
    l.db = 10 * std::log10(std::max(1e-30, p[bi]) / med);
    double pos = (double)bi;
    if (bi > 0 && bi + 1 < p.size()) {
        const double a = std::log(std::max(1e-30, p[bi - 1])), b = std::log(std::max(1e-30, p[bi])), c = std::log(std::max(1e-30, p[bi + 1]));
        const double den = a - 2 * b + c;
        if (den < -1e-9) pos += std::clamp(0.5 * (a - c) / den, -1.0, 1.0);
    }
    l.pos = pos; l.ok = true;
    return l;
}

// spectrum of |x|^2 (mean removed): positive half, bin k at k * fs / nfft
std::vector<double> envelopeSpectrum(const std::vector<cf>& y, size_t nfft) {
    std::vector<cf> e(y.size());
    double m = 0;
    for (const cf& v : y) m += std::norm(v);
    m /= (double)std::max<size_t>(1, y.size());
    for (size_t i = 0; i < y.size(); i++) e[i] = cf((float)(std::norm(y[i]) - m), 0.f);
    std::vector<double> p = signallab::psd(e.data(), e.size(), nfft);
    return std::vector<double>(p.begin() + (long)nfft / 2, p.end());
}

// spectrum of Re(y e^{j theta})^2 (mean removed): the rail's squared level has a line at the symbol rate even for OQPSK, whose envelope has none
std::vector<double> railSpectrum(const std::vector<cf>& y, double theta, size_t nfft) {
    std::vector<cf> e(y.size());
    const cf r((float)std::cos(theta), (float)std::sin(theta));
    double m = 0;
    for (size_t i = 0; i < y.size(); i++) { const float v = (y[i] * r).real(); e[i] = cf(v * v, 0.f); m += v * v; }
    m /= (double)std::max<size_t>(1, y.size());
    for (auto& v : e) v = cf((float)(v.real() - m), 0.f);
    std::vector<double> p = signallab::psd(e.data(), e.size(), nfft);
    return std::vector<double>(p.begin() + (long)nfft / 2, p.end());
}

// spectrum of y^M, full (DC in the middle)
std::vector<double> powerSpectrum(const std::vector<cf>& y, int M, size_t nfft) {
    std::vector<cf> z(y.size());
    for (size_t i = 0; i < y.size(); i++) {
        cf v = y[i];
        const float a = std::abs(v);
        if (a > 0) v /= a;   // constant envelope: the line is the phase's, not the amplitude's
        cf r = v;
        for (int k = 1; k < M; k++) r *= v;
        z[i] = r;
    }
    return signallab::psd(z.data(), z.size(), nfft);
}

struct Trial {
    Order order = Order::Bpsk;
    double evm = 9, baud = 0, lockMetric = 0;
    int used = 0;
    std::vector<cf> sym;
    double freqEndHz = 0, dopplerHzPerSec = 0, spsMean = 0;
    double expEvm = 0.5;   // from the M2M4 moment estimate of Es/N0 (needs no decisions): the EVM a correct order shows
};

Trial runTrial(const std::vector<cf>& y, double fs, double baud, Order o, double alpha, size_t maxSym, bool wantTrack = false, double tbn = 0.004) {
    Trial t;
    t.order = o;
    t.baud = baud;
    DemodConfig dc;
    dc.order = o; dc.sps = fs / baud; dc.alpha = alpha; dc.carrierBn = 0.02; dc.fllGain = 0.03; dc.timingBn = tbn;
    Demod d;
    d.configure(dc);
    const size_t maxIn = (size_t)((double)maxSym * dc.sps);
    const size_t n = std::min(y.size(), maxIn);
    const size_t chunk = 512;
    std::vector<double> ts, fr, sp;
    for (size_t i = 0; i < n; i += chunk) {
        d.process(y.data() + i, std::min(chunk, n - i), t.sym);
        if (wantTrack && (i / chunk) % 2 == 0) {
            const DemodStats s = d.stats();
            ts.push_back((double)i / fs); fr.push_back(s.freq * fs / (2 * kPi)); sp.push_back(s.spsNow);
        }
    }
    const size_t N = t.sym.size();
    if (N < 100) return t;
    const size_t from = N * 2 / 5;
    double pw = 0;
    for (size_t i = from; i < N; i++) pw += std::norm(t.sym[i]);
    pw = std::max(1e-20, pw / (double)(N - from));
    const int M = orderPoints(o);
    std::vector<int> cnt((size_t)M, 0);
    double e = 0;
    for (size_t i = from; i < N; i++) {
        const cf z = t.sym[i] / (float)std::sqrt(pw);
        int idx = 0;
        const cf p = decide(z, o, &idx);
        e += std::norm(z - p);
        cnt[(size_t)idx]++;
    }
    t.evm = std::sqrt(e / (double)(N - from));
    {   // M2M4 estimator for a constant-modulus signal in complex Gaussian noise: S = sqrt(2 M2^2 - M4), N = M2 - S
        double m2 = 0, m4 = 0;
        for (size_t i = from; i < N; i++) { const double a = std::norm(t.sym[i]) / pw; m2 += a; m4 += a * a; }
        m2 /= (double)(N - from); m4 /= (double)(N - from);
        const double S = std::sqrt(std::max(0.0, 2 * m2 * m2 - m4)), Nn = std::max(m2 - S, 1e-3 * m2);
        t.expEvm = std::clamp(std::sqrt(Nn / std::max(S, 1e-6)), 0.03, 1.5);
    }
    for (int c : cnt) if ((double)c >= 0.4 / M * (double)(N - from)) t.used++;
    t.lockMetric = d.stats().lockMetric;
    t.freqEndHz = d.stats().freq * fs / (2 * kPi);
    if (wantTrack && ts.size() > 8) {
        const size_t a = ts.size() * 3 / 10;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, ss = 0; int m = 0;
        for (size_t i = a; i < ts.size(); i++) { sx += ts[i]; sy += fr[i]; sxx += ts[i] * ts[i]; sxy += ts[i] * fr[i]; ss += sp[i]; m++; }
        const double den = m * sxx - sx * sx;
        if (m > 3 && den > 0) t.dopplerHzPerSec = (m * sxy - sx * sy) / den;
        t.spsMean = ss / m;
    } else t.spsMean = d.stats().spsNow;
    return t;
}

// the lowest order that uses all its points and fits as well as the measured noise says a correct one must (M2M4 estimate); else the
// lowest-order fit close to the best one
int pickOrder(const std::vector<Trial>& tr) {
    double exp = 1e9;
    for (const auto& t : tr) exp = std::min(exp, t.expEvm);
    int pick = -1;
    for (size_t i = 0; i < tr.size(); i++) {
        if (tr[i].used != orderPoints(tr[i].order) || tr[i].evm > 1.4 * exp + 0.03) continue;
        // QPSK and OQPSK exclude each other (the other one's EVM is far worse); a higher order must be clearly better to replace a lower one
        if (pick < 0 || tr[i].evm < 0.7 * tr[(size_t)pick].evm) pick = (int)i;
    }
    if (pick >= 0) return pick;
    double best = 1e9;
    for (const auto& t : tr) if (t.used == orderPoints(t.order)) best = std::min(best, t.evm);
    for (size_t i = 0; i < tr.size(); i++)
        if (tr[i].used == orderPoints(tr[i].order) && tr[i].evm <= 1.3 * best) return (int)i;
    return -1;
}

const Order kOrders[4] = {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8};
} // namespace

double snapRate(double b, double tol) {
    static const std::vector<double> list = [] {
        std::vector<double> v;
        for (int k = 0; k <= 11; k++) { v.push_back(75.0 * (1 << k)); v.push_back(25.0 * (1 << k)); }
        for (int n = 1; n <= 20; n++) v.push_back(1200.0 * n);
        for (int n = 1; n <= 12; n++) v.push_back(9600.0 * n);
        const double base[] = {100, 500, 1000, 2000, 2500, 5000, 10000, 20000, 50000, 100000, 125000, 250000, 500000, 1e6, 2e6, 31.25, 62.5, 125, 250, 625, 1250, 4000, 8000, 16000, 32000, 64000, 40000, 80000, 24000, 48000, 96000, 192000};
        for (double d : base) v.push_back(d);
        return v;
    }();
    double bestD = 1e9, bestV = 0;
    for (double v : list) {
        const double d = std::fabs(b - v) / v;
        if (d < bestD) { bestD = d; bestV = v; }
    }
    return bestD <= tol ? bestV : 0;
}

double estimateRolloff(const cf* y, size_t n, double fs, double baud, bool* rect) {
    if (rect) *rect = false;
    if (n < 4096 || baud <= 0 || fs < 2.5 * baud) return 0.35;
    size_t nfft = 256;
    while (nfft * 2 <= 65536 && (double)nfft * baud / fs < 32.0) nfft *= 2;
    nfft = std::min(nfft, pow2Floor(n / 2, 65536, 256));
    const std::vector<double> p = signallab::psd(y, n, nfft);
    const double bin = fs / (double)nfft;
    auto at = [&](double f) { long i = (long)std::lround(f / bin) + (long)nfft / 2; return p[(size_t)std::clamp(i, 0L, (long)nfft - 1)]; };
    double plat = 0; int m = 0;
    for (double f = -0.2 * baud; f <= 0.2 * baud; f += bin) { plat += at(f); m++; }
    plat /= std::max(1, m);
    std::vector<double> sm(p);
    const double noise = std::max(1e-30, medianOf(std::vector<double>(p.begin(), p.begin() + (long)nfft / 8)));   // the far edge of the span
    const double lvl = std::max(plat * 0.0316, 3 * noise);
    // widest span above the level, smoothed over a few bins
    auto sm3 = [&](double f) { double a = 0; for (int d = -2; d <= 2; d++) a += at(f + d * bin); return a / 5; };
    double hi = 0, lo = 0;
    for (double f = 0; f < fs / 2; f += bin) if (sm3(f) >= lvl) hi = f;
    for (double f = 0; f > -fs / 2; f -= bin) if (sm3(f) >= lvl) lo = f;
    // rectangular pulses leave sinc^2 side lobes beyond the symbol rate
    double side = 0; int ms = 0;
    for (double f = 1.25 * baud; f <= 1.75 * baud && f < fs / 2; f += bin) { side += at(f) + at(-f); ms += 2; }
    side = ms ? side / ms : 0;
    if (rect && side > plat * 0.005 && side > 4 * noise) *rect = true;
    const double a = (hi - lo) / baud - 1.0;
    return std::clamp(a, 0.1, 1.0);
}

Analysis analyse(const cf* x0, size_t n0, double fs, const AnalyseOptions& o) {
    Analysis A;
    auto say = [&](const std::string& s) { A.log.push_back(s); };
    A.status = "searching for a signal";
    if (n0 < 4096 || fs <= 0) { A.status = "need more samples"; return A; }
    size_t n = n0;
    const cf* x = x0;
    if (o.maxSeconds > 0 && (double)n > o.maxSeconds * fs) { n = (size_t)(o.maxSeconds * fs); x = x0 + (n0 - n); }

    // ---- 1 the signal
    if (o.haveCarrier) {
        A.carrier.found = true;
        A.carrier.offsetHz = o.carrierHz;
        Carrier near = findCarrier(x, n, fs, true, o.carrierHz, std::max(o.bwHz, 1.0));
        A.carrier = near;
        if (o.bwHz > 0) { A.carrier.occHz = o.bwHz; A.carrier.found = true; }
        A.carrier.offsetHz = o.carrierHz;
        if (!A.carrier.found) A.carrier.found = true;
    } else A.carrier = findCarrier(x, n, fs);
    if (!A.carrier.found) { A.status = "no signal found"; return A; }
    A.signal = true;
    char b[200];
    snprintf(b, sizeof b, "signal at %+.1f Hz, bandwidth %.0f Hz (99 %%), %.1f dB over the noise", A.carrier.offsetHz, A.carrier.occHz, A.carrier.snrDb);
    say(b);

    // ---- 2 channel
    const double pass = std::max(std::max(A.carrier.bw10Hz * 1.4, A.carrier.bw3Hz * 1.6), 8.0);
    Channeliser ch;
    ch.configure(fs, A.carrier.offsetHz, pass, 0);
    std::vector<cf> y;
    y.reserve(n / 2);
    ch.process(x, n, y);
    double fsC = ch.outRate();
    // skip the filter's start-up
    const size_t skip = std::min<size_t>(y.size() / 8, 256);
    if (y.size() > skip) y.erase(y.begin(), y.begin() + (long)skip);
    if (y.size() < 2048) { A.status = "need more samples"; return A; }
    A.fsChan = fsC;

    // ---- 3 power-law lines: the residual carrier
    {
        const size_t nfft = pow2Floor(y.size() / 4, 32768, 512);
        double bestDb = -1; double fc = 0; bool got = false;
        double* dbs[3] = {&A.line2, &A.line4, &A.line8};
        const int Ms[3] = {2, 4, 8};
        for (int i = 0; i < 3; i++) {
            const std::vector<double> p = powerSpectrum(y, Ms[i], nfft);
            const double halfWin = 0.06 * pass * Ms[i] / fsC * (double)nfft;   // the residual carrier is small: lines at +-baud are the cyclic ones
            const size_t cB = nfft / 2, w = (size_t)std::max(3.0, halfWin);
            Line l = strongest(p, 0, nfft, cB - w, cB + w + 1);
            {   // a line stands out from the pedestal next to it (OQPSK's x^2 has a broad skirt but no line)
                const size_t pk = (size_t)std::lround(l.pos);
                std::vector<double> nb;
                for (long d = -48; d <= 48; d++) if (std::labs(d) >= 5 && (long)pk + d >= 0 && (long)pk + d < (long)nfft) nb.push_back(p[(size_t)((long)pk + d)]);
                const double loc = std::max(1e-30, medianOf(nb));
                l.db = std::min(l.db, 10 * std::log10(std::max(1e-30, l.val) / loc) + 3.0);
            }
            *dbs[i] = l.db;
            if (!got && l.db > 14) { got = true; bestDb = l.db; fc = (l.pos - (double)nfft / 2) * fsC / (double)nfft / Ms[i]; }
        }
        if (got && std::fabs(fc) < 0.1 * pass) {
            const double w = -2 * kPi * fc / fsC;
            for (size_t i = 0; i < y.size(); i++) y[i] *= cf((float)std::cos(w * (double)i), (float)std::sin(w * (double)i));
            A.carrierHz = A.carrier.offsetHz + fc;
        } else A.carrierHz = A.carrier.offsetHz;
    }

    // ---- 4 symbol rate candidates
    std::vector<RateCandidate> cand;
    auto addCand = [&](double baud, double conf, const char* how) {
        if (baud < 5 || fsC / baud < 2.4 || fsC / baud > 200) return;
        for (auto& c : cand) if (std::fabs(c.baud - baud) < 0.007 * baud) { c.conf = std::max(c.conf, conf); return; }
        RateCandidate c; c.baud = baud; c.conf = conf; c.how = how;
        cand.push_back(c);
    };
    if (o.forceBaud > 0) addCand(o.forceBaud, 1.0, "set by the user");
    else {
        const size_t nfft = pow2Floor(y.size() / 4, 65536, 1024);
        const double binHz = fsC / (double)nfft;
        struct Ln { double f, db; const char* how; };
        std::vector<Ln> lines;
        auto collect = [&](const std::vector<double>& e, const char* how, bool setEnv) {
            std::vector<double> sm(e.size());
            for (size_t i = 0; i < e.size(); i++) { double a = 0; int m = 0; for (int d = -1; d <= 1; d++) { long j = (long)i + d; if (j >= 0 && j < (long)e.size()) { a += e[(size_t)j]; m++; } } sm[i] = a / m; }
            const double med = std::max(1e-30, medianOf(std::vector<double>(sm.begin() + 4, sm.end())));
            std::vector<std::pair<double, size_t>> peaks;
            for (size_t i = 8; i + 1 < sm.size(); i++)
                if (sm[i] > sm[i - 1] && sm[i] >= sm[i + 1] && sm[i] > 8 * med) peaks.push_back({sm[i], i});
            std::sort(peaks.rbegin(), peaks.rend());
            if (peaks.size() > 3) peaks.resize(3);
            for (auto& pk : peaks) {
                const Line l = strongest(e, pk.second - 2, pk.second + 3);
                const double db = 10 * std::log10(pk.first / med);
                lines.push_back({l.pos * binHz, db, how});
                if (setEnv) A.lineEnv = std::max(A.lineEnv, db);
            }
        };
        collect(envelopeSpectrum(y, nfft), "envelope line", true);
        collect(railSpectrum(y, 0, nfft), "rail line", false);
        collect(railSpectrum(y, kPi / 4, nfft), "rail line", false);
        std::stable_sort(lines.begin(), lines.end(), [](const Ln& a, const Ln& b) { return a.db > b.db; });
        for (const Ln& l : lines) {
            const double conf = std::clamp(l.db / 25.0, 0.1, 1.0);
            addCand(l.f, conf, l.how);
            addCand(l.f / 2, conf * 0.6, "half a line");
        }
        // the -3 dB width of an RRC spectrum is the symbol rate whatever the roll-off; a rectangular pulse is 12 % narrower
        const double w = A.carrier.bw3Hz;
        for (int gi = -24; gi <= 24; gi++) {
            const double gf = 1.0 + 0.005 * gi;
            { addCand(w * gf, 0.3, "bandwidth"); addCand(w * 0.88 * gf, 0.2, "bandwidth"); }
        }
    }
    if (cand.empty()) { A.status = "signal found, symbol rate unclear"; return A; }
    std::stable_sort(cand.begin(), cand.end(), [](const RateCandidate& a, const RateCandidate& b) { return a.conf > b.conf; });
    if (cand.size() > 220) cand.resize(220);

    // ---- 5 trial demodulation per candidate and order
    const bool forceO = o.forceOrder != Order::Auto;
    const double alphaS = o.forceAlpha >= 0 ? std::max(0.05, o.forceAlpha) : 0.35;
    if (cand.size() > 12) {   // screening: how constant the symbol level is after a QPSK / OQPSK timing loop (it does not depend on the order)
        std::vector<std::pair<double, size_t>> sc;
        for (size_t c = 0; c < cand.size(); c++) {
            double ee = 9;
            for (Order od : {Order::Qpsk, Order::Oqpsk}) ee = std::min(ee, runTrial(y, fsC, cand[c].baud, od, o.forceAlpha == -1 ? 0 : alphaS, 1500).expEvm);
            sc.push_back({ee - cand[c].conf * 0.02, c});   // equal scores: the lines before the bandwidth guesses
        }
        std::sort(sc.begin(), sc.end());
        std::vector<RateCandidate> keep;
        for (size_t k = 0; k < std::min<size_t>(10, sc.size()); k++) keep.push_back(cand[sc[k].second]);
        cand.swap(keep);
    }
    double bestScore = 1e9; double winBaud = 0; Order bestO = Order::Bpsk;
    std::vector<std::pair<Order, double>> bestOrderEvm;
    std::vector<double> scores(cand.size(), 9.0), refBaud(cand.size(), 0.0);
    std::vector<Order> candOrder(cand.size(), Order::Bpsk);
    const double alpha0 = o.forceAlpha >= 0 ? std::max(0.05, o.forceAlpha) : 0.35;
    const bool rect0 = o.forceAlpha == -1;
    for (size_t c = 0; c < cand.size(); c++) {
        std::vector<Trial> tr;
        for (Order od : kOrders) {
            if (forceO && od != o.forceOrder) continue;
            tr.push_back(runTrial(y, fsC, cand[c].baud, od, rect0 ? 0 : alpha0, 2500));
        }
        const int pi = forceO ? 0 : pickOrder(tr);
        if (pi < 0) continue;
        double ee = 9;
        for (const auto& t : tr) ee = std::min(ee, t.expEvm);
        scores[c] = ee;   // the symbol rate is ranked by how constant the symbol level is (M2M4), which does not depend on the order
        candOrder[c] = tr[(size_t)pi].order;
        refBaud[c] = tr[(size_t)pi].spsMean > 0 ? fsC / tr[(size_t)pi].spsMean : cand[c].baud;
    }
    // the timing loop pulls in slowly from a rate that is off by a few tenths of a percent: run the best ones again at the rate the loop reached
    for (int round = 0; round < 2; round++) {
        std::vector<size_t> idx(cand.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return scores[a] < scores[b]; });
        for (size_t k = 0; k < std::min<size_t>(6, idx.size()); k++) {
            const size_t c = idx[k];
            if (scores[c] > 0.9 || refBaud[c] <= 0) continue;
            const Trial t = runTrial(y, fsC, refBaud[c], candOrder[c], rect0 ? 0 : alpha0, 2500);
            if (t.expEvm < scores[c] * 0.98) {
                scores[c] = t.expEvm; cand[c].baud = refBaud[c];
                refBaud[c] = t.spsMean > 0 ? fsC / t.spsMean : refBaud[c];
            } else if (round == 0) refBaud[c] = (refBaud[c] + (t.spsMean > 0 ? fsC / t.spsMean : refBaud[c])) / 2;
        }
    }
    {
        size_t cb = 0;
        for (size_t c = 1; c < cand.size(); c++) if (scores[c] < scores[cb]) cb = c;
        bestScore = scores[cb]; winBaud = cand[cb].baud; bestO = candOrder[cb];
        for (Order od : kOrders) {
            if (forceO && od != o.forceOrder) continue;
            bestOrderEvm.push_back({od, runTrial(y, fsC, winBaud, od, rect0 ? 0 : alpha0, 2500).evm});
        }
    }
    for (size_t c = 0; c < cand.size(); c++) cand[c].conf = std::clamp(1.0 - scores[c] / 0.9, 0.0, 1.0);
    std::stable_sort(cand.begin(), cand.end(), [](const RateCandidate& a, const RateCandidate& b) { return a.conf > b.conf; });
    A.rates = cand;
    if (bestScore > 0.75) { A.status = "signal found, but no PSK symbol rate fits"; A.orderEvm = bestOrderEvm; return A; }
    A.orderEvm = bestOrderEvm;
    A.order = bestO;
    const double baud = winBaud;
    // ---- 6 narrow the channel when the oversampling is large
    std::vector<cf> y2;
    const std::vector<cf>* yy = &y;
    if (fsC / baud > 12) {
        Channeliser c2;
        c2.configure(fsC, 0, std::max(baud * 3.0, 8.0), baud * 8);
        c2.process(y.data(), y.size(), y2);
        if (y2.size() > 1024) { fsC = c2.outRate(); yy = &y2; A.fsChan = fsC; }
    }
    // ---- 7 pulse shape
    bool rect = false;
    double aEst = estimateRolloff(yy->data(), yy->size(), fsC, baud, &rect);
    double alpha = alpha0; bool useRect = rect0;
    if (o.forceAlpha < -1.5) {
        const double opts[] = {aEst, 0.2, 0.35, 0.5, 1.0, -1};
        double best = 9;
        for (double a : opts) {
            const Trial t = runTrial(*yy, fsC, baud, bestO, a <= 0 ? 0 : a, 2500);
            if (t.evm < best * 0.97) { best = t.evm; alpha = a <= 0 ? 0.35 : a; useRect = a <= 0; }
        }
    }
    A.alpha = alpha; A.rect = useRect;
    char sh[64];
    if (useRect) snprintf(sh, sizeof sh, "rectangular"); else snprintf(sh, sizeof sh, "RRC %.2f", alpha);
    A.shape = sh;
    // ---- 8 final run
    Trial f = runTrial(*yy, fsC, baud, bestO, useRect ? 0 : alpha, 12000, true, 0.004);
    if (f.sym.size() < 200) { A.status = "need more samples"; return A; }
    A.baudRaw = f.spsMean > 0 ? fsC / f.spsMean : baud;
    const double snapped = o.snapStandard ? snapRate(A.baudRaw, 0.006) : 0;
    A.baudSnapped = snapped > 0;
    A.baud = A.baudSnapped ? snapped : A.baudRaw;
    if (o.forceBaud > 0) { A.baud = o.forceBaud; A.baudSnapped = false; }
    A.rateFound = true;
    if (A.baudSnapped && std::fabs(A.baud - baud) > 0.0005 * baud) f = runTrial(*yy, fsC, A.baud, bestO, useRect ? 0 : alpha, 12000, true, 0.004);
    A.baudConf = std::clamp(1.0 - f.evm / 0.9, 0.0, 1.0);
    A.orderConf = A.baudConf;
    A.evm = f.evm;
    A.merDb = -20 * std::log10(std::max(1e-3, f.evm));
    A.esn0Db = A.merDb;
    A.ebn0Db = A.esn0Db - 10 * std::log10((double)orderBits(bestO));
    A.bitRate = A.baud * orderBits(bestO);
    A.dopplerHzPerSec = f.dopplerHzPerSec;
    A.carrierHz += f.freqEndHz;
    A.locked = f.evm < 0.6 && f.lockMetric > 0.3;
    const size_t keep = std::min<size_t>(f.sym.size(), 2000);
    A.symbols.assign(f.sym.end() - (long)keep, f.sym.end());
    {   // normalise the constellation display
        double pw = 0;
        for (const cf& v : A.symbols) pw += std::norm(v);
        pw = std::sqrt(std::max(1e-20, pw / (double)A.symbols.size()));
        for (cf& v : A.symbols) v /= (float)pw;
    }
    // displays
    {
        const size_t nfft = pow2Floor(yy->size() / 4, 16384, 512);
        const std::vector<double> e = envelopeSpectrum(*yy, nfft);
        A.envHzPerBin = fsC / (double)nfft;
        const double med = std::max(1e-30, medianOf(e));
        for (double v : e) A.envSpecDb.push_back((float)(10 * std::log10(std::max(1e-30, v) / med)));
        A.envLineHz = A.baud;
        A.powM = bestO == Order::Bpsk ? 2 : bestO == Order::Psk8 ? 8 : 4;
        const std::vector<double> p = powerSpectrum(*yy, A.powM, nfft);
        const double pm = std::max(1e-30, medianOf(p));
        for (double v : p) A.powSpecDb.push_back((float)(10 * std::log10(std::max(1e-30, v) / pm)));
        A.powHzPerBin = fsC / (double)nfft / A.powM;
        A.powLineHz = 0;
    }
    snprintf(b, sizeof b, "%s, %.6g baud%s, %s, Eb/N0 %.1f dB%s", orderName(bestO), A.baud, A.baudSnapped ? " (standard)" : "", A.shape.c_str(), A.ebn0Db,
             A.locked ? "" : " (not locked)");
    A.status = std::string(A.locked ? "locked: " : "weak: ") + b;
    say(b);
    return A;
}

} // namespace dect2::pskd
