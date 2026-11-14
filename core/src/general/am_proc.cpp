// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM audio processor, see am_proc.h.
#include "dect2/general/am_proc.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kAnaN = 4096;          // spectrum analysis of the derotated signal: 11.7 Hz bins at 48 kHz, one frame every 85 ms
constexpr int kHil = 255;            // Hilbert filter for the sideband split (flat above about 250 Hz)
constexpr int kLpTaps = 191;         // audio low-pass bank
constexpr int kBwMin = 2500, kBwMax = 10000, kBwStep = 500, kBwN = (kBwMax - kBwMin) / kBwStep + 1;
constexpr int kNrN = 1024, kNrHop = 256;   // 75% overlap: gain changes are spread over four frames, no block edges
constexpr int kNrSub = 12 * 512 / kNrHop;  // frames per minimum-statistics sub-window (the same 128 ms)
constexpr int kMaxNotch = 6;
constexpr int kLook = 144;           // limiter look-ahead, 3 ms

double blackman(int k, int n) { return 0.42 - 0.5 * std::cos(2 * kPi * k / (n - 1)) + 0.08 * std::cos(4 * kPi * k / (n - 1)); }

// exponential integral E1 (Abramowitz and Stegun 5.1.53 / 5.1.56)
double expint1(double v) {
    v = std::max(v, 1e-10);
    if (v <= 1) return -std::log(v) - 0.57721566 + v * (0.99999193 + v * (-0.24991055 + v * (0.05519968 + v * (-0.00976004 + v * 0.00107857))));
    return (v * v + 2.334733 * v + 0.250621) / (v * v + 3.330657 * v + 1.681534) * std::exp(-v) / v;
}

struct Biq {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void notch(double f, double q, double fs) {
        const double w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), a0 = 1 + al, c = std::cos(w);
        b0 = 1 / a0; b1 = -2 * c / a0; b2 = 1 / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    void highShelf(double f, double gainDb, double fs) {   // RBJ, S = 1
        const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w), al = s / 2 * std::sqrt(2.0);
        const double sa = 2 * std::sqrt(A) * al, a0 = (A + 1) - (A - 1) * c + sa;
        b0 = A * ((A + 1) + (A - 1) * c + sa) / a0; b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0; b2 = A * ((A + 1) + (A - 1) * c - sa) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * c) / a0; a2 = ((A + 1) - (A - 1) * c - sa) / a0;
    }
    void bandpass(double f, double q, double fs) {
        const double w = 2 * kPi * f / fs, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b1 = 0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0;
    }
    void highpass(double f, double q, double fs) {
        const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void clear() { z1 = z2 = 0; }
};

// a real FIR with a doubled history
struct Fir {
    std::vector<float> h, hist;
    int pos = 0;
    void set(const std::vector<float>& t) { h = t; hist.assign(2 * h.size(), 0.f); pos = 0; }
    float run(float x) {
        const int n = (int)h.size();
        pos = (pos + n - 1) % n;
        hist[(size_t)pos] = hist[(size_t)(pos + n)] = x;
        const float* w = &hist[(size_t)pos];
        float a = 0;
        for (int k = 0; k < n; k++) a += h[(size_t)k] * w[k];
        return a;
    }
    void clear() { std::fill(hist.begin(), hist.end(), 0.f); pos = 0; }
};

std::vector<float> lowpassTaps(double fc, double fs, int n) {
    std::vector<float> h((size_t)n);
    double s = 0;
    for (int k = 0; k < n; k++) {
        const double x = k - (n - 1) / 2.0, f = fc / fs;
        const double v = x == 0 ? 2 * f : std::sin(2 * kPi * f * x) / (kPi * x);
        h[(size_t)k] = (float)(v * blackman(k, n)); s += h[(size_t)k];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

} // namespace

struct AmProcessor::Impl {
    double fs = 48000;
    AmProcConfig cfg;
    AmProcStatus st;

    // noise blanker
    double nbMean = -1;
    int nbHold = 0;
    long nbRun = -1;
    cf32 nbG1{0, 0}, nbG2{0, 0}, nbFill{0, 0};
    double nbThr = 0;
    uint64_t nbCount = 0, nbBlanked = 0;

    // carrier acquisition and PLL
    std::unique_ptr<Fft> acqFft;
    std::vector<cf32> acqBuf;
    std::vector<double> acqPow;
    int acqFrames = 0;
    bool acquired = false;
    double ph = 0, fr = 0;                // NCO
    cf32 carC{0, 0};                      // the carrier: derotated signal low-passed (about 30 Hz)
    double carMag = 0, carLong = 0;       // |r| low-passed alike; the carrier level over a second
    double tLock = 0, unlockFor = 0, sinceAcq = 0, lockQ = 0;
    Fir envR, envI;                       // the channel filter of the envelope path (the bank's taps on I and Q)
    int envIdx = -1;
    int prevDet = 0, detFade = 0;         // a change of detector is crossfaded over 20 ms
    bool locked = false, fade = false;
    double envMean = -1;
    double dcS = 0;                       // the carrier (mean) of the detected audio
    // Hilbert split
    std::vector<float> hil;
    std::vector<cf32> hHist;
    int hPos = 0;
    // analysis
    std::unique_ptr<Fft> anaFft;
    std::vector<cf32> anaBuf;
    std::vector<float> anaWin;
    std::vector<double> pU, pL;           // averaged power of the upper and lower sideband per bin (bin k = k * fs / kAnaN Hz)
    std::vector<double> qU, qL;           // the same over about 4 s: selective fading moves from side to side and averages out, interference stays
    int anaFrames = 0;
    double floorP = 0;
    struct Cand { float hz; int seen; int miss; };
    std::vector<Cand> cands;
    // detector choice
    int det = 0, side = 0;                // side: 0 both, 1 upper, 2 lower
    int sideWant = 0, sideCount = 0;
    // audio low-pass bank, crossfaded
    std::vector<std::vector<float>> lpBank;
    Fir lpA, lpB;
    int lpCur = -1, lpNext = -1, fadeLeft = 0;
    int bwWant = 4500;
    // notches
    Biq notch[kMaxNotch];
    float notchF[kMaxNotch] = {};
    int nNotch = 0;
    Biq hum[4];
    int humHz = 0;
    // noise reduction (STFT)
    std::unique_ptr<Fft> nrFft;
    std::vector<float> nrIn, nrOut, nrWin;
    size_t nrInPos = 0;
    std::vector<double> nrGOut, nrSmooth, nrMin, nrMinSub, nrNoise, nrPrevG2Gamma, nrG;
    int nrFrame = 0, nrSub = 0;
    std::vector<std::vector<double>> nrSubMins;
    bool nrPrimed = false;
    double nrFloorDb = 0;
    std::vector<cf32> nrSpec;
    std::vector<float> nrQueue;            // processed audio waiting to be handed out
    size_t nrQHead = 0;
    // leveler, limiter, EQ, extension, DC
    double lvlGain = 1, lvlRms = 0, lvlHang = 0;
    std::vector<float> limBuf;
    int limPos = 0;
    double limGain = 1;
    Biq eqShelf, dcHp, exBp, exHp;
    float eqSet = -1, exSet = -1;
    double exEnv = 0;
    // status
    double pubT = 0;

    void configure(double rate) {
        fs = rate;
        acqFft = std::make_unique<Fft>(kAnaN);
        anaFft = std::make_unique<Fft>(kAnaN);
        nrFft = std::make_unique<Fft>(kNrN);
        anaWin.resize(kAnaN);
        for (int k = 0; k < kAnaN; k++) anaWin[(size_t)k] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (k + 0.5) / kAnaN));
        hil.assign(kHil, 0.f);
        for (int k = 0; k < kHil; k++) { const int n = k - kHil / 2; if (n % 2) hil[(size_t)k] = (float)(2.0 / (kPi * n) * blackman(k, kHil)); }
        lpBank.clear();
        for (int i = 0; i < kBwN; i++) lpBank.push_back(lowpassTaps(kBwMin + i * kBwStep, fs, kLpTaps));
        nrWin.resize(kNrN);
        for (int k = 0; k < kNrN; k++) nrWin[(size_t)k] = (float)std::sqrt(0.5 - 0.5 * std::cos(2 * kPi * (k + 0.5) / kNrN));
        reset();
    }

    void reset() {
        AmProcConfig keep = cfg;
        nbMean = -1; nbHold = 0; nbG1 = nbG2 = nbFill = cf32(0, 0); nbCount = nbBlanked = 0;
        acqBuf.clear(); acqPow.assign(kAnaN, 0.0); acqFrames = 0; acquired = false;
        ph = fr = 0; carC = cf32(0, 0); carMag = carLong = 0; carLongN = 0; tLock = unlockFor = sinceAcq = lockQ = 0; locked = fade = false; envMean = -1; dcS = 0;
        envIdx = -1; prevDet = 0; detFade = 0;
        hHist.assign(2 * kHil, cf32(0, 0)); hPos = 0;
        anaBuf.clear(); pU.assign(kAnaN / 2, 0.0); pL.assign(kAnaN / 2, 0.0); qU.assign(kAnaN / 2, 0.0); qL.assign(kAnaN / 2, 0.0); anaFrames = 0; floorP = 0; cands.clear();
        det = 0; side = 0; sideWant = 0; sideCount = 0;
        lpCur = -1; lpNext = -1; fadeLeft = 0; bwWant = 4500;
        nNotch = 0; humHz = 0;
        for (auto& b : notch) b.clear();
        for (auto& b : hum) b.clear();
        nrIn.assign(kNrN, 0.f); nrOut.assign(kNrN, 0.f); nrInPos = 0;
        nrSmooth.assign(kNrN / 2 + 1, 0.0); nrNoise.assign(kNrN / 2 + 1, 0.0); nrPrevG2Gamma.assign(kNrN / 2 + 1, 1.0); nrG.assign(kNrN / 2 + 1, 1.0); nrGOut.assign(kNrN / 2 + 1, 1.0);
        nrSubMins.assign(8, std::vector<double>(kNrN / 2 + 1, 1e30)); nrMinSub.assign(kNrN / 2 + 1, 1e30);
        nrFrame = 0; nrSub = 0; nrPrimed = false; nrSpec.assign(kNrN, cf32(0, 0));
        nrQueue.clear(); nrQHead = 0;
        lvlGain = 1; lvlRms = 0; lvlHang = 0;
        limBuf.assign(kLook, 0.f); limPos = 0; limGain = 1;
        eqShelf.clear(); dcHp.highpass(40, 0.707, fs); dcHp.clear(); eqSet = -1; exSet = -1; exEnv = 0;
        pubT = 0;
        st = AmProcStatus();
        cfg = keep;
    }

    // ---- noise blanker: on the IF magnitude; the running mean is fed with impulses clipped to the threshold, so it can never lock up
    void blank(cf32* x, size_t n) {
        int thrDb = cfg.nbDb;
        if (thrDb < 0) thrDb = cfg.autoQuality ? 15 : 0;
        if (thrDb == 0) { st.nbThrDb = 0; return; }
        // auto: when more than 5 % of the samples get blanked, the threshold is too low for this signal
        double thr = std::pow(10.0, (thrDb + nbThr) / 10.0);
        const double a = 1.0 / (0.05 * fs);
        nbRun = nbHold > 0 ? 0 : -1;
        for (size_t i = 0; i < n; i++) {
            const double p = std::norm(x[i]);
            if (nbMean < 0) nbMean = p;
            const bool imp = nbMean > 0 && p > thr * nbMean;
            nbMean += (std::min(p, thr * std::max(nbMean, 1e-30)) - nbMean) * a;
            if (nbMean <= 0) nbMean = p;
            if (imp) {
                if (nbHold == 0) {   // the samples before it go too (the channel filter rings ahead of an impulse as well)
                    const size_t back = std::min<size_t>(i, 3);
                    nbFill = i > back ? x[i - back - 1] : nbG1;
                    for (size_t k = i - back; k < i; k++) x[k] = nbFill;
                    nbRun = (long)(i - back);
                }
                nbHold = 4;
            }
            nbCount++;
            if (nbHold > 0) { x[i] = nbFill; nbHold--; nbBlanked++; }
            else {
                // the gap is closed with a straight line from the last good sample to this one
                if (nbRun >= 0) {
                    const long len = (long)i - nbRun;
                    for (long k = 0; k < len; k++) x[(size_t)(nbRun + k)] = nbFill + (x[i] - nbFill) * (float)(k + 1) / (float)(len + 1);
                    nbRun = -1;
                }
                nbG2 = nbG1; nbG1 = x[i];
            }
        }
        if (nbCount >= (uint64_t)fs) {
            const double pct = 100.0 * nbBlanked / nbCount;
            st.blankPct = (float)pct;
            if (cfg.nbDb < 0) { if (pct > 5 && nbThr < 12) nbThr += 3; else if (pct < 0.5 && nbThr > 0) nbThr -= 1; }
            nbCount = nbBlanked = 0;
        }
        st.nbThrDb = (float)(thrDb + nbThr);
    }

    // ---- carrier search: the strongest line within +-3 kHz in an averaged spectrum (4 frames, 0.34 s)
    void acquire(const cf32* x, size_t n) {
        for (size_t i = 0; i < n && !acquired; i++) {
            acqBuf.push_back(x[i]);
            if ((int)acqBuf.size() < kAnaN) continue;
            for (int k = 0; k < kAnaN; k++) acqBuf[(size_t)k] *= anaWin[(size_t)k];
            acqFft->forward(acqBuf.data());
            for (int k = 0; k < kAnaN; k++) acqPow[(size_t)k] += std::norm(acqBuf[(size_t)k]);
            acqBuf.clear();
            if (++acqFrames < 4) continue;
            const double bin = fs / kAnaN;
            const int lim = (int)(3000 / bin);
            int best = 0; double bp = -1;
            std::vector<double> all;
            for (int k = -lim; k <= lim; k++) { const double p = acqPow[(size_t)((k + kAnaN) % kAnaN)]; all.push_back(p); if (p > bp) { bp = p; best = k; } }
            std::nth_element(all.begin(), all.begin() + (long)all.size() / 2, all.end());
            const double med = all[all.size() / 2];
            acqPow.assign(kAnaN, 0.0); acqFrames = 0;
            if (bp > 100 * med) {   // a carrier 20 dB over the median: parabolic interpolation of the peak
                acquired = true;
                st.carrier = true;
                fr = 2 * kPi * best * bin / fs;
                ph = 0; sinceAcq = 0; locked = false; tLock = 0;
            } else st.carrier = false;
        }
    }

    // ---- the analysis of the derotated signal: noise floor, audio edges, interference per sideband, whistles, hum, SNR
    void analyse() {
        for (int k = 0; k < kAnaN; k++) anaBuf[(size_t)k] *= anaWin[(size_t)k];
        anaFft->forward(anaBuf.data());
        const double a = anaFrames < 4 ? 1.0 / (anaFrames + 1) : 0.25;
        for (int k = 1; k < kAnaN / 2; k++) {
            pU[(size_t)k] += a * (std::norm(anaBuf[(size_t)k]) - pU[(size_t)k]);
            pL[(size_t)k] += a * (std::norm(anaBuf[(size_t)(kAnaN - k)]) - pL[(size_t)k]);
            const double b = anaFrames < 48 ? 1.0 / (anaFrames + 1) : 0.02;
            qU[(size_t)k] += b * (std::norm(anaBuf[(size_t)k]) - qU[(size_t)k]);
            qL[(size_t)k] += b * (std::norm(anaBuf[(size_t)(kAnaN - k)]) - qL[(size_t)k]);
        }
        anaFrames++;
        anaBuf.clear();
        if (anaFrames < 3) return;
        const double bin = fs / kAnaN;
        auto kOf = [&](double hz) { return std::max(1, std::min(kAnaN / 2 - 1, (int)std::lround(hz / bin))); };
        // noise floor: the 20th percentile of both sidebands between 300 Hz and 11 kHz
        std::vector<double> v;
        for (int k = kOf(300); k <= kOf(11000); k++) { v.push_back(pU[(size_t)k]); v.push_back(pL[(size_t)k]); }
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 5), v.end());
        floorP = std::max(1e-30, v[v.size() / 5]);
        // audio edge of each side: the highest frequency where the mean power of a 400 Hz window is twice the floor (the programme as strong as the noise)
        auto edge = [&](const std::vector<double>& p) {
            const int w = kOf(200);
            // upwards from 1.5 kHz; the edge is where the programme stays below that for 700 Hz (an isolated line or noise further up
            // does not count)
            double last = 1500;
            int below = 0;
            for (int k = kOf(1500); k <= kOf(10000); k += 4) {
                double mean = 0; int nw = 0;
                for (int j = k - w; j <= k + w; j++) { mean += p[(size_t)std::max(1, std::min(kAnaN / 2 - 1, j))]; nw++; }
                mean /= nw;
                if (mean > 2.0 * floorP) { last = k * bin; below = 0; }
                else if (++below * 4 * bin > 700) break;
            }
            return last;
        };
        const double eU = edge(pU), eL = edge(pL);
        // interference: what one side has that the other has not (a DSB programme is the same on both sides), 300 Hz .. the wider edge
        auto excess = [&](const std::vector<double>& p, const std::vector<double>& q, double& startHz) {
            double ex = 0, tot = 0; startHz = 1e9;
            int run = 0;
            for (int k = kOf(300); k <= kOf(std::max(eU, eL)); k++) {
                const double s = 0.5 * (p[(size_t)k] + q[(size_t)k]);
                tot += s;
                if (p[(size_t)k] > 4 * q[(size_t)k] && p[(size_t)k] > 4 * floorP) { ex += p[(size_t)k] - q[(size_t)k]; if (++run == 20 && startHz > 1e8) startHz = (k - 19) * bin; }
                else run = 0;
            }
            return tot > 0 ? ex / tot : 0.0;
        };
        double sU = 1e9, sL = 1e9;
        const double xU = excess(qU, qL, sU), xL = excess(qL, qU, sL);
        st.upperInterf = (float)xU; st.lowerInterf = (float)xL;
        // sideband choice (with hysteresis: 3 frames in a row)
        int want = 0;
        if (xU > 0.25 && xU > 3 * xL) want = 2;          // trouble on the upper side: listen to the lower
        else if (xL > 0.25 && xL > 3 * xU) want = 1;
        else if (side != 0 && (side == 2 ? xU : xL) > 0.1) want = side;   // stay until it is clearly gone
        if (want == sideWant) sideCount++; else { sideWant = want; sideCount = 0; }
        if (sideCount >= 3) side = sideWant;
        // bandwidth: the edge of what is listened to; in DSB also below where one side's interference starts
        double bw = side == 1 ? eU : side == 2 ? eL : std::min(eU, eL);
        if (side == 0) bw = std::min(bw, std::min(sU, sL) - 100);
        if (side == 1) bw = std::min(bw, sU - 100);
        if (side == 2) bw = std::min(bw, sL - 100);
        bw = std::max<double>(kBwMin, std::min<double>(kBwMax, bw + 300));
        bwWant = (int)(std::lround(bw / kBwStep) * kBwStep);
        // SNR of the programme band
        double sig = 0, noi = 0;
        for (int k = kOf(300); k <= kOf(bwWant); k++) { sig += std::max(0.0, pU[(size_t)k] + pL[(size_t)k] - 2 * floorP); noi += 2 * floorP; }
        st.snrDb = (float)(noi > 0 ? 10 * std::log10(std::max(1e-6, sig / noi)) : 0);
        // whistles: a narrow line on one side only (an interfering carrier), inside the audio band and on a side that is listened to
        std::vector<float> found;
        for (int s = 0; s < 2; s++) {
            const std::vector<double>& p = s == 0 ? pU : pL;
            const std::vector<double>& q = s == 0 ? pL : pU;
            if ((s == 0 && side == 2) || (s == 1 && side == 1)) continue;
            for (int k = kOf(150); k <= kOf(bwWant); k++) {
                if (!(p[(size_t)k] >= p[(size_t)k - 1] && p[(size_t)k] >= p[(size_t)k + 1])) continue;
                if (p[(size_t)k] < 10 * q[(size_t)k] || p[(size_t)k] < 30 * floorP) continue;
                double loc = 0; int nl = 0;
                for (int j = k - 26; j <= k + 26; j++) if (std::abs(j - k) > 4 && j > 0 && j < kAnaN / 2) { loc += p[(size_t)j]; nl++; }
                if (nl && p[(size_t)k] > 30 * loc / nl) found.push_back((float)(k * bin));
            }
        }
        for (auto& c : cands) c.miss++;
        for (float f : found) {
            bool m = false;
            for (auto& c : cands) if (std::fabs(c.hz - f) < 3 * bin) { c.hz = 0.7f * c.hz + 0.3f * f; c.seen++; c.miss = 0; m = true; }
            if (!m && cands.size() < 16) cands.push_back({f, 1, 0});
        }
        cands.erase(std::remove_if(cands.begin(), cands.end(), [](const Cand& c) { return c.miss > 6; }), cands.end());
        // hum: a 50 or 60 Hz series (3 of the first 4 harmonics 10 dB over their surroundings)
        int hz = 0;
        for (int f0 : {50, 60}) {
            int hits = 0;
            for (int h = 1; h <= 4; h++) {
                const int k = kOf(f0 * h);
                const double pk = pU[(size_t)k] + pL[(size_t)k];
                double loc = 0; int nl = 0;
                for (int j = k - 4; j <= k + 4; j++) if (std::abs(j - k) >= 2 && j > 0) { loc += pU[(size_t)j] + pL[(size_t)j]; nl++; }
                if (nl && pk > 10 * loc / nl) hits++;
            }
            if (hits >= 3) hz = f0;
        }
        const bool notchOn = cfg.notch != 0 && (cfg.notch > 0 || cfg.autoQuality);
        if (notchOn) {
            // the notches: candidates seen in 3 frames
            float want[kMaxNotch]; int nw = 0;
            for (const auto& c : cands) if (c.seen >= 3 && c.miss <= 2 && nw < kMaxNotch) want[nw++] = c.hz;
            for (int i = 0; i < nw; i++) {
                bool same = i < nNotch && std::fabs(notchF[i] - want[i]) < 2;
                if (!same) { notchF[i] = want[i]; notch[i].notch(want[i], 12, fs); if (i >= nNotch) notch[i].clear(); }
            }
            nNotch = nw;
            if (hz != humHz) { humHz = hz; for (int h = 0; h < 4; h++) { hum[h].notch(hz * (h + 1), 8, fs); hum[h].clear(); } }
        } else { nNotch = 0; humHz = 0; }
    }

    // ---- noise reduction: one STFT frame (the window filled with the newest kNrN audio samples)
    static double nrBias() { return 1.0; }   // the minimum-statistics bias (measured: 1.0 to 1.2 best on speech and music)
    void nrFrameRun() {
        for (int k = 0; k < kNrN; k++) nrSpec[(size_t)k] = cf32(nrIn[(size_t)((nrInPos + (size_t)k) % kNrN)] * nrWin[(size_t)k], 0.f);
        nrFft->forward(nrSpec.data());
        const int K = kNrN / 2 + 1;
        const double gmin = std::pow(10.0, -nrFloorDb / 20.0);
        for (int k = 0; k < K; k++) {
            const double p = std::norm(nrSpec[(size_t)k]);
            nrSmooth[(size_t)k] = nrPrimed ? 0.92 * nrSmooth[(size_t)k] + 0.08 * p : p;
            // minimum statistics: the minimum of the smoothed power over 8 sub-windows of 12 frames (about 1 s), with a bias of 1.5
            nrMinSub[(size_t)k] = std::min(nrMinSub[(size_t)k], nrSmooth[(size_t)k]);
        }
        if (++nrFrame >= kNrSub) {
            nrFrame = 0;
            nrSubMins[(size_t)nrSub] = nrMinSub;
            nrSub = (nrSub + 1) % 8;
            std::fill(nrMinSub.begin(), nrMinSub.end(), 1e30);
        }
        // a white floor: the 20th percentile of the smoothed power over the audio band; the noise of a bin is held below twice that, so a
        // note held longer than the tracker's window is not taken for noise
        double cap = 1e30;
        {
            std::vector<double> v;
            const int k0 = std::max(1, (int)(300.0 * kNrN / fs)), k1 = std::min(K - 1, (int)((double)bwWant * kNrN / fs));
            for (int k = k0; k <= k1; k++) v.push_back(nrSmooth[(size_t)k]);
            if (v.size() > 8) { std::nth_element(v.begin(), v.begin() + (long)v.size() / 5, v.end()); cap = 1.2 * v[v.size() / 5]; }
        }
        for (int k = 0; k < K; k++) {
            double m = nrMinSub[(size_t)k];
            for (const auto& s : nrSubMins) m = std::min(m, s[(size_t)k]);
            nrNoise[(size_t)k] = std::min(m < 1e29 ? nrBias() * m : nrSmooth[(size_t)k], cap);
        }
        {   // the noise estimate smoothed across 7 bins: a ragged estimate makes ragged gains (musical noise)
            std::vector<double> t(nrNoise);
            for (int k = 0; k < K; k++) { double a = 0; int c = 0; for (int j = std::max(0, k - 3); j <= std::min(K - 1, k + 3); j++) { a += t[(size_t)j]; c++; } nrNoise[(size_t)k] = a / c; }
        }
        if (nrFloorDb > 0) {
            for (int k = 0; k < K; k++) {
                const double p = std::norm(nrSpec[(size_t)k]);
                const double n = std::max(nrNoise[(size_t)k], 1e-20);
                const double gamma = std::min(p / n, 1e4);
                const double xi = std::max(0.985 * nrPrevG2Gamma[(size_t)k] + 0.015 * std::max(gamma - 1, 0.0), 1e-3);
                const double vv = xi / (1 + xi) * gamma;
                double g = xi / (1 + xi) * std::exp(0.5 * expint1(vv));
                g = std::max(gmin, std::min(1.0, g));
                nrPrevG2Gamma[(size_t)k] = g * g * gamma;
                nrG[(size_t)k] = g;
            }
            // musical noise: the gain smoothed across neighbouring bins and over time (fast to open on speech onsets, slow to close)
            std::vector<double> gs(nrG);
            for (int k = 1; k + 1 < K; k++) gs[(size_t)k] = 0.25 * nrG[(size_t)k - 1] + 0.5 * nrG[(size_t)k] + 0.25 * nrG[(size_t)k + 1];
            for (int k = 0; k < K; k++) {
                const double prev = nrGOut[(size_t)k], a = gs[(size_t)k] > prev ? 0.6 : 0.15;
                gs[(size_t)k] = nrGOut[(size_t)k] = prev + a * (gs[(size_t)k] - prev);
            }
            for (int k = 0; k < K; k++) {
                nrSpec[(size_t)k] *= (float)gs[(size_t)k];
                if (k > 0 && k < kNrN / 2) nrSpec[(size_t)(kNrN - k)] *= (float)gs[(size_t)k];
            }
        }
        nrPrimed = true;
        // inverse (unnormalised) and overlap-add with the synthesis window
        nrFft->inverse(nrSpec.data());
        for (int k = 0; k < kNrN; k++) nrOut[(size_t)k] += nrSpec[(size_t)k].real() / kNrN * nrWin[(size_t)k] * (float)(2.0 * kNrHop / kNrN);
        for (int k = 0; k < kNrHop; k++) nrQueue.push_back(nrOut[(size_t)k]);
        std::memmove(nrOut.data(), nrOut.data() + kNrHop, sizeof(float) * (kNrN - kNrHop));
        std::fill(nrOut.begin() + (kNrN - kNrHop), nrOut.end(), 0.f);
    }

    float lpRun(float x) {
        if (lpCur < 0) { lpCur = (bwWant - kBwMin) / kBwStep; lpA.set(lpBank[(size_t)lpCur]); }
        const int want = std::max(0, std::min(kBwN - 1, (bwWant - kBwMin) / kBwStep));
        if (fadeLeft == 0 && want != lpCur) { lpNext = want; lpB.set(lpBank[(size_t)want]); lpB.hist = lpA.hist; lpB.pos = lpA.pos; fadeLeft = 2048; }
        const float a = lpA.run(x);
        if (fadeLeft > 0) {
            const float b = lpB.run(x);
            const float w = (float)fadeLeft / 2048.f;
            fadeLeft--;
            if (fadeLeft == 0) { std::swap(lpA, lpB); lpCur = lpNext; }
            return w * a + (1 - w) * b;
        }
        return a;
    }

    uint64_t carLongN = 0;
    void process(const cf32* xin, size_t n, std::vector<float>& out) {
        std::vector<cf32> x(xin, xin + n);
        for (auto& v : x) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        blank(x.data(), n);
        if (!acquired) acquire(x.data(), n);
        const double carK = 1 - std::exp(-1.0 / (0.001 * fs));     // the carrier filter: about 160 Hz (inside the loop)
        const double longK = 1 - std::exp(-1.0 / (1.0 * fs));
        const double dcK = 1 - std::exp(-1.0 / (0.05 * fs));
        const bool sync = cfg.detector != 0 && (cfg.detector > 0 || cfg.autoQuality);
        std::vector<float> aud;
        aud.reserve(n);
        for (size_t i = 0; i < n; i++) {
            const cf32 z = x[i];
            // PLL (only once a carrier was found)
            const cf32 r = z * cf32((float)std::cos(-ph), (float)std::sin(-ph));
            carC += (float)carK * (r - carC);
            carMag += carK * (std::abs(r) - carMag);
            // the carrier's level over a second: from the carrier filter once locked (an adjacent station does not count), else the envelope
            const double m = locked ? std::abs(carC) : std::abs(z);
            // a running mean while the record is young (the one-second filter would start from zero and the sound gain would creep for seconds), then the filter
            carLong += std::max(longK, 1.0 / (double)++carLongN) * (m - carLong);
            if (acquired) {
                sinceAcq += 1 / fs;
                const double bn = sinceAcq < 0.3 ? 40.0 : 12.0, wn = 2 * bn / (0.707 + 1 / (4 * 0.707)), wT = wn / fs;
                const double e = std::atan2((double)carC.imag(), (double)carC.real());
                fade = std::abs(carC) < 0.25 * carLong && sinceAcq > 0.3;
                if (!fade) { fr += wT * wT * e; ph += fr + 2 * 0.707 * wT * e; } else ph += fr;
                fr = std::clamp(fr, -2 * kPi * 3500 / fs, 2 * kPi * 3500 / fs);
                if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
                // lock: the phase error of the carrier filter's output stays small (its cosine averaged over 50 ms)
                lockQ += (std::cos(e) - lockQ) * (1.0 / (0.05 * fs));
                if (lockQ > 0.7 || fade) { tLock += 1 / fs; unlockFor = 0; } else if (lockQ < 0.4) { unlockFor += 1 / fs; tLock = 0; }
                if (!locked && tLock > 0.1) locked = true;
                if (locked && unlockFor > 0.4) locked = false;
                if (!locked && unlockFor > 1.5) { acquired = false; acqBuf.clear(); acqPow.assign(kAnaN, 0.0); acqFrames = 0; unlockFor = 0; }   // search again
            }
            // analysis on the derotated signal
            anaBuf.push_back(r);
            if ((int)anaBuf.size() == kAnaN) analyse();
            // Hilbert split of the derotated signal
            hPos = (hPos + kHil - 1) % kHil;
            hHist[(size_t)hPos] = hHist[(size_t)(hPos + kHil)] = r;
            const cf32* h = &hHist[(size_t)hPos];
            const cf32 mid = h[kHil / 2];
            const bool useSync = sync && locked;
            const int d = useSync ? (cfg.detector > 0 ? cfg.detector : (side == 0 ? 1 : side == 1 ? 2 : 3)) : 0;
            if (d != det) { prevDet = det; det = d; detFade = (int)(0.02 * fs); }
            // the envelope through the channel filter (both paths delayed alike: the filter's centre is near the Hilbert filter's)
            const int ei = std::max(0, std::min(kBwN - 1, (bwWant - kBwMin) / kBwStep));
            if (ei != envIdx) { envIdx = ei; envR.set(lpBank[(size_t)ei]); envI.set(lpBank[(size_t)ei]); }
            const float fr_ = envR.run(z.real()), fi_ = envI.run(z.imag());
            auto detOut = [&](int dd) -> double {
                if (dd == 0) return std::sqrt((double)fr_ * fr_ + (double)fi_ * fi_);
                if (dd == 1) return mid.real();
                double hi = 0;
                for (int k = 0; k < kHil; k++) hi += hil[(size_t)k] * h[k].imag();
                return dd == 2 ? mid.real() - hi : mid.real() + hi;
            };
            double a = detOut(det);
            if (detFade > 0) { const double w = detFade / (0.02 * fs); a = w * detOut(prevDet) + (1 - w) * a; detFade--; }
            dcS += dcK * (a - dcS);
            const double c = std::max(carLong, 1e-9);
            double y = cfg.agc ? (a - dcS) / c * 0.5 : (a - dcS) * std::pow(10.0, cfg.gainDb / 20.0);
            aud.push_back((float)y);
        }
        // audio: low-pass (the bandwidth), notches, hum
        const int bwFix = cfg.bandwidthHz > 0 ? std::clamp(cfg.bandwidthHz, kBwMin, kBwMax) : 0;
        if (bwFix) bwWant = bwFix;
        else if (!cfg.autoQuality) bwWant = 4500;
        for (auto& v : aud) {
            double y = lpRun(v);
            for (int k = 0; k < nNotch; k++) y = notch[k].run(y);
            if (humHz) for (auto& b : hum) y = b.run(y);
            v = (float)y;
        }
        // noise reduction strength
        if (cfg.nr == 0 || (cfg.nr < 0 && !cfg.autoQuality)) nrFloorDb = 0;
        else if (cfg.nr > 0) nrFloorDb = 3 + 0.15 * cfg.nr;
        else nrFloorDb = st.snrDb > 35 ? 0 : std::clamp(6.0 + (35.0 - st.snrDb) * 0.4, 6.0, 16.0);
        // through the STFT (always: the delay stays the same when the strength changes)
        for (float v : aud) {
            nrIn[nrInPos] = v; nrInPos = (nrInPos + 1) % kNrN;
            if (((nrInPos) % kNrHop) == 0) nrFrameRun();
        }
        // EQ, extension, leveler, limiter, DC
        const double eqDb = cfg.eq == 0 ? 0 : (cfg.eq > 0 || cfg.autoQuality) && bwWant <= 6000 ? 3.0 : 0.0;
        if (eqDb != eqSet) { eqSet = (float)eqDb; if (eqDb > 0) eqShelf.highShelf(0.55 * bwWant, eqDb, fs); }
        if (cfg.enhance && exSet != bwWant) { exSet = (float)bwWant; exBp.bandpass(0.7 * bwWant, 1.5, fs); exHp.highpass(bwWant, 0.707, fs); }
        const double tgt = 0.12;
        while (nrQHead < nrQueue.size()) {
            double y = nrQueue[nrQHead++];
            if (eqSet > 0) y = eqShelf.run(y);
            if (cfg.enhance) {   // harmonics of the top octave, high-passed above the edge, at -15 dB
                const double b = exBp.run(y);
                y += 0.18 * exHp.run(std::fabs(b) - 0.5 * exEnv);
                exEnv += 0.001 * (std::fabs(b) - exEnv);
            }
            if (cfg.agc) {
                // leveler: 60 ms rms, attack 250 ms, release 1.5 s after a hang of 0.8 s, within +-12 dB
                lvlRms += (y * y - lvlRms) * (1.0 / (0.06 * fs));
                const double r = std::sqrt(lvlRms);
                if (r > 1e-4 && cfg.leveler) {
                    const double want = std::clamp(tgt / r, 0.25, 4.0);
                    if (want < lvlGain) { lvlGain += (want - lvlGain) * (1.0 / (0.25 * fs)); lvlHang = 0.8; }
                    else if (lvlHang > 0) lvlHang -= 1 / fs;
                    else lvlGain += (want - lvlGain) * (1.0 / (1.5 * fs));
                }
                y *= lvlGain;
                // limiter: 3 ms ahead, at 0.9
                const float old = limBuf[(size_t)limPos];
                limBuf[(size_t)limPos] = (float)y; limPos = (limPos + 1) % kLook;
                double pk = 0;
                for (float s : limBuf) pk = std::max(pk, (double)std::fabs(s));
                const double need = pk > 0.9 ? 0.9 / pk : 1.0;
                limGain = need < limGain ? need : limGain + (need - limGain) * (1.0 / (0.05 * fs));
                y = old * limGain;
            }
            out.push_back((float)std::clamp(y, -1.0, 1.0));
        }
        if (nrQHead > 65536) { nrQueue.erase(nrQueue.begin(), nrQueue.begin() + (long)nrQHead); nrQHead = 0; }
        publish(n);
    }

    void publish(size_t n) {
        pubT += n / fs;
        if (pubT < 0.1) return;
        pubT = 0;
        st.locked = locked && acquired;
        st.carrierHz = fr * fs / (2 * kPi);
        st.carrierDb = (float)(20 * std::log10(std::max(carLong, 1e-10)));
        st.detector = det;
        st.ecss = det >= 2 && cfg.detector < 0;
        st.bandwidthHz = (float)bwWant;
        st.nrDb = (float)nrFloorDb;
        st.notchHz.assign(notchF, notchF + nNotch);
        st.humHz = humHz;
        st.fading = fade;
        st.enhanced = cfg.enhance;
        st.levelDb = (float)(20 * std::log10(std::max(lvlGain, 1e-6)));
        // plain words
        std::string s;
        char b[96];
        if (!st.carrier && !acquired) s = "Looking for the carrier";
        else if (!st.locked) s = "Envelope detector (no steady carrier to lock to)";
        else {
            snprintf(b, sizeof b, "Locked to the carrier (%+.0f Hz)", st.carrierHz); s = b;
            if (st.ecss) s += det == 3 ? ", interference on the upper sideband: lower one only" : ", interference on the lower sideband: upper one only";
            if (fade) s += ", fading: holding";
        }
        if (cfg.autoQuality || cfg.bandwidthHz) { snprintf(b, sizeof b, "; audio to %.1f kHz", bwWant / 1000.0); s += b; }
        if (nrFloorDb > 0) s += st.snrDb < 15 ? "; weak signal: strong noise reduction" : "; noise reduction";
        if (st.blankPct > 0.05f) s += "; clicks blanked";
        for (int i = 0; i < nNotch; i++) { snprintf(b, sizeof b, "; whistle at %.2f kHz removed", notchF[i] / 1000.0); s += b; }
        if (humHz) { snprintf(b, sizeof b, "; %d Hz hum removed", humHz); s += b; }
        if (cfg.enhance) s += "; enhanced (synthetic treble)";
        st.text = s;
    }
};

AmProcessor::AmProcessor() : p_(std::make_unique<Impl>()) {}
AmProcessor::~AmProcessor() {}
void AmProcessor::configure(double rate) { p_->configure(rate); }
void AmProcessor::reset() { p_->reset(); }
void AmProcessor::setConfig(const AmProcConfig& c) { p_->cfg = c; }
void AmProcessor::process(const cf32* x, size_t n, std::vector<float>& out) { p_->process(x, n, out); }
AmProcStatus AmProcessor::status() const { return p_->st; }

} // namespace dect2
