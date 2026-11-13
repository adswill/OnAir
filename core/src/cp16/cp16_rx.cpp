// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CP16 receiver (see cp16_rx.h).
#include "dect2/cp16/cp16_rx.h"
#include "dect2/cp16/cp16_proto.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.141592653589793;
constexpr double kBase = marine::kBaseRate;   // 24 kHz from the front end
constexpr double kFs = 8000;                   // analysis rate
constexpr int kDec = 3;
constexpr int kN = 4096;                       // spectrum: 1.95 Hz bins
constexpr double kBin = kFs / kN;
constexpr double kHistSec = 32, kWinSec = 20, kFirstSec = 5, kEverySec = 1;
constexpr int kStep = 4;                       // envelope step, samples (0.5 ms)
constexpr int kMaxShift = 4;                   // the comb can sit this many carriers off: the outer columns of a glyph are often blank
constexpr int kPos = cp16::kCarriers + 2 * kMaxShift;   // comb positions read
constexpr float kCombMin = 0.12f;              // comb contrast (log10 units) to take the carriers as found
constexpr float kTimeRatio = 6.f;              // the row line over the median of the slope spectrum, to take the rows as found
constexpr double kConcMin = 0.25;              // concentration of the folded edges to take the rows as found
constexpr float kSure = 0.62f, kSureMargin = 0.03f, kShow = 0.40f;

struct Decim {   // complex FIR low-pass and down by kDec
    std::vector<float> taps;
    std::vector<cf32> hist;
    int pos = 0, phase = 0, n = 0;
    void setup() {
        n = 63;
        taps.resize((size_t)n);
        const double fc = 3700.0 / kBase;
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double k = i - (n - 1) / 2.0;
            const double s = k == 0 ? 2 * fc : std::sin(2 * kPi * fc * k) / (kPi * k);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
            taps[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& t : taps) t = (float)(t / sum);
        hist.assign((size_t)(2 * n), cf32(0, 0));
        pos = phase = 0;
    }
    bool push(cf32 x, cf32& out) {
        hist[(size_t)pos] = x; hist[(size_t)(pos + n)] = x;
        if (++pos >= n) pos = 0;
        if (++phase < kDec) return false;
        phase = 0;
        const cf32* w = &hist[(size_t)pos];
        float re = 0, im = 0;
        for (int k = 0; k < n; k++) { re += taps[(size_t)k] * w[k].real(); im += taps[(size_t)k] * w[k].imag(); }
        out = cf32(re, im);
        return true;
    }
};

struct Row {
    uint64_t serial = 0;
    double t = 0;                  // seconds of signal at its start
    float v[kPos] = {};            // amplitudes at the comb positions -kMaxShift .. 15 + kMaxShift (ascending baseband frequency)
    float d[kPos] = {};            // the same as dots 0 .. 1
};

// one way of reading the comb: the 16 columns start this many carriers off the found comb
constexpr int kNumArr = 2 * kMaxShift + 1;
const int kArrs[kNumArr] = {0, -1, 1, -2, 2, -3, 3, -4, 4};
} // namespace

struct Cp16Receiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    Cp16Telemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false}, loadReq{false};
    std::string loadText;
    double manBase = 0, manSpacing = 0, manChar = 0;
    int manSide = 0;
    bool readOn = true;
    const bool dbg = std::getenv("CP16_DEBUG") != nullptr;   // the arrangement scores of each character in the log (tests)

    // analysis thread
    Cp16Telemetry tel;
    double curRate = 0;
    bool running = false;
    marine::Front front;
    Decim dec;
    std::vector<cf32> base;
    std::vector<cf32> hist;          // 8 kHz, hist[0] is sample histStart
    int64_t histStart = 0, nextEst = 0;
    double power = 0; int64_t nPower = 0;
    Fft fft{kN};
    std::vector<float> win;
    // what was measured
    bool combOk = false, timeOk = false;
    int lostCount = 0;
    double combF = 0, combD = 15;    // comb: positions combF + j * combD, j = 0 .. 15 (baseband Hz)
    bool lsb = false;
    double period = 0;               // samples a row
    double rowAt = -1;               // sample index where the next row starts
    uint64_t rowSerial = 0;
    std::deque<Row> rows;            // for the reading and the levels
    // levels
    std::deque<float> levels;
    float n0 = 0, a1 = 1;
    // reading
    uint64_t readPtr = 0, lastEnd = 0;
    bool anyChar = false;
    int arr = 0;                     // index into kArrs
    bool arrFixed = false;
    double arrScore[kNumArr] = {};
    int arrVotes = 0, flips = 0;
    // where this lock's reading began, to start over when the arrangement changes
    uint64_t lockRow = 0, lockOk = 0, lockBad = 0;
    size_t lockTextLen = 0, lockChars = 0;
    bool lockAny = false;
    uint64_t charSerial = 0;

    Impl() {
        win.resize(kN);
        for (int i = 0; i < kN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN));
        dec.setup();
    }
    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void resetState() {
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; }
        running = curRate >= cp16Tuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        dec.setup();
        base.clear(); hist.clear();
        histStart = 0; nextEst = (int64_t)(kFirstSec * kFs);
        power = 0; nPower = 0;
        combOk = timeOk = false; lostCount = 0;
        period = 0; rowAt = -1;
        rows.clear(); levels.clear(); n0 = 0; a1 = 1;
        readPtr = rowSerial; lastEnd = rowSerial;
        arrFixed = false; arr = 0; arrVotes = 0; flips = 0;
        for (double& s : arrScore) s = 0;
        markLock();
        tel.state = 0;
        tel.inputRate = curRate;
        tel.rateOk = running;
    }

    void markLock() { lockRow = rowSerial; lockOk = tel.blocksOk; lockBad = tel.blocksBad; lockTextLen = tel.text.size(); lockChars = tel.chars.size(); lockAny = anyChar; }

    int64_t histEnd() const { return histStart + (int64_t)hist.size(); }
    const cf32* at(int64_t i) const { return &hist[(size_t)(i - histStart)]; }

    // ------------------------------------------------------------ carriers
    // log10 of the averaged spectrum over its median, 0 .. 7, at baseband frequency f (linear between bins)
    std::vector<float> A;
    float aAt(double f) const {
        const double x = f / kBin + kN / 2;
        const int i = (int)std::floor(x);
        if (i < 0 || i + 1 >= kN) return 0;
        const float u = (float)(x - i);
        return A[(size_t)i] * (1 - u) + A[(size_t)i + 1] * u;
    }
    float combScore(double F, double D) const {
        float on = 0, gap = 0;
        for (int j = 0; j < cp16::kCarriers; j++) on += aAt(F + j * D);
        for (int j = 0; j < cp16::kCarriers - 1; j++) gap += aAt(F + (j + 0.5) * D);
        return (on - gap * 16.f / 15.f) / 16.f;
    }
    bool findComb(int64_t w0, int64_t w1) {
        std::vector<double> P(kN, 0.0);
        std::vector<cf32> b(kN);
        int frames = 0;
        for (int64_t s = w0; s + kN <= w1; s += kN / 2) {
            const cf32* x = at(s);
            for (int i = 0; i < kN; i++) b[(size_t)i] = x[i] * win[(size_t)i];
            fft.forward(b.data());
            for (int i = 0; i < kN; i++) P[(size_t)((i + kN / 2) % kN)] += std::norm(b[(size_t)i]);
            frames++;
        }
        if (frames < 4) return false;
        std::vector<double> srt(P.begin() + kN / 16, P.end() - kN / 16);
        std::nth_element(srt.begin(), srt.begin() + (long)srt.size() / 2, srt.end());
        const double topP = *std::max_element(P.begin(), P.end());
        const double floorP = std::max({1e-30, srt[srt.size() / 2], topP * 1e-6});   // 60 dB at most: a clean signal must not saturate lines and gaps alike
        A.assign(kN, 0.f);
        for (int i = 0; i < kN; i++) A[(size_t)i] = (float)std::min(7.0, std::max(0.0, std::log10(P[(size_t)i] / floorP)));
        double sp0 = 5, sp1 = 30;
        double manB = 0, manS = 0; int side = 0;
        { std::lock_guard<std::mutex> lk(mu); manB = manBase; manS = manSpacing; side = manSide; }
        if (manS > 0) { sp0 = manS - 0.3; sp1 = manS + 0.3; }
        const bool near0 = combOk;   // following a found comb: only around it (less than half a spacing), so its numbering stays; lost, the whole band again
        if (near0) { sp0 = std::max(sp0, combD - 0.3); sp1 = std::min(sp1, combD + 0.3); }
        // where to search: a manual base frequency, or the strongest 300 Hz of the band
        std::vector<std::pair<double, double>> ranges;
        if (near0) ranges.push_back({combF - 0.4 * combD, combF + 0.4 * combD});
        else if (manB > 0) {
            const double spanMax = 15 * sp1;
            // the given frequency itself (a few Hz of tuning error): a comb further off could be another tone of the signal, a pilot or a marker
            (void)spanMax;
            if (side != 2) ranges.push_back({manB - 4, manB + 4});
            if (side != 1) ranges.push_back({-manB - 15 * sp1 - 4, -manB - 15 * sp0 + 4});
        } else {
            const int wbox = (int)(300 / kBin);
            double best = -1; int bi = 0;
            double acc = 0;
            for (int i = 0; i < kN; i++) {
                acc += A[(size_t)i];
                if (i >= wbox) acc -= A[(size_t)(i - wbox)];
                const double f = (i - wbox / 2 - kN / 2) * kBin;
                if (side == 1 && f < 0) continue;
                if (side == 2 && f > 0) continue;
                if (i >= wbox && acc > best) { best = acc; bi = i - wbox / 2; }
            }
            const double c = (bi - kN / 2) * kBin;
            ranges.push_back({c - 15 * sp1 / 2 - 260, c - 15 * sp0 / 2 + 260});
        }
        float bestS = -1e9f; double bF = 0, bD = 15;
        for (const auto& rg : ranges)
            for (double D = sp0; D <= sp1 + 1e-9; D += 0.2)
                for (double F = rg.first; F <= rg.second; F += 0.5) {
                    if (F < -3950 || F + 15 * D > 3950) continue;
                    const float s = combScore(F, D);
                    if (s > bestS) { bestS = s; bF = F; bD = D; }
                }
        // refine
        double cF = bF, cD = bD;
        for (double D = bD - 0.2; D <= bD + 0.2 + 1e-9; D += 0.01)
            for (double F = bF - 0.6; F <= bF + 0.6 + 1e-9; F += 0.05) {
                const float s = combScore(F, D);
                if (s > bestS) { bestS = s; cF = F; cD = D; }
            }
        tel.combScore = 10 * bestS;
        if (bestS < kCombMin) return false;
        if (combOk) {   // the same carriers: keep the numbering (the outer columns come and go with the text), so the rows already read stay valid
            const double k = std::round((cF - combF) / cD);
            if (std::fabs(k) <= kMaxShift) cF -= k * cD;
        }
        combF = cF; combD = cD;
        lsb = side == 2 ? true : side == 1 ? false : combF + 7.5 * combD < 0;
        return true;
    }

    // ------------------------------------------------------------ rows
    // The rows: each carrier's envelope through a box filter 1 / spacing long (zeros on the neighbours, so no beats), the squared slopes of all 16
    // summed: a pulse train at the row edges. The strongest lines in 2 .. 40 Hz and a half and a third of them are the candidates for the row rate; each
    // is tried as rows (rowFisher) and the longest row that explains the envelopes nearly as well as the best is taken. The phase of its line gives the edges.
    bool findTiming(int64_t w0, int64_t w1, double& P, double& b0, float& score) {
        const int L = std::max(8, (int)std::lround(kFs / combD));
        const int64_t n = w1 - w0;
        constexpr int kBinSteps = 4;                    // slopes summed per 2 ms bin
        constexpr int kBinS = kStep * kBinSteps;        // samples a bin
        constexpr double kBinRate = kFs / kBinS;        // 500 Hz
        const int64_t m = n / kBinS;
        if (m < 512) return false;
        std::vector<float> g((size_t)m, 0.f);
        std::vector<cf32> mixed((size_t)n);
        const int64_t ks = n / kStep;
        std::vector<float> e((size_t)ks);
        for (int j = 0; j < cp16::kCarriers; j++) {
            const double f = combF + j * combD;
            const cf32 st = std::polar(1.f, (float)(-2 * kPi * f / kFs));
            cf32 ph = std::polar(1.f, (float)std::fmod(-2 * kPi * f * (double)w0 / kFs, 2 * kPi));
            const cf32* x = at(w0);
            for (int64_t i = 0; i < n; i++) { mixed[(size_t)i] = x[i] * ph; ph *= st; if ((i & 1023) == 1023) ph /= std::abs(ph); }
            cf32 acc(0, 0);
            for (int64_t i = 0; i < n; i++) {
                acc += mixed[(size_t)i];
                if (i >= L) acc -= mixed[(size_t)(i - L)];
                if (i % kStep == kStep - 1) e[(size_t)(i / kStep)] = std::abs(acc) / (float)L;
            }
            for (int64_t k = kBinSteps; k < ks; k++) {
                const int64_t b = k / kBinSteps;
                if (b >= m) break;
                const float dlt = e[(size_t)k] - e[(size_t)(k - kBinSteps)];
                g[(size_t)b] += dlt * dlt;
            }
        }
        double mean = 0;
        for (float v : g) mean += v;
        mean /= (double)m;
        if (mean <= 0) return false;
        constexpr int kM = 1 << 16;
        Fft big(kM);
        std::vector<cf32> G(kM, cf32(0, 0));
        for (int64_t b = 0; b < m && b < kM; b++) G[(size_t)b] = cf32((float)(g[(size_t)b] - mean), 0.f);
        big.forward(G.data());
        const double res = kBinRate / kM;
        double manC = 0;
        { std::lock_guard<std::mutex> lk(mu); manC = manChar; }
        double u0 = 1.0 / cp16::kMaxRowSec, u1 = 1.0 / cp16::kMinRowSec;
        if (manC > 0) { const double u = cp16::kCellRows / manC; u0 = u * 0.98; u1 = u * 1.02; }
        else if (timeOk && period > 0) { const double u = kFs / period; u0 = std::max(u0, u * 0.97); u1 = std::min(u1, u * 1.03); }
        const int i0 = std::max(2, (int)(u0 / res)), i1 = std::min(kM / 2 - 2, (int)(u1 / res) + 1);
        if (i1 - i0 < 4) return false;
        // the level of the slope spectrum: its median over the whole range of row rates, also when only part of it is searched
        const int w0i = std::max(2, (int)(1.0 / cp16::kMaxRowSec / res)), w1i = std::min(kM / 2 - 2, (int)(1.0 / cp16::kMinRowSec / res) + 1);
        std::vector<float> mag((size_t)(std::max(i1, w1i) + 2), 0.f);
        for (int i = 1; i <= std::max(i1, w1i) + 1; i++) mag[(size_t)i] = std::abs(G[(size_t)i]);
        std::vector<float> band(mag.begin() + w0i, mag.begin() + w1i);
        std::nth_element(band.begin(), band.begin() + (long)band.size() / 2, band.end());
        const float med = std::max(1e-20f, band[band.size() / 2]);
        // candidate lines: the strongest peaks and what lies a half and a third of them
        std::vector<int> peaks;
        for (int i = i0; i < i1; i++) if (mag[(size_t)i] >= mag[(size_t)i - 1] && mag[(size_t)i] > mag[(size_t)i + 1] && mag[(size_t)i] >= 4 * med) peaks.push_back(i);
        if (peaks.empty()) return false;
        std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return mag[(size_t)a] > mag[(size_t)b]; });
        if (peaks.size() > 12) peaks.resize(12);
        std::vector<int> cands(peaks);
        for (int p : peaks)
            for (int div : {2, 3}) {
                const int c = (int)std::lround((double)p / div);
                if (c < i0) continue;
                int q = c;
                for (int i = std::max(i0, c - 4); i <= std::min(i1 - 1, c + 4); i++) if (mag[(size_t)i] > mag[(size_t)q]) q = i;
                cands.push_back(q);
            }
        std::sort(cands.begin(), cands.end());
        cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
        // each candidate folded: how much of the slope energy falls into one filter length of the row (1 at the row rate and its multiples, little elsewhere);
        // the longest row that concentrates it nearly as well as the best is the row
        struct Cn { double u, edge, conc; float ratio; };
        std::vector<Cn> tried;
        for (int c : cands) {
            double u = c * res;
            const double a = mag[(size_t)c - 1], b = mag[(size_t)c], cc = mag[(size_t)c + 1];
            const double den = a - 2 * b + cc;
            if (den < 0) u += std::clamp(0.5 * (a - cc) / den, -0.5, 0.5) * res;
            if (u <= 0) continue;
            double edge = 0;
            const double conc = fold(g, mean, kFs / u, L, kBinS, edge);
            if (conc > -1) tried.push_back({u, edge, conc, (float)(mag[(size_t)c] / med)});
        }
        double cmax = 0;
        for (const Cn& c : tried) cmax = std::max(cmax, c.conc);
        if (cmax <= 0) return false;
        const Cn* pick = nullptr;
        for (const Cn& c : tried) if (c.conc >= 0.85 * cmax && (!pick || c.u < pick->u)) pick = &c;
        score = (float)pick->conc;
        if (pick->ratio < kTimeRatio || pick->conc < kConcMin) return false;
        P = kFs / pick->u;
        b0 = (double)w0 + pick->edge;
        return true;
    }

    // The slopes folded at period P (samples): the share of their energy in the best window one filter long, against what a flat fold would give (0 flat, 1 all
    // in the window); edge: the first row edge, samples from the start. -2 when the row is shorter than the window.
    static double fold(const std::vector<float>& g, double mean, double P, int L, int binS, double& edge) {
        if (!(P > 2.0 * binS) || !std::isfinite(P)) return -2;
        const double W = std::min((double)L + 2 * binS, 0.6 * P);
        const int nb = std::clamp((int)(P / binS), 16, 256);
        std::vector<double> h((size_t)nb, 0.0);
        double total = 0;
        for (size_t b = 0; b < g.size(); b++) {
            const double t = ((double)b + 0.5) * binS;
            const double v = std::max(0.0, (double)g[b] - 0.5 * mean);
            int k = (int)(std::fmod(t, P) / P * nb);
            k = std::clamp(k, 0, nb - 1);
            h[(size_t)k] += v;
            total += v;
        }
        if (total <= 0) return -2;
        const int wb = std::max(1, (int)std::lround(W / P * nb));
        if (wb >= nb) return -2;
        double acc = 0;
        for (int k = 0; k < wb; k++) acc += h[(size_t)k];
        double bestAcc = acc; int bestK = 0;
        for (int k = 1; k < nb; k++) {
            acc += h[(size_t)((k + wb - 1) % nb)] - h[(size_t)(k - 1)];
            if (acc > bestAcc) { bestAcc = acc; bestK = k; }
        }
        // the centroid of the window (circular)
        double re = 0, im = 0;
        for (int k = bestK; k < bestK + wb; k++) {
            const double ph = 2 * kPi * ((double)k + 0.5) / nb;
            re += h[(size_t)(k % nb)] * std::cos(ph);
            im += h[(size_t)(k % nb)] * std::sin(ph);
        }
        double phi = std::atan2(im, re);
        if (phi < 0) phi += 2 * kPi;
        edge = phi / (2 * kPi) * P - (L - 1) / 2.0 - binS / 2.0;
        const double uni = (double)wb / nb;
        return (bestAcc / total - uni) / (1 - uni);
    }

    // amplitudes of the comb positions over one row
    void readRow(double s, Row& r) {
        const int64_t i0 = (int64_t)std::ceil(s), i1 = (int64_t)std::floor(s + period);
        const int L = std::max(8, (int)std::lround(kFs / combD));
        const bool box = (i1 - i0) >= L + 2 * kStep;
        for (int p = 0; p < kPos; p++) {
            const double f = combF + (p - kMaxShift) * combD;
            const cf32 st = std::polar(1.f, (float)(-2 * kPi * f / kFs));
            cf32 ph = std::polar(1.f, (float)std::fmod(-2 * kPi * f * (double)i0 / kFs, 2 * kPi));
            const cf32* x = at(i0);
            const int64_t n = i1 - i0;
            float sum = 0; int cnt = 0;
            if (box) {
                std::vector<cf32> mixed((size_t)n);
                for (int64_t i = 0; i < n; i++) { mixed[(size_t)i] = x[i] * ph; ph *= st; }
                cf32 acc(0, 0);
                for (int64_t i = 0; i < n; i++) {
                    acc += mixed[(size_t)i];
                    if (i >= L) acc -= mixed[(size_t)(i - L)];
                    if (i >= L - 1 && (i - L + 1) % kStep == 0) { sum += std::abs(acc) / (float)L; cnt++; }
                }
            } else {
                const int64_t a = n / 10, b = n - n / 10;
                cf32 acc(0, 0);
                for (int64_t i = 0; i < n; i++) { if (i >= a && i < b) acc += x[i] * ph; ph *= st; }
                sum = std::abs(acc) / (float)std::max<int64_t>(1, b - a); cnt = 1;
            }
            r.v[p] = cnt ? sum / (float)cnt : 0.f;
        }
    }

    void updateLevels(const Row& r) {
        for (int p = kMaxShift; p < kMaxShift + cp16::kCarriers; p++) levels.push_back(r.v[p]);
        while (levels.size() > 16 * 240) levels.pop_front();
        if (levels.size() < 16 * 8) return;
        // two classes (blank and inked dots): the split with the largest between-class variance
        std::vector<float> v(levels.begin(), levels.end());
        std::sort(v.begin(), v.end());
        const size_t N = v.size();
        std::vector<double> cs(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) cs[i + 1] = cs[i] + v[i];
        double best = -1; size_t bk = N / 2;
        for (size_t k = N / 20; k < N - N / 50; k += std::max<size_t>(1, N / 400)) {
            const double m0 = cs[k] / (double)k, m1 = (cs[N] - cs[k]) / (double)(N - k);
            const double w0 = (double)k / N;
            const double bv = w0 * (1 - w0) * (m1 - m0) * (m1 - m0);
            if (bv > best) { best = bv; bk = k; }
        }
        n0 = (float)(cs[bk] / (double)bk);
        a1 = (float)((cs[N] - cs[bk]) / (double)(N - bk));
        if (a1 <= n0 * 1.05f) a1 = n0 * 1.05f + 1e-12f;
        tel.snrDb = 20.f * std::log10(std::max(1e-9f, a1) / std::max(1e-9f, n0));
    }

    void makeDots(Row& r) const {
        for (int p = 0; p < kPos; p++) r.d[p] = std::clamp((r.v[p] - n0) / (a1 - n0), 0.f, 1.f);
    }

    // the column c of arrangement a in a row, as a dot
    float dotOf(const Row& r, int a, int c) const {
        const int j = lsb ? cp16::kCarriers - 1 - c : c;   // comb position of the column
        return r.d[std::clamp(j + kArrs[a] + kMaxShift, 0, kPos - 1)];
    }
    bool inked(const Row& r) const {
        if (arrFixed) { for (int c = 0; c < cp16::kCarriers; c++) if (dotOf(r, arr, c) >= 0.5f) return true; return false; }
        for (int p = 0; p < kPos; p++) if (r.d[p] >= 0.5f) return true;
        return false;
    }
    const Row* rowBySerial(uint64_t s) const {
        if (rows.empty() || s < rows.front().serial || s > rows.back().serial) return nullptr;
        return &rows[(size_t)(s - rows.front().serial)];
    }

    void publishPictureRow(const Row& r) {
        std::array<uint8_t, 16> px{};
        const int a = arr;
        for (int c = 0; c < cp16::kCarriers; c++) px[(size_t)c] = (uint8_t)std::lround(255.f * dotOf(r, a, c));
        if (tel.rows.empty()) tel.firstRow = r.serial;
        tel.rows.push_back(px);
        while (tel.rows.size() > kCp16PictureRows) { tel.rows.erase(tel.rows.begin()); tel.firstRow++; }
        tel.rowCount = r.serial + 1;
    }

    // ------------------------------------------------------------ characters
    struct Try { uint64_t start = 0; cp16::Match m; };
    Try bestStart(uint64_t from, uint64_t r, int a) const {
        Try best;
        best.m.score = -1;
        float dots[cp16::kRows][cp16::kCarriers];
        const uint64_t s0 = r >= from + 15 ? r - 15 : from;
        for (uint64_t s = s0; s <= r; s++) {
            for (int k = 0; k < cp16::kRows; k++) {
                const Row* rr = rowBySerial(s + (uint64_t)k);
                for (int c = 0; c < cp16::kCarriers; c++) dots[k][c] = rr ? dotOf(*rr, a, c) : 0.f;
            }
            const cp16::Match m = cp16::recognise(dots);
            if (m.score > best.m.score) { best.m = m; best.start = s; }
        }
        return best;
    }

    // which arrangements hold (nearly) all the ink read so far in their 16 columns, with its centre near theirs (the glyphs of a font sit in the middle of the cell)
    void arrangementsThatFit(bool valid[kNumArr]) const {
        double ink[kPos] = {}, total = 0, mom = 0;
        for (const Row& r : rows) for (int p = 0; p < kPos; p++) if (r.d[p] >= 0.5f) { ink[p] += 1; total += 1; mom += p; }
        const double centre = total > 0 ? mom / total : 0;
        int nearest = 0;
        bool any = false;
        for (int a = 0; a < kNumArr; a++) {
            double out = 0;
            for (int p = 0; p < kPos; p++) { const int j = p - kMaxShift - kArrs[a]; if (j < 0 || j >= cp16::kCarriers) out += ink[p]; }
            const double wc = kMaxShift + kArrs[a] + 7.5;
            if (std::fabs(wc - centre) < std::fabs(kMaxShift + kArrs[nearest] + 7.5 - centre)) nearest = a;
            valid[a] = total < 50 || (out <= 0.03 * total && std::fabs(wc - centre) <= 1.6);
            any = any || valid[a];
        }
        if (!any) valid[nearest] = true;
    }

    void readCharacters() {
        bool on;
        { std::lock_guard<std::mutex> lk(mu); on = readOn; }
        if (!on || rows.empty()) return;
        if (readPtr < rows.front().serial) readPtr = rows.front().serial;
        while (true) {
            uint64_t r = readPtr;
            const uint64_t last = rows.back().serial;
            while (r <= last && !inked(*rowBySerial(r))) r++;
            if (r > last) { readPtr = r; return; }
            if (r + 15 > last) { readPtr = r >= 15 && r - 15 > readPtr ? r - 15 : readPtr; return; }
            Try t;
            if (!arrFixed) {
                // the arrangements that hold all the ink so far in their 16 columns, each scored on this character; the best total wins, and reading starts over when it changes
                bool valid[kNumArr];
                arrangementsThatFit(valid);
                Try all[kNumArr];
                int bi = -1;
                for (int a = 0; a < kNumArr; a++) {
                    if (!valid[a]) continue;
                    all[a] = bestStart(readPtr, r, a);
                    if (bi < 0 || all[a].m.score > all[bi].m.score) bi = a;
                }
                if (bi < 0) { bi = 0; all[0] = bestStart(readPtr, r, 0); }
                if (dbg) {
                    std::string l = "CP16 debug: row " + std::to_string(r) + " arr " + std::to_string(kArrs[arr]) + ":";
                    for (int a = 0; a < kNumArr; a++) { char b[48]; snprintf(b, sizeof b, " %+d%s%.2f", kArrs[a], valid[a] ? "=" : "x", valid[a] ? all[a].m.score : 0.f); l += b; }
                    say(l);
                }
                if (all[bi].m.score >= kShow) {
                    for (int a = 0; a < kNumArr; a++) if (valid[a]) arrScore[a] += std::max(0.f, all[a].m.score);
                    arrVotes++;
                    int b = bi;
                    for (int a = 0; a < kNumArr; a++) if (valid[a] && arrScore[a] > arrScore[b]) b = a;
                    if (b != arr && arrVotes >= 2 && flips < 8) {
                        arr = b; flips++;
                        tel.text.resize(std::min(tel.text.size(), lockTextLen));
                        while (tel.chars.size() > lockChars) tel.chars.pop_back();
                        tel.blocksOk = lockOk; tel.blocksBad = lockBad;
                        readPtr = std::max(lockRow, rows.front().serial); lastEnd = readPtr; anyChar = lockAny;
                        continue;
                    }
                    // settled: a clear leader (clean characters fit one arrangement exactly), or many characters
                    double first = 0, second = 0;
                    for (int q = 0; q < kNumArr; q++) { if (arrScore[q] > first) { second = first; first = arrScore[q]; } else if (arrScore[q] > second) second = arrScore[q]; }
                    if ((arrVotes >= 4 && arrScore[arr] >= 0.9 * arrVotes && arrScore[arr] >= first) ||
                        (arrVotes >= 8 && arrScore[arr] >= first && first >= 1.15 * second) || arrVotes >= 40) arrFixed = true;
                }
                t = valid[arr] ? all[arr] : all[bi];
            } else t = bestStart(readPtr, r, arr);
            if (t.m.score < 0.25f) { readPtr = r + 1; continue; }   // a stray dot
            Cp16Char ch;
            ch.serial = charSerial++;
            const Row* rr = rowBySerial(t.start);
            ch.timeSec = rr ? rr->t : 0;
            ch.cp = t.m.cp;
            ch.score = t.m.score;
            ch.margin = t.m.score - t.m.second;
            ch.sure = t.m.score >= kSure && ch.margin >= kSureMargin;
            uint32_t shown = t.m.score >= kShow ? t.m.cp : '?';
            if (shown >= 0xFF01 && shown <= 0xFF5E) shown = shown - 0xFF01 + 0x21;   // full-width forms read as ASCII
            ch.text = cp16::codepointToUtf8(shown);
            if (anyChar && t.start >= lastEnd + 14) tel.text += ' ';
            tel.text += ch.text;
            if (tel.text.size() > 8000) tel.text.erase(0, tel.text.size() - 8000);
            if (ch.sure) tel.blocksOk++; else tel.blocksBad++;
            tel.chars.push_back(ch);
            while (tel.chars.size() > kCp16CharsMax) tel.chars.erase(tel.chars.begin());
            anyChar = true;
            lastEnd = t.start + cp16::kRows;
            readPtr = lastEnd;
        }
    }

    // ------------------------------------------------------------ the pipeline
    void estimate() {
        const int64_t w1 = histEnd();
        const int64_t w0 = std::max(histStart, w1 - (int64_t)(kWinSec * kFs));
        const bool c = findComb(w0, w1);
        if (!c) {
            if (combOk && ++lostCount >= 4) { combOk = timeOk = false; tel.state = 0; say("CP16: carriers lost"); }
            if (!combOk) return;
        } else {
            if (!combOk) {
                char b[160];
                snprintf(b, sizeof b, "CP16: 16 carriers from %.1f Hz, %.2f Hz apart (%s sideband)", lsb ? -(combF + 15 * combD) : combF, combD, lsb ? "lower" : "upper");
                say(b);
            }
            combOk = true; lostCount = 0;
        }
        tel.state = std::max(tel.state, 1);
        tel.spacingHz = combD;
        tel.baseHz = lsb ? -(combF + (15 + kArrs[arr]) * combD) : combF + kArrs[arr] * combD;
        tel.mirrored = lsb;
        double P = 0, b0 = 0; float coh = 0;
        const bool t = findTiming(w0, w1, P, b0, coh);
        tel.timingScore = coh;
        if (!t) return;
        if (!timeOk) {
            period = P;
            markLock();
            { double mb; { std::lock_guard<std::mutex> lk(mu); mb = manBase; } if (mb > 0) { arr = 0; arrFixed = true; } else { arrFixed = false; arr = 0; arrVotes = 0; flips = 0; for (double& q : arrScore) q = 0; } }
            rowAt = b0;   // the first row edge of the window: the picture starts with what is already there
            while (rowAt - P >= (double)w0) rowAt -= P;
            char b[120];
            snprintf(b, sizeof b, "CP16: rows of %.1f ms (%.2f s a character)", 1000 * P / kFs, cp16::kCellRows * P / kFs);
            say(b);
        } else {
            period = 0.7 * period + 0.3 * P;
            const double k = std::round((rowAt - b0) / P);
            const double diff = b0 + k * P - rowAt;
            if (std::fabs(diff) < 0.35 * P) rowAt += 0.5 * diff;
            else rowAt += diff;   // a jump of the sender's clock: follow it
        }
        timeOk = true;
        tel.state = 2;
        tel.rowSec = period / kFs;
        tel.charSec = cp16::kCellRows * tel.rowSec;
    }

    void extract() {
        if (!timeOk || period <= 0) return;
        const uint64_t newFrom = rowSerial;
        while (rowAt + period + 1 < (double)histEnd()) {
            if (rowAt < (double)histStart) { rowAt += period; continue; }
            Row r;
            r.serial = rowSerial++;
            r.t = rowAt / kFs;
            readRow(rowAt, r);
            updateLevels(r);
            rows.push_back(r);
            while (rows.size() > 2000) rows.pop_front();
            rowAt += period;
        }
        // dots with the levels of now: the rows the reader may still use and the new ones (the first rows came before the levels were known)
        const uint64_t redo = std::min(newFrom, readPtr);
        for (Row& r : rows) if (r.serial >= redo) makeDots(r);
        for (const Row& r : rows) if (r.serial >= newFrom) publishPictureRow(r);
        readCharacters();
    }

    void process(const cf32* x, size_t n) {
        base.clear();
        front.process(x, n, base);
        for (const cf32& v : base) {
            cf32 o;
            if (!dec.push(v, o)) continue;
            hist.push_back(o);
            power += std::norm(o); nPower++;
        }
        const int64_t keep = (int64_t)(kHistSec * kFs);
        if ((int64_t)hist.size() > keep + (int64_t)kFs * 4) {
            const int64_t drop = (int64_t)hist.size() - keep;
            hist.erase(hist.begin(), hist.begin() + (long)drop);
            histStart += drop;
        }
        while (histEnd() >= nextEst) {
            estimate();
            extract();
            nextEst += (int64_t)(kEverySec * kFs);
            if (nPower) { tel.levelDb = (float)(10 * std::log10(power / (double)nPower + 1e-20)); power = 0; nPower = 0; }
            tel.nowSec = (double)histEnd() / kFs;
            tel.fontName = cp16::fontName();
            tel.dataValid = tel.state == 2;
            tel.seq++;
            std::lock_guard<std::mutex> lk(mu);
            const uint64_t s = pub.seq;
            pub = tel;
            pub.seq = s + 1;
        }
    }
};

Cp16Receiver::Cp16Receiver() : p_(std::make_unique<Impl>()) {}
Cp16Receiver::~Cp16Receiver() = default;

void Cp16Receiver::configure(double r) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->pub.inputRate = r; p_->pub.rateOk = r >= cp16Tuning().minSampleRate - 1; }
    p_->resetReq = true;
}
void Cp16Receiver::setSignalOffset(double hz) { { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; } p_->offsetReq = true; }
bool Cp16Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= cp16Tuning().minSampleRate - 1; }
void Cp16Receiver::reset() { p_->resetReq = true; }
void Cp16Receiver::clearResults() { p_->clearReq = true; }
void Cp16Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void Cp16Receiver::setManual(double baseHz, double spacingHz, double charSec, int sideband) {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->manBase = baseHz; p_->manSpacing = spacingHz; p_->manChar = charSec; p_->manSide = sideband; }
    p_->resetReq = true;
}
void Cp16Receiver::setRead(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->readOn = on; }

void Cp16Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false)) { double o; { std::lock_guard<std::mutex> lk(m.mu); o = m.offsetHz; } m.front.setOffsetHz(o); }
    if (m.clearReq.exchange(false)) {
        m.tel.text.clear(); m.tel.chars.clear(); m.tel.rows.clear(); m.tel.blocksOk = m.tel.blocksBad = 0;
        m.tel.firstRow = m.rowSerial; m.anyChar = false;
    }
    if (m.loadReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.tel.text = m.loadText; }
    if (!m.running) return;
    m.process(x, n);
}

bool Cp16Receiver::telemetry(Cp16Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

std::string Cp16Receiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->pub.text;
}
bool Cp16Receiver::loadResults(const std::string& blob) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->loadText = blob;
    p_->pub.text = blob;
    p_->loadReq = true;
    return true;
}

ModeTuning cp16Tuning() {
    ModeTuning t;
    t.stdMode = 882;
    t.id = "cp16";
    t.name = "CP16";
    t.minMhz = 1.8;
    t.maxMhz = 30;
    t.defMhz = 7.050;                  // a 40 m dial frequency; CRAC's exercises announce their own (either sideband is read)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;            // as the other HF digital modes: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

} // namespace dect2
