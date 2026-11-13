// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging detector (see multitone_rx.h): the front end of the pager receiver, an FM discriminator, then a per-transmission analysis.
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kMidRate = 96000.0, kWork = 48000.0;
constexpr size_t kMaxSeg = (size_t)(6.0 * kWork);
constexpr int kAudDecim = 6;                  // 48 kHz -> 8 kHz audio for the spectrum
constexpr int kWin = 512, kHop = 256, kFftN = 1024;
constexpr double kAudRate = kWork / kAudDecim;
constexpr double kLoHz = 60, kHiHz = 3300;

int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kMidRate));
    const int lo = std::max(1, (int)std::floor(fs / 130000.0));
    auto largestPrime = [](int v) { int lp = 1; for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; } return lp; };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) { if (d % p == 0) { primes.push_back(p); d /= p; } else p++; }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) { if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p; else stages.push_back(p); }
    return stages;
}

float pctF(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, (size_t)(q * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)i, v.end());
    return v[i];
}

struct Frame { std::vector<float> p; };   // power per bin, 0 .. kFftN / 2

struct ToneEv { double hz, startSec, durSec; };
} // namespace

struct MultitoneReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    MultitoneTelemetry pub;
    std::atomic<bool> resetReq{true}, clearReq{false};

    // receiver thread
    MultitoneTelemetry tel;
    double curRate = 0;
    bool built = false;
    int64_t nIn = 0;
    double nextReport = 0, power = 0;
    int64_t nPower = 0;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    dmr::Fir<cf32> fin;
    double offPhase = 0;
    std::vector<cf32> bufA, bufB, mixBuf, rsOut, finOut, clean;
    cf32 prev = cf32(0, 0);
    float d1 = 0, d2 = 0;
    double blkPow = 0; int blkN = 0;
    double noiseFloor = 0, snrEma = 0, snrPeak = 0;
    int sigHold = 0;
    bool haveFloor = false;
    bool inSeg = false;
    int64_t segStart = 0;                 // work-rate sample index
    int64_t nWork = 0;
    std::vector<float> seg;
    double lastEndSec = -1e9;
    Fft fft{kFftN};
    std::vector<float> hann;

    // results (under mu)
    std::vector<MtTone> tones;
    std::deque<MtTx> txs;
    std::deque<MtBurst> bursts;
    uint64_t txSerial = 0, burstSerial = 0, txTotal = 0, burstTotal = 0;
    uint64_t wordsOk = 0, wordsBad = 0;
    bool dirty = false;

    void say(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = log; } if (cb) cb(s); }
    double nowSec() const { return curRate > 0 ? (double)nIn / curRate : 0; }

    void resetState() {
        double r;
        { std::lock_guard<std::mutex> lk(mu); r = rate; curRate = r; }
        const uint64_t s = tel.seq;
        tel = MultitoneTelemetry();
        tel.seq = s;
        tel.inputRate = r;
        nIn = 0; nextReport = 0; power = 0; nPower = 0; nWork = 0;
        built = false;
        stages.clear();
        offPhase = 0;
        if (r >= multitoneTuning().minSampleRate - 1) {
            const int d = pickDecimation(r);
            double rr = r;
            for (int f : splitFactors(d)) {
                stages.emplace_back();
                const double out = rr / f;
                const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
                stages.back().design(dmr::lowpassTaps(pass, std::min(stop, rr * 0.5), rr, 60, 9), f);
                rr = out;
            }
            rsPass = std::fabs(rr - kMidRate) < 0.01;
            built = rsPass || rs.configure(rr, kMidRate);
            if (!rsPass) rs.reset();
            fin.design(dmr::lowpassTaps(10000.0, 14000.0, kMidRate, 60, 9), 2);
        }
        prev = cf32(0, 0); d1 = d2 = 0;
        blkPow = 0; blkN = 0; noiseFloor = 0; snrEma = 0; snrPeak = 0; sigHold = 0; haveFloor = false;
        inSeg = false; seg.clear(); lastEndSec = -1e9;
        if (hann.empty()) { hann.resize(kWin); for (int i = 0; i < kWin; i++) hann[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kWin)); }
        if (built) say("Multitone: listening (detector only: the format is not public)");
    }

    void report() {
        tel.seq++;
        tel.timeSec = nowSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.signal = sigHold > 0;
        tel.state = (sigHold > 0 || tel.timeSec - lastEndSec < 2.0) ? 1 : 0;
        tel.snrDb = tel.signal ? (float)snrEma : 0.f;
        std::lock_guard<std::mutex> lk(mu);
        tel.dataValid = !txs.empty();
        tel.blocksOk = wordsOk; tel.blocksBad = wordsBad;
        tel.txTotal = txTotal; tel.burstTotal = burstTotal;
        if (dirty) {
            pub.tones = tones;
            std::sort(pub.tones.begin(), pub.tones.end(), [](const MtTone& a, const MtTone& b) { return a.count > b.count; });
            pub.tx.assign(txs.begin(), txs.end());
            pub.bursts.assign(bursts.begin(), bursts.end());
            dirty = false;
        }
        auto tones2 = std::move(pub.tones); auto tx2 = std::move(pub.tx); auto b2 = std::move(pub.bursts);
        pub = tel;
        pub.tones = std::move(tones2); pub.tx = std::move(tx2); pub.bursts = std::move(b2);
    }

    // ---- the analysis of one transmission
    void addTone(double hz, double dur, double at) {
        for (auto& t : tones)
            if (std::fabs(t.hz - hz) < 6) { t.hz = (float)((t.hz * t.count + hz) / (t.count + 1)); t.count++; t.totalSec += (float)dur; t.lastSec = at; return; }
        if (tones.size() >= kMtToneMax) {
            size_t w = 0;
            for (size_t i = 1; i < tones.size(); i++) if (tones[i].count < tones[w].count) w = i;
            tones.erase(tones.begin() + (long)w);
        }
        MtTone t; t.hz = (float)hz; t.count = 1; t.totalSec = (float)dur; t.lastSec = at;
        tones.push_back(t);
    }

    void finishSeg(double trimSec) {
        const size_t trim = (size_t)(trimSec * kWork);
        if (seg.size() > trim) seg.resize(seg.size() - trim); else seg.clear();
        const double dur = (double)seg.size() / kWork;
        const double startSec = (double)segStart / kWork;
        const bool keep = dur >= 0.2;
        std::vector<float> disc;
        disc.swap(seg);
        if (!keep) return;
        MtTx tx;
        tx.timeSec = startSec; tx.durSec = dur; tx.wall = (int64_t)std::time(nullptr);
        tx.snrDb = (float)snrPeak;
        const float p02 = pctF(disc, 0.02), p98 = pctF(disc, 0.98);
        tx.devHz = (float)(0.5 * (p98 - p02) * kWork / (2 * kPi));

        // audio at 8 kHz, spectra
        std::vector<float> aud;
        for (size_t i = 0; i + kAudDecim <= disc.size(); i += kAudDecim) { float s = 0; for (int k = 0; k < kAudDecim; k++) s += disc[i + (size_t)k]; aud.push_back(s / kAudDecim); }
        double mean = 0;
        for (float v : aud) mean += v;
        if (!aud.empty()) mean /= (double)aud.size();
        for (float& v : aud) v -= (float)mean;
        std::vector<Frame> frames;
        std::vector<cf32> buf(kFftN);
        for (size_t s = 0; s + kWin <= aud.size(); s += kHop) {
            std::fill(buf.begin(), buf.end(), cf32(0, 0));
            for (int i = 0; i < kWin; i++) buf[(size_t)i] = cf32(aud[s + (size_t)i] * hann[(size_t)i], 0);
            fft.forward(buf.data());
            Frame f; f.p.resize(kFftN / 2 + 1);
            for (int k = 0; k <= kFftN / 2; k++) f.p[(size_t)k] = std::norm(buf[(size_t)k]);
            frames.push_back(std::move(f));
        }
        const double binHz = kAudRate / kFftN;
        const int k0 = (int)std::ceil(kLoHz / binHz), k1 = (int)(kHiHz / binHz);
        // spectral lines of the average
        std::vector<double> avg;
        if (frames.size() >= 4) {
            avg.assign((size_t)(kFftN / 2 + 1), 0.0);
            for (const Frame& f : frames) for (size_t k = 0; k < avg.size(); k++) avg[k] += f.p[k];
            std::vector<double> band(avg.begin() + k0, avg.begin() + k1 + 1);
            std::vector<double> sorted = band;
            std::nth_element(sorted.begin(), sorted.begin() + (long)(sorted.size() / 2), sorted.end());
            const double med = std::max(sorted[sorted.size() / 2], 1e-30);
            std::vector<std::pair<double, int>> pk;
            for (int k = k0 + 3; k <= k1 - 3; k++) {
                bool mx = true;
                for (int j = -3; j <= 3; j++) if (avg[(size_t)(k + j)] > avg[(size_t)k]) mx = false;
                if (mx && 10 * std::log10(avg[(size_t)k] / med) >= 12.0) pk.push_back({avg[(size_t)k], k});
            }
            std::sort(pk.rbegin(), pk.rend());
            for (const auto& q : pk) {
                if (tx.lineHz.size() >= 8) break;
                const int k = q.second;
                const double a = std::log(avg[(size_t)k - 1] + 1e-30), b = std::log(avg[(size_t)k] + 1e-30), c = std::log(avg[(size_t)k + 1] + 1e-30);
                const double den = a - 2 * b + c, off = den != 0 ? 0.5 * (a - c) / den : 0;
                const float hz = (float)((k + std::max(-0.5, std::min(0.5, off))) * binHz);
                bool nearBy = false;
                for (float h : tx.lineHz) if (std::fabs(h - hz) < 30) nearBy = true;
                if (nearBy) continue;
                tx.lineHz.push_back(hz);
                tx.lineDb.push_back((float)(10 * std::log10(q.first / med)));
            }
        }
        // steady tones frame by frame
        std::vector<ToneEv> ev;
        {
            const double hopSec = (double)kHop / kAudRate;
            double runHz = 0, runStart = 0; int runN = 0, miss = 0;
            auto closeRun = [&]() {
                if (runN >= 3) ev.push_back({runHz / runN, runStart, (double)runN * hopSec});
                runN = 0; runHz = 0; miss = 0;
            };
            for (size_t fi = 0; fi < frames.size(); fi++) {
                const std::vector<float>& p = frames[fi].p;
                int pk = k0; double tot = 0;
                for (int k = k0; k <= k1; k++) { tot += p[(size_t)k]; if (p[(size_t)k] > p[(size_t)pk]) pk = k; }
                std::vector<float> bandp(p.begin() + k0, p.begin() + k1 + 1);
                const double med = std::max((double)pctF(bandp, 0.5), 1e-30);
                double nearBy = 0;
                for (int j = -3; j <= 3; j++) if (pk + j >= 0 && pk + j <= kFftN / 2) nearBy += p[(size_t)(pk + j)];
                bool tone = tot > 0 && 10 * std::log10(p[(size_t)pk] / med) >= 15 && nearBy / tot >= 0.4 && pk > k0 + 1 && pk < k1 - 1;
                double hz = 0;
                if (tone) {
                    const double a = std::log(p[(size_t)pk - 1] + 1e-30), b = std::log(p[(size_t)pk] + 1e-30), c = std::log(p[(size_t)pk + 1] + 1e-30);
                    const double den = a - 2 * b + c, off = den != 0 ? 0.5 * (a - c) / den : 0;
                    hz = (pk + std::max(-0.5, std::min(0.5, off))) * binHz;
                }
                if (tone && (runN == 0 || std::fabs(hz - runHz / runN) < 14)) {
                    if (runN == 0) runStart = startSec + (double)fi * hopSec;
                    runHz += hz; runN++; miss = 0;
                } else if (runN > 0 && !tone && miss < 1) miss++;
                else { closeRun(); if (tone) { runStart = startSec + (double)fi * hopSec; runHz = hz; runN = 1; } }
            }
            closeRun();
        }
        std::string toneTxt;
        for (const ToneEv& e : ev) {
            char b[64];
            snprintf(b, sizeof b, "%s%.1f Hz %.2f s", toneTxt.empty() ? "" : "; ", e.hz, e.durSec);
            toneTxt += b;
        }
        tx.tones = toneTxt; tx.toneCount = (int)ev.size();

        // a repeating audio pattern (a tone cycle, a preamble repeated): the autocorrelation of the first 2 s, lags 10 - 400 ms
        {
            const size_t N = std::min(aud.size(), (size_t)(2.0 * kAudRate));
            const size_t l0 = (size_t)(0.010 * kAudRate), l1 = std::min(N / 2, (size_t)(0.400 * kAudRate));
            if (N > 4 * l0 && l1 > l0) {
                double r0 = 0;
                for (size_t i = 0; i < N; i++) r0 += (double)aud[i] * aud[i];
                std::vector<double> r(l1 + 2, 0.0);
                for (size_t l = l0 - 1; l <= l1 + 1; l++) { double acc = 0; for (size_t i = 0; i + l < N; i++) acc += (double)aud[i] * aud[i + l]; r[l] = r0 > 0 ? acc / r0 * (double)N / (double)(N - l) : 0; }
                double gm = 0;
                for (size_t l = l0; l <= l1; l++) if (r[l] >= r[l - 1] && r[l] >= r[l + 1]) gm = std::max(gm, r[l]);
                if (gm >= 0.35)
                    for (size_t l = l0; l <= l1; l++)
                        if (r[l] >= r[l - 1] && r[l] >= r[l + 1] && r[l] >= 0.8 * gm) { tx.periodMs = (float)(1000.0 * (double)l / kAudRate); tx.periodStrength = (float)r[l]; break; }
            }
        }
        // FSK: the whole transmission, then halves and quarters (a tone preamble may sit in front of the data)
        multitone::FskResult fr;
        double fskStart = 0, fskDur = dur;
        {
            const size_t N = disc.size();
            const size_t parts[][2] = {{0, 1}, {0, 2}, {1, 2}, {0, 4}, {1, 4}, {2, 4}, {3, 4}};
            const int div[] = {1, 2, 2, 4, 4, 4, 4};
            for (int pi = 0; pi < 7 && !fr.found; pi++) {
                const size_t a = N * parts[pi][0] / (size_t)div[pi], b = N * (parts[pi][0] + 1) / (size_t)div[pi];
                std::vector<float> sub(disc.begin() + (long)a, disc.begin() + (long)b);
                multitone::FskResult r = multitone::sliceFsk(sub, kWork);
                if (r.found && r.nBits >= 64) { fr = std::move(r); fskStart = (double)a / kWork; fskDur = (double)(b - a) / kWork; }
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        tx.id = ++txSerial; txTotal++;
        for (const ToneEv& e : ev) addTone(e.hz, e.durSec, e.startSec);
        if (fr.found) {
            MtBurst b;
            b.id = ++burstSerial; burstTotal++; b.txId = tx.id; b.wall = tx.wall;
            b.timeSec = startSec + fskStart; b.durSec = fskDur;
            b.ffsk = fr.ffsk; b.toneLoHz = (float)fr.loHz; b.toneHiHz = (float)fr.hiHz;
            b.baud = fr.baud; b.nearestStd = multitone::nearestStandardBaud(fr.baud); b.coherence = fr.coherence;
            multitone::analyseBits(fr.bits, b);
            if (b.bestRun >= 2) wordsOk += (uint64_t)b.validWords;
            wordsBad += (uint64_t)std::max(0, b.totalWords - b.validWords);
            tx.burst = b.id;
            tx.kind = b.ffsk ? "FFSK data" : "FSK data";
            char m[240];
            snprintf(m, sizeof m, "Multitone: %s %.1f baud, %d bits, %d valid BCH words (best run %d), 0x605A x%d%s", b.ffsk ? "FFSK data" : "FSK data", b.baud, b.nBits, b.validWords, b.bestRun, b.syncCount, b.syncFound ? ", POCSAG sync" : "");
            bursts.push_front(std::move(b));
            if (bursts.size() > kMtBurstMax) bursts.pop_back();
            if (log) log(m);
        } else if (tx.toneCount >= 1 && (tx.lineHz.size() <= 2 || tx.toneCount >= 2)) tx.kind = "tone sequence";
        else if (tx.lineHz.size() >= 2) tx.kind = tx.periodMs > 0 ? "repeating multi-tone audio" : "multi-tone audio";
        else tx.kind = "other";
        if (log && !fr.found) { char m[200]; snprintf(m, sizeof m, "Multitone: %s, %.1f s%s%s", tx.kind.c_str(), dur, tx.tones.empty() ? "" : ": ", tx.tones.c_str()); log(m); }
        txs.push_front(std::move(tx));
        if (txs.size() > kMtTxMax) txs.pop_back();
        dirty = true;
    }

    // ---- per work-rate sample
    void blockPower() {
        const double p = blkPow / blkN;
        blkPow = 0; blkN = 0;
        if (!haveFloor) { noiseFloor = p; haveFloor = true; }
        else noiseFloor = std::min(p, noiseFloor * 1.0005);
        if (p > 2.0 * noiseFloor) {
            sigHold = 30;
            const double s = 10 * std::log10(std::max(p - noiseFloor, 1e-20) / std::max(noiseFloor, 1e-20));
            snrEma = snrEma == 0 ? s : 0.9 * snrEma + 0.1 * s;
            snrPeak = std::max(snrPeak, snrEma);
        } else if (sigHold > 0) sigHold--;
        if (sigHold > 0 && !inSeg) { inSeg = true; seg.clear(); segStart = nWork; snrPeak = snrEma; }
        else if (sigHold == 0 && inSeg) { inSeg = false; finishSeg(0.31); lastEndSec = nowSec(); snrEma = 0; }
    }

    void onSample(cf32 z) {
        blkPow += std::norm(z);
        const cf32 d = z * std::conj(prev);
        float f = 0;
        if (std::norm(d) > 1e-30f) f = (float)std::atan2((double)d.imag(), (double)d.real());
        prev = z;
        const float sm = (f + d1 + d2) * (1.f / 3.f);
        d2 = d1; d1 = f;
        nWork++;
        if (inSeg) {
            seg.push_back(sm);
            if (seg.size() >= kMaxSeg) { finishSeg(0); segStart = nWork; seg.clear(); }
        }
        if (++blkN >= 480) blockPower();
    }

    void feed(const cf32* x, size_t n) {
        if (!built || n == 0) return;
        for (size_t i = 0; i < n; i++) power += (double)std::norm(x[i]);
        nPower += (int64_t)n;
        const cf32* in = x;
        bool bad = false;
        for (size_t i = 0; i < n; i++) { const float re = x[i].real(), im = x[i].imag(); if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; } }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& s : clean) if (!(std::fabs(s.real()) < 1e4f && std::fabs(s.imag()) < 1e4f)) s = cf32(0, 0);
            in = clean.data();
        }
        double off;
        { std::lock_guard<std::mutex> lk(mu); off = offsetHz; }
        const cf32* c = in;
        size_t cn = n;
        if (off != 0) {
            mixBuf.resize(n);
            const double wm = -2 * kPi * off / curRate;
            const cf32 stp((float)std::cos(wm), (float)std::sin(wm));
            cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
            for (size_t i = 0; i < n; i++) { mixBuf[i] = in[i] * ph; ph *= stp; if ((i & 1023) == 1023) ph /= std::abs(ph); }
            offPhase = std::fmod(offPhase + wm * (double)n, 2 * kPi);
            c = mixBuf.data();
        }
        for (size_t s = 0; s < stages.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
            dst.clear();
            stages[s].process(c, cn, dst);
            c = dst.data(); cn = dst.size();
        }
        if (!rsPass) { rsOut.clear(); rs.process(c, cn, rsOut); c = rsOut.data(); cn = rsOut.size(); }
        finOut.clear();
        fin.process(c, cn, finOut);
        nIn += (int64_t)n;
        for (const cf32& z : finOut) onSample(z);
        while ((double)nIn >= nextReport) { nextReport += 0.25 * curRate; report(); }
    }
};

MultitoneReceiver::MultitoneReceiver() : p_(std::make_unique<Impl>()) {}
MultitoneReceiver::~MultitoneReceiver() = default;

void MultitoneReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = inputRateHz; p_->resetReq = true; }
void MultitoneReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->resetReq = true; }
bool MultitoneReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= multitoneTuning().minSampleRate - 1; }
void MultitoneReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    auto t = std::move(p_->pub.tones); auto x = std::move(p_->pub.tx); auto b = std::move(p_->pub.bursts);
    p_->pub = MultitoneTelemetry();
    p_->pub.seq = s;
    p_->pub.tones = std::move(t); p_->pub.tx = std::move(x); p_->pub.bursts = std::move(b);
}
void MultitoneReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tones.clear(); p_->txs.clear(); p_->bursts.clear();
    p_->txTotal = p_->burstTotal = p_->wordsOk = p_->wordsBad = 0;
    p_->pub.tones.clear(); p_->pub.tx.clear(); p_->pub.bursts.clear();
    p_->pub.seq++;
    p_->dirty = false;
}
std::string MultitoneReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<MtTx> tx(p_->txs.begin(), p_->txs.end());
    std::vector<MtBurst> b(p_->bursts.begin(), p_->bursts.end());
    return results::pack(p_->pub.timeSec, p_->tones, tx, b, p_->txSerial, p_->burstSerial, p_->txTotal, p_->burstTotal);
}
bool MultitoneReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<MtTone> tones; std::vector<MtTx> tx; std::vector<MtBurst> b;
    uint64_t a1 = 0, a2 = 0, a3 = 0, a4 = 0;
    if (!results::unpack(blob, t0, tones, tx, b, a1, a2, a3, a4)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tones = std::move(tones);
    p_->txs.assign(tx.begin(), tx.end());
    p_->bursts.assign(b.begin(), b.end());
    p_->txSerial = std::max(p_->txSerial, a1); p_->burstSerial = std::max(p_->burstSerial, a2);
    p_->txTotal = a3; p_->burstTotal = a4;
    p_->dirty = true;
    p_->pub.tones = p_->tones;
    p_->pub.tx.assign(p_->txs.begin(), p_->txs.end());
    p_->pub.bursts.assign(p_->bursts.begin(), p_->bursts.end());
    p_->pub.seq++;
    return true;
}
void MultitoneReceiver::feed(const cf32* x, size_t n) {
    Impl& im = *p_;
    if (im.resetReq.exchange(false)) im.resetState();
    if (im.curRate <= 0) return;
    im.feed(x, n);
}
bool MultitoneReceiver::telemetry(MultitoneTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void MultitoneReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning multitoneTuning() {
    ModeTuning t;
    t.stdMode = 77; t.id = "multitone"; t.name = "Multitone Paging";
    t.minMhz = 25; t.maxMhz = 480; t.defMhz = 446.475;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 50000;
    return t;
}

} // namespace dect2
