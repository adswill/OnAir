// ISM sensors receiver. See ism_rx.h.
#include "dect2/ism/ism_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/ism/ism_tel.h"
#include "dect2/ism/ism_sigfox.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kFrameSec = 128e-6;     // detection FFT length
constexpr double kHangSec = 0.025;       // a burst ends after this long without a bin over the floor
constexpr double kPreSec = 0.001, kPostSec = 0.0005;
constexpr double kGroupHz = 120e3;       // bins of one signal (FSK tones, drift) lie within this of its peak
constexpr double kApartHz = 150e3;       // a second signal in the same burst must be this far from the first
constexpr double kCutHz = 110e3;         // channel filter
constexpr double kDemodRate = 500e3;
constexpr double kSplitSec = 200e-6;     // an FSK burst whose carrier drops this long is two trains
}

struct IsmReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, offsetSet = 0, centerSet = 0;
    float thrSet = 13;
    bool narrowRetry = true;
    bool resetReq = true;
    std::atomic<bool> clearReq{false};
    std::function<void(const std::string&)> log;
    std::function<void(const IsmReading&, double)> tap;

    // analysis thread
    double rate = 0, offset = 0, center = 0;
    float thrDb = 13, thrLin = 20;
    int N = 256, dec = 4;
    double rd = 500e3;
    std::unique_ptr<Fft> fft;
    std::vector<float> win, floorP, excess, taps;
    std::vector<cf32> fbuf;
    int fpos = 0;
    uint64_t frames = 0;
    std::vector<cf32> ring;
    size_t ringMask = 0;
    uint64_t total = 0;
    bool inBurst = false;
    uint64_t bStart = 0, bLast = 0;
    int idle = 0, hangFrames = 94;
    uint64_t maxBurst = 0;
    double levelAcc = 0; size_t levelN = 0;
    IsmTelemetry tel, pub;
    SigfoxRx sfx;
    double lastPubSec = -1;
    bool changed = false;
    // the decoded lists (devices, log, unknown bursts) stay through resets: Clear and a loaded file reach the feed thread's copy
    // through these requests (mu), and pub at once
    bool loadReq = false;
    IsmTelemetry loaded;

    static void moveLists(IsmTelemetry& to, IsmTelemetry& from) {
        to.devices = std::move(from.devices); to.events = std::move(from.events); to.raws = std::move(from.raws);
        from.devices.clear(); from.events.clear(); from.raws.clear();
    }
    static void shiftTimes(IsmTelemetry& t, double dt) {
        for (auto& d : t.devices) d.lastSec += dt;
        for (auto& e : t.events) e.sec += dt;
        for (auto& r : t.raws) r.sec += dt;
    }

    void setup() {
        std::lock_guard<std::mutex> lk(mu);
        rate = rateSet; offset = offsetSet; center = centerSet; thrDb = thrSet;
        resetReq = false;
    }

    void init() {
        thrLin = (float)std::pow(10.0, thrDb / 10.0);
        N = 64;
        while (N < 4096 && N < rate * kFrameSec) N *= 2;
        fft = std::make_unique<Fft>(N);
        win.resize(N);
        for (int i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N));
        floorP.assign(N, 0); excess.assign(N, 0); fbuf.assign(N, cf32(0, 0));
        fpos = 0; frames = 0;
        dec = std::max(1, (int)std::lround(rate / kDemodRate));
        rd = rate / dec;
        const int nt = ((int)std::ceil(3.3 * rate / 80e3)) | 1;   // Hamming: about 80 kHz from pass to stop
        taps.resize(nt);
        double s = 0;
        for (int i = 0; i < nt; i++) {
            const double m = i - (nt - 1) / 2.0, fc = kCutHz / rate;
            const double h = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            taps[i] = (float)(h * (0.54 - 0.46 * std::cos(2 * M_PI * i / (nt - 1))));
            s += taps[i];
        }
        for (auto& t : taps) t = (float)(t / s);
        size_t rn = 1;
        while (rn < (size_t)(rate * 1.2) && rn < ((size_t)1 << 23)) rn <<= 1;
        ring.assign(rn, cf32(0, 0));
        ringMask = rn - 1;
        maxBurst = (uint64_t)(rn * 0.75);
        hangFrames = std::max(4, (int)(kHangSec * rate / N));
        const double t0 = (double)total / std::max(1.0, tel.inputRate);   // the clock starts again: the kept lists move back by it
        total = 0; inBurst = false; idle = 0;
        const uint64_t s0 = tel.seq;
        IsmTelemetry keep;
        moveLists(keep, tel);
        shiftTimes(keep, -t0);
        tel = IsmTelemetry();
        tel.seq = s0;
        moveLists(tel, keep);
        tel.inputRate = rate; tel.centerHz = center; tel.dialOffsetHz = offset;
        tel.state = 0;
        sfx.configure(rate, offset, center > 0 ? center + offset : 0);
        changed = true;
    }

    double binHz(int k) const { return (k < N / 2 ? k : k - N) * rate / N; }

    void frame(uint64_t endSample) {
        for (int i = 0; i < N; i++) fbuf[i] *= win[i];
        fft->forward(fbuf.data());
        std::vector<float> p(N);
        double tot = 0;
        for (int k = 0; k < N; k++) { p[k] = std::norm(fbuf[k]); tot += p[k]; }
        if (!std::isfinite(tot)) return;
        const bool learn = frames < 30;
        int act = 0;
        for (int k = 0; k < N; k++) {
            float& f = floorP[k];
            if (frames == 0) f = p[k];
            else if (learn) f = 0.8f * f + 0.2f * p[k];
            const bool over = !learn && f > 0 && p[k] > f * thrLin && k > 1 && k < N - 1;
            if (over) { act++; if (inBurst || true) excess[k] += p[k] - f; }
            if (!learn) f += (inBurst ? 0.00005f : p[k] < f * thrLin ? 0.01f : 0.00005f) * (p[k] - f);   // held during a burst, so a weak part of it does not lift the floor
            if (f < 1e-30f) f = 1e-30f;
        }
        frames++;
        if (act) {
            if (!inBurst) { inBurst = true; bStart = endSample - N; std::vector<float> e(N, 0); for (int k = 0; k < N; k++) if (p[k] > floorP[k] * thrLin) e[k] = p[k] - floorP[k]; excess = e; }
            bLast = endSample; idle = 0;
            if (bLast - bStart > maxBurst) {   // a carrier that came up: take it into the floor, drop the burst
                for (int k = 0; k < N; k++) if (p[k] > floorP[k] * thrLin) floorP[k] = p[k];
                inBurst = false;
                std::fill(excess.begin(), excess.end(), 0.f);
            }
        } else if (inBurst && ++idle >= hangFrames) {
            inBurst = false;
            burst();
            std::fill(excess.begin(), excess.end(), 0.f);
        } else if (!inBurst) std::fill(excess.begin(), excess.end(), 0.f);
    }

    // ---------------------------------------------------------------- one burst

    void burst() {
        const uint64_t pre = (uint64_t)(kPreSec * rate), post = (uint64_t)(kPostSec * rate);
        uint64_t s0 = bStart > pre ? bStart - pre : 0, s1 = std::min(total, bLast + post);
        if (total > ring.size() && s0 < total - ring.size()) s0 = total - ring.size();
        if (s1 <= s0 + 64) return;
        tel.bursts++;
        std::vector<cf32> x(s1 - s0);
        for (uint64_t i = s0; i < s1; i++) x[i - s0] = ring[i & ringMask];
        // the signals in it
        std::vector<float> e = excess;
        std::vector<double> found;
        const int grp = std::max(1, (int)std::ceil(kGroupHz / (rate / N)));
        float first = 0;
        for (int sig = 0; sig < 3; sig++) {
            int kb = -1;
            for (int k = 0; k < N; k++) if (e[k] > 0 && (kb < 0 || e[k] > e[kb])) kb = k;
            if (kb < 0) break;
            if (sig == 0) first = e[kb];
            else if (e[kb] < 0.02f * first) break;
            double sw = 0, sf = 0;
            const double fpk = binHz(kb);
            for (int d = -grp; d <= grp; d++) {
                const int k = (kb + d + N) % N;
                if (e[k] <= 0) continue;
                sw += e[k]; sf += e[k] * (fpk + d * rate / N);
            }
            const double fc = sw > 0 ? sf / sw : fpk;
            bool apart = true;
            for (double f : found) apart = apart && std::fabs(f - fc) >= kApartHz;
            const int clr = (int)std::ceil(kApartHz / (rate / N));
            for (int d = -clr; d <= clr; d++) e[(kb + d + N) % N] = 0;
            if (apart) found.push_back(fc);
        }
        for (double fc : found) demod(x, fc, s0);
    }

    void demod(const std::vector<cf32>& x, double fc, uint64_t s0) {
        const size_t L = x.size(), nt = taps.size();
        if (L < nt + 16) return;
        std::vector<cf32> m(L);
        const double w = -2 * M_PI * fc / rate;
        for (size_t i = 0; i < L; i++) { const double a = w * (double)(s0 + i); m[i] = x[i] * cf32((float)std::cos(a), (float)std::sin(a)); }
        const size_t M = (L - nt) / dec;
        std::vector<cf32> y(M);
        for (size_t k = 0; k < M; k++) {
            const cf32* p = &m[k * dec];
            float re = 0, im = 0;
            for (size_t t = 0; t < nt; t++) { re += taps[t] * p[t].real(); im += taps[t] * p[t].imag(); }
            y[k] = cf32(re, im);
        }
        // Two envelopes: the wide one (|y| over 8 us) says where an FSK carrier is; the narrow one (the channel cut to about +-25 kHz by
        // a 20 us average of the samples, then its magnitude over 8 us) slices OOK, whose carrier is narrow and whose centre is known to a
        // few kHz. The shortest OOK element here is about 100 us; FSK chips are judged by frequency, from the wide channel.
        const int W1 = std::max(2, (int)std::lround(rd * 20e-6)), W2 = std::max(1, (int)std::lround(rd * 8e-6));
        std::vector<float> magW(M), magN(M);
        {
            std::complex<double> ca(0, 0);
            std::vector<float> a(M), b(M);
            for (size_t k = 0; k < M; k++) {
                ca += std::complex<double>(y[k].real(), y[k].imag());
                if (k >= (size_t)W1) ca -= std::complex<double>(y[k - W1].real(), y[k - W1].imag());
                a[k] = (float)(std::abs(ca) / (double)std::min<size_t>(k + 1, (size_t)W1));
                b[k] = std::abs(y[k]);
            }
            double accA = 0, accB = 0;
            for (size_t k = 0; k < M; k++) {
                accA += a[k]; accB += b[k];
                if (k >= (size_t)W2) { accA -= a[k - W2]; accB -= b[k - W2]; }
                const double n = (double)std::min<size_t>(k + 1, (size_t)W2);
                magN[k] = (float)(accA / n); magW[k] = (float)(accB / n);
            }
        }
        const size_t head = std::min(M / 4, (size_t)(0.8 * kPreSec * rd)), tail = std::min(M / 4, (size_t)(0.8 * kPostSec * rd));
        const size_t skip = 12 + (size_t)W1;   // the filters' start
        if (head < skip + 16) return;
        double hp = 0;
        for (size_t k = skip; k < head; k++) hp += std::norm(y[k]);
        const double noiseP = std::max(1e-20, hp / (double)(head - skip));
        // on / off: levels from the noise before (and after) the burst and the middle of the samples clearly above it, a 2-sample debounce
        auto slice = [&](const std::vector<float>& mag, std::vector<uint8_t>& st) -> bool {
            auto stats = [&](size_t a, size_t b, double& mean, double& sd) {
                double s1 = 0, s2 = 0;
                for (size_t k = a; k < b; k++) { s1 += mag[k]; s2 += (double)mag[k] * mag[k]; }
                mean = s1 / (double)(b - a);
                sd = std::sqrt(std::max(0.0, s2 / (double)(b - a) - mean * mean));
            };
            double lo, sd;
            stats(skip, head, lo, sd);
            if (tail > 16) { double l2, s2; stats(M - tail, M, l2, s2); if (l2 < lo) { lo = l2; sd = s2; } }
            std::vector<float> sm;
            for (size_t k = 0; k < M; k++) if (mag[k] > lo + 6 * sd + 1e-12) sm.push_back(mag[k]);
            if (sm.size() < 20) return false;
            std::nth_element(sm.begin(), sm.begin() + (ptrdiff_t)(sm.size() / 2), sm.end());
            const double hi = sm[sm.size() / 2];
            if (!(hi > lo * 1.6)) return false;   // too weak to slice
            const float on = (float)(lo + 0.55 * (hi - lo)), off = (float)(lo + 0.45 * (hi - lo));
            st.assign(M, 0);
            int s = 0, cand = 0, run = 0;
            for (size_t k = 0; k < M; k++) {
                const int want = s ? (mag[k] >= off) : (mag[k] > on);
                if (want != s) {
                    if (want == cand) run++; else { cand = want; run = 1; }
                    if (run >= 2) { s = want; for (int b = 0; b < run; b++) st[k - b] = (uint8_t)s; run = 0; continue; }
                } else run = 0;
                st[k] = (uint8_t)s;
            }
            return true;
        };
        auto span = [&](const std::vector<uint8_t>& st, size_t& first, size_t& last, double& pOn) {
            size_t nOn = 0;
            first = M; last = 0; pOn = 0;
            for (size_t k = 0; k < M; k++) if (st[k]) { first = std::min(first, k); last = k; nOn++; pOn += std::norm(y[k]); }
            if (nOn) pOn /= (double)nOn;
            return nOn;
        };
        const double us = 1e6 / rd;
        const double fAbs = center > 0 ? center + fc : fc - offset;
        const double sec = (double)s0 / rate;
        // FSK: the carrier stays up and the frequency takes two values
        std::vector<uint8_t> st;
        size_t first = M, last = 0;
        double pOn = 0;
        if (slice(magW, st)) {
            const size_t nOn = span(st, first, last, pOn);
            if (nOn >= 10 && (double)nOn / (double)(last - first + 1) >= 0.9) {
                std::vector<float> f0(M, 0.f);
                for (size_t k = 1; k < M; k++) f0[k] = std::arg(y[k] * std::conj(y[k - 1]));
                {   // median of three: the clicks of the discriminator at low C/N are single samples, far from both tones
                    const std::vector<float> g = f0;
                    for (size_t k = 1; k + 1 < M; k++) f0[k] = std::max(std::min(g[k - 1], g[k]), std::min(std::max(g[k - 1], g[k]), g[k + 1]));
                }
                // two passes: the first finds the tones and the length of a chip, the second averages the frequency over a third of it
                std::vector<float> fr;
                float mid = 0, devHz = 0;
                int fw = 3;
                bool fsk = false;
                for (int pass = 0; pass < 2; pass++) {
                    fr.assign(M, 0.f);
                    double acc = 0;
                    for (size_t k = 0; k < M; k++) { acc += f0[k]; if (k >= (size_t)fw) acc -= f0[k - fw]; fr[k] = (float)(acc / fw); }
                    std::vector<float> v;
                    for (size_t k = first + (size_t)fw; k <= last; k++) if (st[k]) v.push_back(fr[k]);
                    if (v.size() <= 20) break;
                    std::vector<float> sv = v;
                    std::sort(sv.begin(), sv.end());
                    float c1 = sv[sv.size() / 10], c2 = sv[sv.size() * 9 / 10];
                    for (int it = 0; it < 12; it++) {
                        const float md = 0.5f * (c1 + c2);
                        double a = 0, b = 0; size_t na = 0, nb = 0;
                        for (float q : v) { if (q < md) { a += q; na++; } else { b += q; nb++; } }
                        if (na) c1 = (float)(a / na);
                        if (nb) c2 = (float)(b / nb);
                    }
                    mid = 0.5f * (c1 + c2);
                    devHz = (float)((c2 - c1) * rd / (4 * M_PI));
                    fsk = devHz > 4e3f;
                    if (!fsk || pass == 1) break;
                    std::vector<size_t> runs;   // the tone runs, for the chip length
                    size_t a = first;
                    for (size_t k = first + 1; k <= last; k++) if ((fr[k] > mid) != (fr[k - 1] > mid)) { runs.push_back(k - a); a = k; }
                    if (runs.size() < 8) break;
                    std::sort(runs.begin(), runs.end());
                    std::vector<size_t> big;
                    for (size_t r : runs) if (r * 10 >= runs[runs.size() / 2]) big.push_back(r);
                    const size_t chip = big.empty() ? 3 : big[big.size() / 4];
                    const int nfw = std::max(1, std::min(64, (int)(chip / 3)));
                    if (nfw == fw) break;
                    fw = nfw;
                }
                if (fsk) {
                    const float snr = (float)(10 * std::log10(std::max(1e-20, pOn) / noiseP));
                    const float rssi = (float)(10 * std::log10(std::max(1e-20, pOn)));
                    // the averaging delays the frequency by half its length: line it up with the envelope
                    const size_t dl = (size_t)(fw / 2);
                    auto mark = [&](size_t j) { return fr[std::min(M - 1, j + dl)] > mid; };
                    const size_t split = (size_t)(kSplitSec * rd);   // split where the carrier drops; the upper tone is the mark
                    size_t k = first;
                    while (k <= last) {
                        size_t e = k, offRun = 0;
                        while (e <= last) { if (!st[e]) { if (++offRun > split) break; } else offRun = 0; e++; }
                        const size_t end = e - offRun;
                        IsmTrain t; t.fsk = true;
                        size_t j = k;
                        while (j < end && !mark(j)) j++;
                        while (j < end) {
                            size_t a = j;
                            while (j < end && mark(j)) j++;
                            const size_t pl = j - a;
                            a = j;
                            while (j < end && !mark(j)) j++;
                            t.p.push_back({(float)(pl * us), (float)((j - a) * us)});
                        }
                        fold(t);
                        handle(t, fAbs, rssi, snr, devHz, sec);
                        k = e;
                        while (k <= last && !st[k]) k++;
                    }
                    return;
                }
            }
        }
        // OOK
        // 4.0: a burst that the standard envelope (+-25 kHz) did not give to any protocol is sliced again with narrower ones, 40 / 80 / 160 us
        // boxcars of the carrier (+-12.5 / 6 / 3 kHz): the noise falls with the filter, and the protocols' own checks (CRC, parity, repeats) accept or refuse the result
        auto narrowTry = [&]() -> bool {
            if (!narrowRetry) return false;
            std::complex<double> acc(0, 0);
            for (size_t k = 1; k < M; k++) acc += std::complex<double>(y[k].real(), y[k].imag()) * std::conj(std::complex<double>(y[k - 1].real(), y[k - 1].imag()));
            const double fe = std::arg(acc) * rd / (2 * M_PI);   // the carrier's offset from the centre found by the FFT
            std::vector<std::complex<double>> z(M);
            for (size_t k = 0; k < M; k++) z[k] = std::complex<double>(y[k].real(), y[k].imag()) * std::polar(1.0, -2 * M_PI * fe / rd * (double)k);
            for (const double usN : {40.0, 80.0, 160.0}) {
                const int w1n = std::max(2, (int)std::lround(rd * usN * 1e-6));
                std::vector<float> a(M), mg(M);
                std::complex<double> ca(0, 0);
                for (size_t k = 0; k < M; k++) {
                    ca += z[k];
                    if (k >= (size_t)w1n) ca -= z[k - (size_t)w1n];
                    a[k] = (float)(std::abs(ca) / (double)std::min<size_t>(k + 1, (size_t)w1n));
                }
                double acc2 = 0;
                for (size_t k = 0; k < M; k++) {
                    acc2 += a[k];
                    if (k >= (size_t)W2) acc2 -= a[k - (size_t)W2];
                    mg[k] = (float)(acc2 / (double)std::min<size_t>(k + 1, (size_t)W2));
                }
                std::vector<uint8_t> s2;
                size_t f2 = M, l2 = 0;
                double p2 = 0;
                if (!slice(mg, s2) || span(s2, f2, l2, p2) < 10) continue;
                IsmTrain t2; t2.fsk = false;
                size_t j2 = f2;
                while (j2 <= l2) {
                    size_t a2 = j2;
                    while (j2 <= l2 && s2[j2]) j2++;
                    const size_t pl = j2 - a2;
                    a2 = j2;
                    while (j2 <= l2 && !s2[j2]) j2++;
                    t2.p.push_back({(float)(pl * us), j2 > l2 ? 1e6f : (float)((j2 - a2) * us)});
                }
                fold(t2);
                if (ismDecode(t2).empty()) continue;
                handle(t2, fAbs + fe, (float)(10 * std::log10(std::max(1e-20, p2))), (float)(10 * std::log10(std::max(1e-20, p2) / noiseP)), 0, sec);
                return true;
            }
            return false;
        };
        if (!slice(magN, st)) { narrowTry(); return; }
        if (span(st, first, last, pOn) < 10) { narrowTry(); return; }
        const float snr = (float)(10 * std::log10(std::max(1e-20, pOn) / noiseP));
        const float rssi = (float)(10 * std::log10(std::max(1e-20, pOn)));
        IsmTrain t; t.fsk = false;
        size_t j = first;
        while (j <= last) {
            size_t a = j;
            while (j <= last && st[j]) j++;
            const size_t pl = j - a;
            a = j;
            while (j <= last && !st[j]) j++;
            t.p.push_back({(float)(pl * us), j > last ? 1e6f : (float)((j - a) * us)});
        }
        fold(t);
        if (ismDecode(t).empty()) {
            // a fast ASK (EnOcean: 8 us chips) is smeared by the narrow envelope: slice the channel envelope itself and keep that train
            // only when a protocol reads it, so slow bursts and noise are not reported twice
            std::vector<float> magF(M);
            magF[0] = std::abs(y[0]);
            for (size_t k = 1; k < M; k++) magF[k] = 0.5f * (std::abs(y[k]) + std::abs(y[k - 1]));
            std::vector<uint8_t> st2;
            size_t f2 = M, l2 = 0;
            double p2 = 0;
            if (slice(magF, st2) && span(st2, f2, l2, p2) >= 10) {
                IsmTrain t2; t2.fsk = false;
                size_t j2 = f2;
                while (j2 <= l2) {
                    size_t a2 = j2;
                    while (j2 <= l2 && st2[j2]) j2++;
                    const size_t pl = j2 - a2;
                    a2 = j2;
                    while (j2 <= l2 && !st2[j2]) j2++;
                    t2.p.push_back({(float)(pl * us), j2 > l2 ? 1e6f : (float)((j2 - a2) * us)});
                }
                fold(t2);
                if (!ismDecode(t2).empty()) { handle(t2, fAbs, rssi, snr, 0, sec); return; }
            }
        }
        if (ismDecode(t).empty() && narrowTry()) return;
        handle(t, fAbs, rssi, snr, 0, sec);
    }

    // Noise splits a pulse or a gap with a sliver of the other state. The shortest real element is found from the train itself (the
    // quarter point of the lengths, slivers under a tenth of the median left out), and anything under a third of it is merged into its
    // neighbours.
    void fold(IsmTrain& t) {
        if (t.p.size() < 4) return;
        std::vector<float> d;
        for (const auto& q : t.p) { d.push_back(q.pulseUs); d.push_back(q.gapUs); }
        const float last = d.back();
        d.pop_back();
        std::vector<float> s = d;
        std::sort(s.begin(), s.end());
        const float med = s[s.size() / 2];
        std::vector<float> big;
        for (float v : s) if (v >= 0.1f * med) big.push_back(v);
        if (big.empty()) return;
        const float thr = std::max((float)(2.5e6 / rd), big[big.size() / 4] / 3.f);
        for (size_t i = 1; i + 1 < d.size();) {
            if (d[i] < thr) { d[i - 1] += d[i] + d[i + 1]; d.erase(d.begin() + (ptrdiff_t)i, d.begin() + (ptrdiff_t)i + 2); }
            else i++;
        }
        if (d.size() >= 2 && d[0] < thr) { d.erase(d.begin(), d.begin() + 2); }
        d.push_back(last);
        IsmTrain o; o.fsk = t.fsk;
        for (size_t i = 0; i + 1 < d.size(); i += 2) o.p.push_back({d[i], d[i + 1]});
        t.p = o.p;
    }

    void handle(const IsmTrain& t, double fAbs, float rssi, float snr, float devHz, double sec) {
        if (t.p.size() < 3) return;
        const int64_t now = (int64_t)std::time(nullptr);
        std::vector<IsmReading> rs = ismDecode(t);
        if (rs.empty()) {
            if (t.p.size() < 8) return;
            IsmRaw r;
            r.wallTime = now; r.sec = sec; r.freqHz = fAbs; r.rssiDb = rssi; r.snrDb = snr; r.fsk = t.fsk; r.fskDevHz = devHz;
            r.guess = ismClassify(t);
            r.pulses.assign(t.p.begin(), t.p.begin() + (ptrdiff_t)std::min(t.p.size(), kIsmRawPulsesMax));
            tel.raws.insert(tel.raws.begin(), r);
            if (tel.raws.size() > kIsmRawMax) tel.raws.pop_back();
            IsmEvent ev;
            ev.wallTime = now; ev.sec = sec; ev.freqHz = fAbs; ev.rssiDb = rssi; ev.unknown = true;
            char b[160];
            snprintf(b, sizeof b, "%s %s, %zu pulses, short %.0f us, long %.0f us", t.fsk ? "FSK" : "OOK", r.guess.code.c_str(), t.p.size(), r.guess.shortUs, r.guess.longUs);
            ev.text = b;
            pushEvent(ev);
            tel.blocksBad++;
            changed = true;
            return;
        }
        for (auto& rd0 : rs) {
            if (tap) tap(rd0, fAbs);
            IsmDevice* d = nullptr;
            size_t di = 0;
            for (size_t i = 0; i < tel.devices.size(); i++) {
                auto& q = tel.devices[i];
                if (q.model == rd0.model && q.id == rd0.id && q.channel == rd0.channel && q.channelName == rd0.channelName) { d = &q; di = i; break; }
            }
            bool fresh = false;
            if (!d) {
                IsmDevice nd;
                nd.model = rd0.model; nd.kind = rd0.kind; nd.id = rd0.id; nd.channel = rd0.channel; nd.channelName = rd0.channelName;
                nd.firstWall = now;
                for (const auto& f : rd0.fields) if (f.text.empty() && !f.unit.empty()) { nd.histName = f.name; nd.histUnit = f.unit; break; }
                tel.devices.insert(tel.devices.begin(), nd);
                if (tel.devices.size() > kIsmDevMax) tel.devices.pop_back();
                d = &tel.devices[0]; di = 0;
                fresh = true;
            }
            bool same = !fresh && d->fields.size() == rd0.fields.size();
            for (size_t i = 0; same && i < d->fields.size(); i++) same = d->fields[i].value == rd0.fields[i].value && d->fields[i].text == rd0.fields[i].text;
            const bool dup = same && sec - d->lastSec < 2.0;
            d->fields = rd0.fields; d->battery = rd0.battery; d->mic = rd0.mic; d->kind = rd0.kind;
            d->freqHz = fAbs; d->rssiDb = rssi; d->snrDb = snr; d->fsk = t.fsk;
            d->lastWall = now; d->lastSec = sec;
            if (!dup) {
                d->count++;
                for (const auto& f : rd0.fields)
                    if (f.name == d->histName) { d->hist.push_back((float)f.value); if (d->hist.size() > kIsmHistMax) d->hist.erase(d->hist.begin()); }
                IsmEvent ev;
                ev.wallTime = now; ev.sec = sec; ev.freqHz = fAbs; ev.rssiDb = rssi;
                ev.model = rd0.model; ev.id = rd0.id; ev.text = ismReadingText(rd0.fields);
                pushEvent(ev);
                tel.blocksOk++;
                tel.snrDb = snr;
                if (fresh && log) {
                    char b[200];
                    snprintf(b, sizeof b, "ISM: %s, ID %s, at %.3f MHz", rd0.model.c_str(), rd0.id.c_str(), fAbs / 1e6);
                    log(b);
                }
            }
            if (di) std::rotate(tel.devices.begin(), tel.devices.begin() + (ptrdiff_t)di, tel.devices.begin() + (ptrdiff_t)di + 1);
            changed = true;
        }
    }

    void pushEvent(const IsmEvent& e) {
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kIsmLogMax) tel.events.pop_back();
    }

    void publish() {
        tel.timeSec = (double)total / std::max(1.0, rate);
        tel.state = 1;
        tel.dataValid = false;
        for (const auto& d : tel.devices) if (tel.timeSec - d.lastSec < 10) { tel.dataValid = true; break; }
        if (tel.dataValid) tel.state = 2;
        if (levelN) tel.levelDb = (float)(10 * std::log10(std::max(1e-20, levelAcc / (double)levelN)));
        double nf = 0;
        for (int k = 2; k < N - 1; k++) nf += floorP[k];
        tel.noiseDb = (float)(10 * std::log10(std::max(1e-30, nf / (N - 3)) / ((double)N * N * 0.375)));
        tel.sigfox = sfx.telemetry();
        tel.seq++;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
        changed = false;
        lastPubSec = tel.timeSec;
    }
};

IsmReceiver::IsmReceiver() : p_(std::make_unique<Impl>()) {}
IsmReceiver::~IsmReceiver() = default;

void IsmReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void IsmReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetSet = hz; p_->resetReq = true; }
void IsmReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->centerSet) { p_->centerSet = hz; p_->resetReq = true; } }
bool IsmReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= ismTuning().minSampleRate - 1; }
void IsmReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->resetReq = true; }
void IsmReceiver::clearDevices() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearReq = true; p_->loadReq = false;
    IsmTelemetry e;
    Impl::moveLists(p_->pub, e);
    p_->pub.seq++;
}
void IsmReceiver::clearResults() { clearDevices(); }
std::string IsmReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.devices, p_->pub.events, p_->pub.raws);
}
bool IsmReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    IsmTelemetry l;
    if (!results::unpack(blob, t0, l.devices, l.events, l.raws)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    p_->pub.devices = l.devices; p_->pub.events = l.events; p_->pub.raws = l.raws;
    Impl::moveLists(p_->loaded, l);
    p_->loadReq = true;
    p_->pub.seq++;
    return true;
}
void IsmReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void IsmReceiver::setReadingTap(std::function<void(const IsmReading&, double)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tap = std::move(cb); }
void IsmReceiver::setNarrowRetry(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->narrowRetry = on; }
void IsmReceiver::setThresholdDb(float db) { std::lock_guard<std::mutex> lk(p_->mu); p_->thrSet = std::max(8.f, std::min(25.f, db)); p_->resetReq = true; }

bool IsmReceiver::telemetry(IsmTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void IsmReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    bool rs;
    { std::lock_guard<std::mutex> lk(m.mu); rs = m.resetReq; }
    if (rs) { m.setup(); if (m.rate > 0) m.init(); }
    if (m.rate < ismTuning().minSampleRate - 1 || !m.fft) return;
    if (m.clearReq.exchange(false)) { m.tel.devices.clear(); m.tel.events.clear(); m.tel.raws.clear(); m.tel.blocksOk = m.tel.blocksBad = 0; m.sfx.clear(); m.changed = true; }
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.loadReq) { m.loadReq = false; Impl::moveLists(m.tel, m.loaded); m.changed = true; }
    }
    m.sfx.feed(x, n);
    for (size_t i = 0; i < n; i++) {
        cf32 v = x[i];
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.ring[m.total & m.ringMask] = v;
        m.total++;
        m.levelAcc += std::norm(v); m.levelN++;
        m.fbuf[m.fpos++] = v;
        if (m.fpos == m.N) { m.fpos = 0; m.frame(m.total); }
    }
    const double now = (double)m.total / m.rate;
    if (m.changed || now - m.lastPubSec >= 0.25) {
        m.publish();
        m.levelAcc = 0; m.levelN = 0;
    }
}

ModeTuning ismTuning() {
    ModeTuning t;
    t.stdMode = 30; t.id = "ism"; t.name = "ISM sensors";
    t.minMhz = 300; t.maxMhz = 930; t.defMhz = 433.92;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 250000;     // the radio sits 250 kHz above the dial, so a sensor on 433.92 MHz is not on the DC spike
    return t;
}

} // namespace dect2
