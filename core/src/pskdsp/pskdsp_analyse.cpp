// PSK demodulator: blind analysis of a block. Finds the signal, its symbol rate (the line of |x|^2 and the side lines of x^2 / x^4 around
// the carrier line), the modulation order (power-law lines, then a trial demodulation per order), the pulse shape, carrier, Doppler and the
// quality figures, and keeps the displays the analyser shows (the line spectra and the constellation).
#include "dect2/pskdsp/pskdsp.h"
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::pskd {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

// averaged power spectrum, N bins (zero padded x2), DC at bin 0 (not centred)
std::vector<double> spectrum(const std::vector<cd>& v, size_t& N) {
    const size_t blk = std::min<size_t>(signallab::nextPow2(std::max<size_t>(v.size() / 2, 64)), 65536);
    N = blk * 2;
    std::vector<double> acc(N, 0.0);
    std::vector<cd> b(N);
    size_t used = 0;
    for (size_t st = 0; st + blk / 2 <= v.size() && used < (1u << 21); st += blk) {
        const size_t e = std::min(v.size(), st + blk);
        std::fill(b.begin(), b.end(), cd(0, 0));
        for (size_t i = st; i < e; i++) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)(i - st) / (double)blk);
            b[i - st] = v[i] * w;
        }
        signallab::fft(b);
        for (size_t i = 0; i < N; i++) acc[i] += std::norm(b[i]);
        used += e - st;
    }
    return acc;
}
// the strength of bin k over its neighbourhood (dB): the peak (with its neighbour bins) against the mean further out
double lineDb(const std::vector<double>& s, long k, long W) {
    const long N = (long)s.size();
    auto at = [&](long i) { return s[(size_t)(((i % N) + N) % N)]; };
    const double pk = at(k - 1) + at(k) + at(k + 1);   // the zero padding spreads a line over two or three bins
    double m = 0; long c = 0;
    for (long i = k - W; i <= k + W; i++) if (std::labs(i - k) > 4) { m += at(i); c++; }
    m = c ? 3 * m / (double)c : 0;
    return 10 * std::log10(std::max(1e-30, pk) / std::max(1e-30, m));
}
long argmaxRange(const std::vector<double>& s, long a, long b) {
    const long N = (long)s.size();
    long best = a; double bv = -1;
    for (long i = a; i <= b; i++) { const double v = s[(size_t)(((i % N) + N) % N)]; if (v > bv) { bv = v; best = i; } }
    return best;
}
double interp(const std::vector<double>& s, long k) {   // parabolic peak offset in bins
    const long N = (long)s.size();
    auto at = [&](long i) { return s[(size_t)(((i % N) + N) % N)]; };
    const double a = at(k - 1), b = at(k), c = at(k + 1), d = a - 2 * b + c;
    return d != 0 ? std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
}
double binHz(long k, size_t N, double fs) {   // signed frequency of a (fractional) bin
    double f = (double)k * fs / (double)N;
    if (f > fs / 2) f -= fs;
    return f;
}

// usedOdd: the share of the decisions on the odd points (QPSK: the second diagonal, 8PSK: the points between the QPSK ones); a lower order
// sent through a higher order's demodulator leaves them almost empty
struct Trial { double evm = 9; std::vector<cf> sym; DemodStats st; double slope = 0; double usedOdd = 0; };
Trial trial(const std::vector<cf>& y, double fs, double baud, Order o, double alpha, double initHz) {
    Trial t;
    DemodConfig c;
    c.order = o; c.sps = fs / baud; c.alpha = alpha; c.initFreq = 2 * kPi * initHz / fs;
    if (c.sps < 1.6) return t;
    Demod d;
    d.configure(c);
    std::vector<cf> sym;
    // in chunks, so the loop frequency can be followed over the block (the Doppler slope)
    const size_t chunks = 8, step = (y.size() + chunks - 1) / chunks;
    std::vector<double> ft, ff;
    for (size_t s = 0; s < y.size(); s += step) {
        d.process(y.data() + s, std::min(step, y.size() - s), sym);
        ft.push_back((double)(s + std::min(step, y.size() - s)) / fs);
        ff.push_back(d.stats().freq * fs / (2 * kPi));
    }
    t.st = d.stats();
    if (sym.size() < 40) return t;
    const size_t from = sym.size() * 2 / 5;
    double e = 0, odd = 0;
    for (size_t i = from; i < sym.size(); i++) {
        int k = 0;
        const cf p = decide(sym[i], o, &k);
        e += std::norm(sym[i] - p);
        if (k & 1) odd++;
    }
    const double n = (double)(sym.size() - from);
    t.evm = std::sqrt(e / n);
    t.usedOdd = odd / n;
    const size_t keep = std::min<size_t>(sym.size() - from, 2048);
    t.sym.assign(sym.end() - (long)keep, sym.end());
    // slope of the loop frequency over the second half of the block
    if (ft.size() >= 4) {
        const size_t a = ft.size() / 2;
        double mt = 0, mf = 0;
        for (size_t i = a; i < ft.size(); i++) { mt += ft[i]; mf += ff[i]; }
        mt /= (double)(ft.size() - a); mf /= (double)(ft.size() - a);
        double num = 0, den = 0;
        for (size_t i = a; i < ft.size(); i++) { num += (ft[i] - mt) * (ff[i] - mf); den += (ft[i] - mt) * (ft[i] - mt); }
        t.slope = den > 0 ? num / den : 0;
    }
    return t;
}

const double kStdRates[] = {50, 75, 100, 110, 125, 150, 200, 250, 300, 400, 500, 600, 625, 800, 1000, 1200, 1250, 1500, 1600, 1800, 2000, 2400,
                            2500, 3000, 3200, 3600, 4000, 4800, 5000, 6000, 6250, 7200, 8000, 9600, 10000, 12000, 12500, 14400, 16000, 19200,
                            20000, 24000, 25000, 28800, 32000, 36000, 38400, 40000, 48000, 50000, 57600, 64000, 72000, 76800, 80000, 96000,
                            100000, 115200, 125000, 128000, 144000, 153600, 160000, 192000, 200000, 230400, 250000, 256000, 288000, 307200,
                            320000, 384000, 400000, 460800, 500000, 512000, 576000, 614400, 640000, 665400, 768000, 800000, 921600,
                            1000000, 1024000, 1152000, 1200000, 1250000, 1500000, 1536000, 2000000, 2048000, 2500000, 3000000};
} // namespace

double snapRate(double b, double tol) {
    double best = 0, bd = 1e9;
    for (double r : kStdRates) {
        const double d = std::fabs(b - r) / r;
        if (d < bd) { bd = d; best = r; }
    }
    return bd <= tol ? best : 0;
}

double estimateRolloff(const cf* y, size_t n, double fs, double baud, bool* rect, double bandHz) {
    // the share of the power beyond +-0.75 x baud (up to the channel edge): a few % for rectangular pulses, about none for a root-raised cosine up to 0.5
    const std::vector<double> P = signallab::psd(y, n, 4096);
    const size_t N = P.size();
    std::vector<double> s(P);
    std::vector<double> srt;
    for (size_t i = 0; i < N; i++) {
        const double f = ((double)i - N / 2.0) * fs / (double)N;
        if (bandHz <= 0 || std::fabs(f) < 0.45 * bandHz) srt.push_back(s[i]);
    }
    if (srt.empty()) srt = s;
    std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() / 20), srt.end());
    const double fl = srt[srt.size() / 20];
    double in = 0, out = 0, occ = 0;
    std::vector<double> cum;
    for (size_t i = 0; i < N; i++) {
        const double f = ((double)i - N / 2.0) * fs / (double)N;
        const double v = std::max(0.0, s[i] - fl);
        if (std::fabs(f) <= 0.75 * baud) in += v; else if (std::fabs(f) <= 3 * baud) out += v;
    }
    const bool r = in + out > 0 && out / (in + out) > 0.02;   // rectangular about 0.035 inside the analysis channel, RRC up to 0.5 below 0.01
    if (rect) *rect = r;
    // 99 % width over the symbol rate gives 1 + alpha
    double tot = 0;
    for (size_t i = 0; i < N; i++) { const double f = ((double)i - N / 2.0) * fs / (double)N; if (std::fabs(f) <= 1.5 * baud) tot += std::max(0.0, s[i] - fl); }
    double acc = 0; size_t lo = 0, hi = N - 1;
    for (size_t i = 0; i < N; i++) { const double f = ((double)i - N / 2.0) * fs / (double)N; if (std::fabs(f) > 1.5 * baud) continue; acc += std::max(0.0, s[i] - fl); if (acc >= 0.01 * tot) { lo = i; break; } }
    acc = 0;
    for (size_t i = N; i-- > 0;) { const double f = ((double)i - N / 2.0) * fs / (double)N; if (std::fabs(f) > 1.5 * baud) continue; acc += std::max(0.0, s[i] - fl); if (acc >= 0.01 * tot) { hi = i; break; } }
    occ = (double)(hi > lo ? hi - lo : 1) * fs / (double)N;
    return std::clamp(occ / baud - 1.0, 0.05, 1.0);
}

Analysis analyse(const cf* x0, size_t n0, double fs, const AnalyseOptions& o) {
    Analysis a;
    const cf* x = x0;
    size_t n = n0;
    if (o.maxSeconds > 0 && (double)n > o.maxSeconds * fs) { const size_t m = (size_t)(o.maxSeconds * fs); x += n - m; n = m; }
    char line[256];
    if (n < 256 || fs <= 0) { a.status = "waiting for samples"; return a; }
    // 1. where the signal is
    if (o.haveCarrier && o.bwHz > 0) {
        a.carrier.found = true; a.carrier.offsetHz = o.carrierHz; a.carrier.bw3Hz = o.bwHz; a.carrier.bw10Hz = o.bwHz; a.carrier.occHz = o.bwHz;
        const Carrier c = findCarrier(x, n, fs, true, o.carrierHz, o.bwHz, o.bandHz);
        if (c.found) { a.carrier = c; }
    } else a.carrier = o.nearCentre ? findCarrier(x, n, fs, true, 0, o.nearMaxHz, o.bandHz) : findCarrier(x, n, fs, false, 0, 0, o.bandHz);
    if (!a.carrier.found) { a.status = "searching for a signal"; a.log.push_back("no signal above the noise"); return a; }
    a.signal = true;
    const Carrier& c = a.carrier;
    std::snprintf(line, sizeof line, "signal at %+.1f Hz, -3 dB width %.1f Hz, 99 %% width %.1f Hz, SNR %.1f dB", c.offsetHz, c.bw3Hz, c.occHz, c.snrDb);
    a.log.push_back(line);
    // 2. the channel around it
    double pass = o.forceBaud > 0 ? o.forceBaud * 2.4 : std::max({c.bw3Hz * 2.4, std::min(c.occHz, c.bw3Hz * 4.0) * 1.3, 8.0});
    pass = std::min(pass, fs * 0.9);
    std::vector<cf> in(x, x + n);
    double fsc = fs;
    std::vector<cf> y = signallab::channelise(in, fs, c.offsetHz, pass * 0.85, fsc);
    if (y.size() > (1u << 20)) y.erase(y.begin(), y.end() - (long)(1u << 20));
    a.fsChan = fsc;
    // 3. symbol rate
    std::vector<cd> env(y.size()), sq(y.size()), q4(y.size()), q8(y.size());
    {
        double m = 0;
        for (size_t i = 0; i < y.size(); i++) m += std::norm(y[i]);
        m /= (double)std::max<size_t>(1, y.size());
        const double g = m > 0 ? 1.0 / std::sqrt(m) : 1.0;
        for (size_t i = 0; i < y.size(); i++) {
            const cd v = cd(y[i].real(), y[i].imag()) * g;
            env[i] = std::norm(v) - 1.0;
            sq[i] = v * v;
            q4[i] = sq[i] * sq[i];
            q8[i] = q4[i] * q4[i];
        }
    }
    size_t Ne = 0, N2 = 0, N4 = 0, N8 = 0;
    const std::vector<double> Se = spectrum(env, Ne), S2 = spectrum(sq, N2), S4 = spectrum(q4, N4), S8 = spectrum(q8, N8);
    const double bwRef = c.bw3Hz > 0 ? c.bw3Hz : fsc / 8;
    const double lo = std::max(0.45 * bwRef, 4 * fsc / (double)Ne), hi = std::min(1.75 * bwRef, fsc / 2.3);
    const long W = std::max<long>(24, (long)Ne / 512);
    // power-law lines: the carrier at M x the offset
    auto powLine = [&](const std::vector<double>& S, size_t N, double& hz) {
        const long k = argmaxRange(S, -(long)N / 2 + 1, (long)N / 2 - 1);
        hz = binHz(k, N, fsc) + interp(S, k) * fsc / (double)N;
        return lineDb(S, k, std::max<long>(24, (long)N / 512));
    };
    double c2 = 0, c4 = 0, c8 = 0;
    bool oqHint = false;
    a.line2 = powLine(S2, N2, c2); a.line4 = powLine(S4, N4, c4); a.line8 = powLine(S8, N8, c8);
    if (o.forceBaud > 0) {
        a.baud = o.forceBaud; a.baudConf = 1; a.rateFound = true;
        a.rates.push_back({o.forceBaud, 1.0, "set by hand", false});
    } else if (lo < hi) {
        // (a) the line of |x|^2 at the symbol rate
        {
            const long ka = (long)std::floor(lo * (double)Ne / fsc), kb = (long)std::ceil(hi * (double)Ne / fsc);
            const long k = argmaxRange(Se, ka, kb);
            const double db = lineDb(Se, k, W);
            a.rates.push_back({((double)k + interp(Se, k)) * fsc / (double)Ne, db, "|x|^2 line", false});
            a.lineEnv = db; a.envLineHz = a.rates.back().baud;
        }
        // (b, c) side lines of the carrier line in x^2 and x^4, both sides added
        auto side = [&](const std::vector<double>& S, size_t N, double cHz, const char* how) {
            const long kc = (long)std::lround(cHz * (double)N / fsc);
            const long ka = (long)std::floor(lo * (double)N / fsc), kb = (long)std::ceil(hi * (double)N / fsc);
            std::vector<double> sum((size_t)(kb - ka + 1), 0.0);
            const long NN = (long)N;
            auto at = [&](long i) { return S[(size_t)(((i % NN) + NN) % NN)]; };
            long best = ka; double bv = -1;
            for (long d = ka; d <= kb; d++) { const double v = at(kc + d) + at(kc - d); if (v > bv) { bv = v; best = d; } }
            const double db = 0.5 * (lineDb(S, kc + best, std::max<long>(24, NN / 512)) + lineDb(S, kc - best, std::max<long>(24, NN / 512)));
            a.rates.push_back({(double)best * fsc / (double)N, db, how, false});
        };
        if (a.line2 > 10) side(S2, N2, c2, "x^2 side lines");
        if (a.line4 > 10 && a.line4 > a.line2 - 3) side(S4, N4, c4, "x^4 side lines");
        // OQPSK: x^2 has no carrier line but two lines at twice the carrier +- the symbol rate (the I and Q rails' lines add, the carrier ones cancel)
        {
            const long NN = (long)N2;
            auto at = [&](long i) { return S2[(size_t)(((i % NN) + NN) % NN)]; };
            const long da = (long)std::floor(lo * (double)N2 / fsc), db = (long)std::ceil(hi * (double)N2 / fsc);
            const long cr = std::max<long>(2, (long)(0.03 * bwRef * (double)N2 / fsc));
            long bk = 0, bd = da; double bv = -1;
            for (long kc = -cr; kc <= cr; kc++)
                for (long d = std::max<long>(da, 3); d <= db; d++) { const double v = at(kc + d) + at(kc - d); if (v > bv) { bv = v; bk = kc; bd = d; } }
            const double db2 = 0.5 * (lineDb(S2, bk + bd, std::max<long>(24, NN / 512)) + lineDb(S2, bk - bd, std::max<long>(24, NN / 512)));
            std::snprintf(line, sizeof line, "x^2 pair lines: %.1f dB at +-%.1f Hz around %+.1f Hz", db2, (double)bd * fsc / (double)N2, binHz(bk, N2, fsc));
            a.log.push_back(line);
            const double mid = lineDb(S2, bk, std::max<long>(24, NN / 512));
            if (db2 > 9 && mid < db2 - 6) {
                a.rates.push_back({(double)bd * fsc / (double)N2, db2, "x^2 line pair without a carrier line (offset QPSK)", false});
                oqHint = true; c2 = binHz(bk, N2, fsc);
            }
        }
        // OQPSK with a small roll-off: its envelope is nearly constant, so take the carrier phase from x^4 block by block (Viterbi and
        // Viterbi), put the points on the diagonals and look at I^2 - Q^2: the two rails' lines at the symbol rate are half a symbol apart,
        // so they add there (and cancel in |x|^2)
        if (a.line4 > 10) {
            const size_t L = std::clamp<size_t>((size_t)(64 * fsc / std::max(1.0, bwRef)), 64, 1u << 15);
            std::vector<cd> rail(y.size());
            double prevTh = 0; bool have = false;
            for (size_t st = 0; st < y.size(); st += L) {
                const size_t e = std::min(y.size(), st + L);
                cd acc(0, 0);
                for (size_t i = st; i < e; i++) acc += q4[i] * std::polar(1.0, -2 * kPi * c4 * (double)i / fsc);   // the x^4 line's frequency removed first
                double th = (std::arg(acc) - kPi) / 4;
                if (have) { while (th - prevTh > kPi / 4) th -= kPi / 2; while (th - prevTh < -kPi / 4) th += kPi / 2; }
                prevTh = th; have = true;
                for (size_t i = st; i < e; i++) {
                    const cd z = cd(y[i].real(), y[i].imag()) * std::polar(1.0, -th - 2 * kPi * c4 / 4 * (double)i / fsc);
                    rail[i] = z.real() * z.real() - z.imag() * z.imag();
                }
            }
            size_t Nr = 0;
            const std::vector<double> Sr = spectrum(rail, Nr);
            const long ka = (long)std::floor(lo * (double)Nr / fsc), kb = (long)std::ceil(hi * (double)Nr / fsc);
            const long k = argmaxRange(Sr, ka, kb);
            const double db = lineDb(Sr, k, std::max<long>(24, (long)Nr / 512));
            if (db > a.lineEnv + 3) { a.rates.push_back({((double)k + interp(Sr, k)) * fsc / (double)Nr, db, "I^2 - Q^2 line after the x^4 carrier (offset QPSK)", false}); oqHint = true; }
        }
        std::sort(a.rates.begin(), a.rates.end(), [](const RateCandidate& p, const RateCandidate& q) { return p.conf > q.conf; });
        // votes: candidates within 2 % of the best add half their strength
        const RateCandidate best = a.rates[0];
        double conf = best.conf;
        for (size_t i = 1; i < a.rates.size(); i++) if (std::fabs(a.rates[i].baud - best.baud) < 0.02 * best.baud) conf += 0.5 * a.rates[i].conf;
        a.baud = best.baud; a.baudConf = std::clamp((conf - 6.0) / 14.0, 0.0, 1.0);
        a.rateFound = best.conf >= 7.5;
        for (auto& r : a.rates) r.conf = std::clamp((r.conf - 6.0) / 14.0, 0.0, 1.0);
    }
    // displays: the |x|^2 spectrum up to fs/2 and the strongest power-law spectrum, 512 points each
    {
        const size_t pts = 512;
        a.envSpecDb.assign(pts, -100.f);
        for (size_t i = 0; i < pts; i++) {
            double m = 0; const size_t b0 = i * (Ne / 2) / pts, b1 = std::max(b0 + 1, (i + 1) * (Ne / 2) / pts);
            for (size_t k = b0; k < b1; k++) m = std::max(m, Se[k]);
            a.envSpecDb[i] = (float)(10 * std::log10(m + 1e-30));
        }
        a.envHzPerBin = fsc / 2 / (double)pts;
        const bool use4 = a.line4 > a.line2 + 3 && a.line4 >= a.line8 - 3, use8 = a.line8 > a.line4 + 3 && a.line8 > a.line2 + 3;
        const std::vector<double>& S = use8 ? S8 : use4 ? S4 : S2;
        const size_t N = use8 ? N8 : use4 ? N4 : N2;
        a.powM = use8 ? 8 : use4 ? 4 : 2;
        a.powSpecDb.assign(pts, -100.f);
        for (size_t i = 0; i < pts; i++) {
            double m = 0;
            const long b0 = (long)(i * N / pts) - (long)N / 2, b1 = std::max(b0 + 1, (long)((i + 1) * N / pts) - (long)N / 2);
            for (long k = b0; k < b1; k++) m = std::max(m, S[(size_t)((k + (long)N) % (long)N)]);
            a.powSpecDb[i] = (float)(10 * std::log10(m + 1e-30));
        }
        a.powHzPerBin = fsc / (double)pts;
        a.powLineHz = use8 ? c8 : use4 ? c4 : c2;
    }
    if (!a.rateFound) {
        std::snprintf(line, sizeof line, "lines: |x|^2 %.1f dB, x^2 %.1f, x^4 %.1f, x^8 %.1f dB; best rate candidate %.1f Bd", a.lineEnv, a.line2, a.line4, a.line8,
                      a.rates.empty() ? 0.0 : a.rates[0].baud);
        a.log.push_back(line);
        a.status = "signal found, looking for its symbol rate";
        a.log.push_back("no clear symbol-rate line yet");
        return a;
    }
    a.baudRaw = a.baud;
    if (o.snapStandard) { const double s = snapRate(a.baud, 0.02); if (s > 0) { a.baud = s; a.baudSnapped = true; } }
    std::snprintf(line, sizeof line, "symbol rate line at %.2f Bd (%s)%s", a.baudRaw, a.rates.empty() ? "" : a.rates[0].how.c_str(), a.baudSnapped ? ", near a standard rate" : "");
    a.log.push_back(line);
    // 4. order: a trial demodulation per order; a lower order wins unless a higher one is clearly better
    auto initFor = [&](Order od) { if (od == Order::Oqpsk && oqHint) return c2 / 2; return od == Order::Bpsk ? (a.line2 > 10 ? c2 / 2 : 0) : od == Order::Psk8 ? (a.line8 > 10 ? c8 / 8 : 0) : (a.line4 > 10 ? c4 / 4 : 0); };
    const double alpha0 = o.forceAlpha >= 0 ? o.forceAlpha : o.forceAlpha == -1 ? 0.0 : 0.35;
    std::vector<Order> orders;
    if (o.forceOrder != Order::Auto) orders = {o.forceOrder}; else orders = {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8};
    Trial best; Order bo = orders[0];
    std::vector<Trial> tr;
    // the trial demodulations look at the newest 5000 symbols at most (the cost of an analysis stays bounded for fast signals)
    const size_t maxT = (size_t)std::max(2048.0, 5000.0 * fsc / a.baud);
    const std::vector<cf> yt(y.size() > maxT ? y.end() - (long)maxT : y.begin(), y.end());
    for (Order od : orders) {
        tr.push_back(trial(yt, fsc, a.baud, od, alpha0, initFor(od)));
        a.orderEvm.push_back({od, tr.back().evm});
    }
    if (orders.size() == 1) { best = tr[0]; bo = orders[0]; a.orderConf = 1; }
    else {
        // the lowest order that fits: BPSK is a subset of QPSK (and QPSK of 8PSK), so a higher order needs its other points in use too
        auto spread = [](const Trial& t) { return std::min(t.usedOdd, 1.0 - t.usedOdd); };
        size_t bi = 0;
        const size_t qo = (oqHint ? tr[2].evm < tr[1].evm * 1.15 : tr[2].evm < tr[1].evm * 0.85) ? 2 : 1;
        if (spread(tr[qo]) > 0.25 && tr[qo].evm < tr[0].evm * 0.8) bi = qo;
        if (tr[3].evm < 0.6 * tr[bi].evm && spread(tr[3]) > 0.3) bi = 3;
        best = tr[bi]; bo = orders[bi];
        double second = 9;
        for (size_t i = 0; i < tr.size(); i++) if (i != bi) second = std::min(second, tr[i].evm);
        a.orderConf = std::clamp(1.0 - best.evm / std::max(1e-9, second), 0.0, 1.0);
    }
    a.order = bo;
    {
        std::string t = "trial demodulation (EVM, odd points):";
        for (size_t i = 0; i < tr.size(); i++) { std::snprintf(line, sizeof line, " %s %.0f %% %.2f", orderName(orders[i]), 100 * tr[i].evm, tr[i].usedOdd); t += line; }
        a.log.push_back(t);
    }
    // 5. pulse shape
    if (o.forceAlpha == -2) {
        bool rect = false;
        const double est = estimateRolloff(y.data(), y.size(), fsc, a.baud, &rect, pass);
        const double cands[] = {-1, 0.2, 0.35, 0.5, 0.8};
        double be = best.evm, ba = alpha0;
        for (double al : cands) {
            if (std::fabs(al - alpha0) < 1e-9) continue;
            Trial t = trial(yt, fsc, a.baud, bo, al < 0 ? 0.0 : al, initFor(bo));
            const double need = al < 0 && !rect ? 0.8 : 0.97;   // rectangular only when the spectrum agrees, or clearly better
            if (t.evm < be * need) { be = t.evm; ba = al; best = t; }
        }
        a.rect = ba < 0; a.alpha = a.rect ? 0 : ba;
        std::snprintf(line, sizeof line, "pulse shape: %s (spectrum says %s, roll-off about %.2f)", a.rect ? "rectangular" : "root-raised cosine", rect ? "rectangular" : "shaped", est);
        a.log.push_back(line);
    } else { a.rect = o.forceAlpha == -1; a.alpha = a.rect ? 0 : alpha0; }
    char shp[48];
    if (a.rect) std::snprintf(shp, sizeof shp, "rectangular"); else std::snprintf(shp, sizeof shp, "RRC %.2f", a.alpha);
    a.shape = shp;
    // 6. figures from the winning trial
    a.symbols = best.sym;
    a.evm = best.evm;
    if (best.st.spsNow > 0) {
        const double raw = fsc / best.st.spsNow;
        a.baudRaw = raw;
        if (!a.baudSnapped || std::fabs(raw - a.baud) / a.baud > 0.006) { if (o.forceBaud <= 0) a.baud = raw; a.baudSnapped = false; }
    }
    a.carrierHz = c.offsetHz + best.st.freq * fsc / (2 * kPi);
    a.dopplerHzPerSec = best.slope;
    a.merDb = best.evm > 0 ? -20 * std::log10(best.evm) : 60;
    a.esn0Db = a.merDb;
    a.bitRate = a.baud * orderBits(bo);
    a.ebn0Db = a.esn0Db - 10 * std::log10((double)orderBits(bo));
    const double lockEvm = bo == Order::Psk8 ? 0.28 : bo == Order::Bpsk ? 0.55 : 0.42;
    a.locked = best.evm < lockEvm && best.sym.size() >= 64;
    std::snprintf(line, sizeof line, "%s %s at %.2f Bd (%s), EVM %.0f %%, Eb/N0 about %.1f dB, carrier %+.1f Hz", a.locked ? "locked:" : "no clean lock:",
                  orderName(bo), a.baud, a.shape.c_str(), 100 * best.evm, a.ebn0Db, a.carrierHz);
    a.log.push_back(line);
    if (a.locked) {
        std::snprintf(line, sizeof line, "locked at %.0f baud %s, Eb/N0 %.0f dB", a.baud, orderName(bo), a.ebn0Db);
        a.status = line;
    } else a.status = "signal and symbol rate found, the constellation does not settle";
    return a;
}

} // namespace dect2::pskd
