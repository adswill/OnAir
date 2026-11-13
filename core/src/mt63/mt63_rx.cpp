// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 receiver (see mt63_rx.h).
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_proto.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cstdio>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kBase = marine::kBaseRate;      // 24 kHz complex from the front end
constexpr double kFs = 8000;                     // the analysis rate
constexpr int kN = 2048, kHop = 1024;            // window spectrum
constexpr double kBinHz = kFs / kN;
constexpr double kLoHz = 100, kHiHz = 3900;      // the audio passband searched
constexpr int kLoBin = (int)(kLoHz / kBinHz) + 1, kHiBin = (int)(kHiHz / kBinHz);
constexpr int kRing = 1 << 16;                   // 8 s of 8 kHz samples
constexpr int kSyncSyms = 16;                    // symbols the synchroniser looks at
constexpr int kTimingPhases = 8;
constexpr int kBinSearch = 12;                   // comb positions tried each side of the nominal one (FFT bins of the internal rate)
constexpr int kBnom = mt63::kFft - 128;          // bin of the first carrier of a comb centred on the mixer frequency
constexpr int kEL = 24;                          // early / late offset of the timing loop (internal-rate samples)
constexpr float kCharConf = 5.0f;                // Walsh peak ratio over which a character is accepted
constexpr float kLockConf = 6.5f;                // smoothed ratio that confirms a comb position
constexpr float kDropConf = 3.8f;                // smoothed ratio under which a lock is given up

// Complex samples through a real low-pass and down by an integer
struct Decim {
    int dec = 1, n = 0, pos = 0, phase = 0;
    std::vector<float> taps;
    std::vector<cf32> hist;
    void configure(int d, double cutoff, int ntaps) {   // cutoff in cycles per input sample
        dec = d; n = ntaps;
        const auto lp = hfdig::lowpass(n, cutoff, 6.0);
        taps.assign(lp.begin(), lp.end());
        hist.assign((size_t)(2 * n), cf32(0.f, 0.f));
        pos = 0; phase = 0;
    }
    bool push(cf32 x, cf32& out) {
        hist[(size_t)pos] = x; hist[(size_t)(pos + n)] = x;
        if (++pos >= n) pos = 0;
        if (++phase < dec) return false;
        phase = 0;
        const cf32* w = &hist[(size_t)pos];
        float re = 0, im = 0;
        for (int k = 0; k < n; k++) { re += taps[(size_t)k] * w[k].real(); im += taps[(size_t)k] * w[k].imag(); }
        out = cf32(re, im);
        return true;
    }
};

struct Comb {
    int b0 = 0;
    std::array<std::array<cf32, mt63::kCarriers>, 3> prev;   // the previous symbol's carriers at the early, centre and late frame
    bool havePrev = false;
    cf32 rot[mt63::kCarriers];
    mt63::Deinterleaver dS{false}, dL{true};
    float qS = 0, qL = 0;
    void setup(int b) {
        b0 = b;
        for (int i = 0; i < mt63::kCarriers; i++) {
            const int k = (b0 + mt63::kCarrSep * i) & (mt63::kFft - 1);
            rot[i] = std::polar(1.f, (float)(-2 * kPi * k * mt63::kSymSep / mt63::kFft));
        }
        havePrev = false;
        dS.reset(); dL.reset(); qS = qL = 0;
    }
    int bin(int i) const { return (b0 + mt63::kCarrSep * i) & (mt63::kFft - 1); }
};

} // namespace

struct Mt63Receiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    Mt63Telemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    std::atomic<int> bwReq{0}, intlReq{0};
    // receiver thread
    Mt63Telemetry tel;
    double curRate = 0;
    bool running = false;
    int64_t nIn = 0, n8 = 0;
    double nextReport = 0;
    double power = 0; int64_t nPower = 0;
    marine::Front front;
    hfdig::Listen listen;
    std::vector<cf32> base;
    Decim dec24;
    // 8 kHz stream and its spectrum
    std::vector<cf32> ring;
    std::vector<cf32> fftIn;
    Fft fft{kN}, fft512{mt63::kFft};
    std::vector<float> win, held, acc, avgP;
    int accN = 0, frames = 0, sinceSearch = 0;
    float floorLin = 0;
    std::vector<float> win512;
    double cooldownSec = 0;
    // lock
    bool locked = false, verified = false;
    int bw = 0, dec = 4, selIntl = 0, symSinceLock = 0, lowCount = 0;
    double fcMix = 0, fcLock = 0, theta = 0, tSym = 0.1;
    Decim dec8;
    std::vector<cf32> yr;
    int64_t yrBase = 0, nextStart = 0;
    std::vector<Comb> combs;
    float tauAcc = 0;
    float snrEma = 0;
    std::array<float, mt63::kCarriers> carrPow{};
    // text
    std::string text;
    std::vector<Mt63Line> lines;
    std::vector<std::string> calls;
    bool lineOpen = false;
    int lastCh = 0;
    uint64_t chars = 0, bad = 0;
    int syncs = 0;

    Impl() {
        win.resize(kN);
        for (int i = 0; i < kN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN));
        win512.resize(mt63::kFft);
        for (int i = 0; i < mt63::kFft; i++) win512[(size_t)i] = (float)mt63::pulse(i);
        dec24.configure(3, 3900.0 / kBase, 127);
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    double sigSec() const { return (double)n8 / kFs; }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        const uint64_t s = tel.seq, ss = tel.specSeq;
        tel = Mt63Telemetry();
        tel.seq = s; tel.specSeq = ss;
        tel.inputRate = curRate;
        tel.loHz = kLoBin * kBinHz; tel.hiHz = kHiBin * kBinHz;
        nIn = n8 = 0; nextReport = 0; power = 0; nPower = 0;
        running = curRate >= mt63Tuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        listen.reset();
        base.clear(); fftIn.clear();
        dec24.configure(3, 3900.0 / kBase, 127);
        ring.assign(kRing, cf32(0.f, 0.f));
        const int W = kHiBin - kLoBin + 1;
        held.assign((size_t)W, 0.f); acc.assign((size_t)W, 0.f);
        avgP.assign(kN / 2, 0.f);
        accN = frames = sinceSearch = 0;
        floorLin = 0; cooldownSec = 0;
        locked = verified = false; combs.clear(); yr.clear();
        syncs = 0;
    }

    // ------------------------------------------------------------ the spectrum and the search
    void spectrumFrame() {
        std::vector<cf32> b(kN);
        for (int i = 0; i < kN; i++) b[(size_t)i] = fftIn[(size_t)i] * win[(size_t)i];
        fft.forward(b.data());
        const int W = kHiBin - kLoBin + 1;
        for (int i = 0; i < W; i++) {
            const float p = std::norm(b[(size_t)(kLoBin + i)]) * (4.f / ((float)kN * kN));
            held[(size_t)i] += (p > held[(size_t)i] ? 0.5f : 0.05f) * (p - held[(size_t)i]);
            acc[(size_t)i] += p;
        }
        for (int k = 0; k < kN / 2; k++) {
            const float p = std::norm(b[(size_t)k]) * (4.f / ((float)kN * kN));
            avgP[(size_t)k] += 0.12f * (p - avgP[(size_t)k]);
        }
        accN++;
        std::vector<float> srt(held);
        std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
        floorLin = srt[(size_t)W / 2];
        frames++;
        sinceSearch++;
    }

    struct Cand { int bw = 0; double fc = 0; double contrast = 0; };

    // A flat block of the width of one of the modes: power in the box against the bands beside it
    Cand search() {
        Cand best;
        std::vector<double> ps((size_t)kN / 2 + 1, 0.0);
        for (int k = 0; k < kN / 2; k++) ps[(size_t)k + 1] = ps[(size_t)k] + avgP[(size_t)k];
        auto mean = [&](double f0, double f1, bool& ok) {
            int a = (int)std::lround(f0 / kBinHz), b = (int)std::lround(f1 / kBinHz);
            if (a < kLoBin || b > kHiBin || b <= a) { ok = false; return 0.0; }
            ok = true;
            return (ps[(size_t)b + 1] - ps[(size_t)a]) / (b - a + 1);
        };
        std::vector<float> srt(avgP.begin() + kLoBin, avgP.begin() + kHiBin);
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        const double med = srt[srt.size() / 2];
        const int want = bwReq.load();
        for (int bwi : {500, 1000, 2000}) {
            if (want && want != bwi) continue;
            for (double fc = kLoHz + bwi * 0.47; fc <= kHiHz - bwi * 0.47; fc += kBinHz) {   // fc: the centre of the block
                bool okIn, okL, okR;
                const double sin = mean(fc - 0.45 * bwi, fc + 0.45 * bwi, okIn);
                if (!okIn || sin < 1.6 * med) continue;
                const double sl = mean(fc - 0.5 * bwi - 0.3 * bwi, fc - 0.5 * bwi - 0.06 * bwi, okL);
                const double sr = mean(fc + 0.5 * bwi + 0.06 * bwi, fc + 0.5 * bwi + 0.3 * bwi, okR);
                if (!okL && !okR) continue;
                const double sout = okL && okR ? 0.5 * (sl + sr) : okL ? sl : sr;
                double q[4], qmax = 0, qmin = 1e30;
                for (int j = 0; j < 4; j++) { bool ok; q[j] = mean(fc - 0.45 * bwi + j * 0.225 * bwi, fc - 0.45 * bwi + (j + 1) * 0.225 * bwi, ok); qmax = std::max(qmax, q[j]); qmin = std::min(qmin, q[j]); }
                if (qmin < 0.3 * qmax) continue;   // not flat
                const double c = sin / (sout + 1e-30);
                if (c > best.contrast) { best.bw = bwi; best.fc = fc; best.contrast = c; }
            }
        }
        return best;
    }

    // ------------------------------------------------------------ the synchroniser
    void frameAt(const std::vector<cf32>& y, int64_t base, int64_t start, cf32* X) {
        const int64_t s = start - base;
        for (int i = 0; i < mt63::kFft; i++) X[i] = y[(size_t)(s + i)] * win512[(size_t)i];
        fft512.forward(X);
    }

    // The last samples of the ring through the mixer and the decimator
    void rebuild(int bwi, double fcm, int64_t& j0, std::vector<cf32>& out, int nR) {
        const int d = 8000 / (2 * bwi);
        int64_t s = n8 - (int64_t)nR * d;
        s -= ((s % d) + d) % d;
        Decim dd;
        dd.configure(d, 0.37 * 2 * bwi / kFs, 32 * d + 1);
        out.clear();
        j0 = s / d;
        const double w = 2 * kPi * fcm / kFs;
        for (int64_t n = s; n < n8; n++) {
            const cf32 v = ring[(size_t)(n & (kRing - 1))];
            const cf32 m = v * std::polar(1.f, (float)(-std::fmod(w * (double)n, 2 * kPi)));
            cf32 o;
            if (dd.push(m, o)) out.push_back(o);
        }
    }

    struct Sync { bool ok = false; int b = 0, tau = 0; double M = 0, ratio = 0, q = 0, delta = 0; };

    Sync analyse(int bwi, double fcm, int bLo, int bHi) {
        Sync res;
        int64_t j0;
        std::vector<cf32> y;
        const int need = mt63::kSymSep * (kSyncSyms + 1) + mt63::kFft + 40;
        rebuild(bwi, fcm, j0, y, need);
        if ((int)y.size() < need - 8) return res;
        const int nb = bHi - bLo + 1;
        std::vector<double> M((size_t)kTimingPhases * nb, 0.0), A((size_t)kTimingPhases * nb, 0.0);
        std::vector<cf32> S((size_t)kTimingPhases * nb, cf32(0, 0));
        std::vector<std::vector<cf32>> X(kSyncSyms, std::vector<cf32>(mt63::kFft));
        for (int ti = 0; ti < kTimingPhases; ti++) {
            const int64_t t0 = 40 + ti * (mt63::kSymSep / kTimingPhases);
            for (int k = 0; k < kSyncSyms; k++) frameAt(y, j0, j0 + t0 + (int64_t)mt63::kSymSep * k, X[(size_t)k].data());
            for (int bi = 0; bi < nb; bi++) {
                const int b0 = bLo + bi;
                double m = 0, a = 0; cf32 s(0, 0);
                for (int k = 1; k < kSyncSyms; k++)
                    for (int i = 0; i < mt63::kCarriers; i++) {
                        const int kb = (b0 + mt63::kCarrSep * i) & (mt63::kFft - 1);
                        const std::complex<float> r = X[(size_t)k][(size_t)kb] * std::conj(X[(size_t)k - 1][(size_t)kb]) * std::polar(1.f, (float)(-2 * kPi * kb * mt63::kSymSep / mt63::kFft));
                        m += std::fabs(r.real()); a += std::abs(r); s += r * r / (std::abs(r) + 1e-20f);
                    }
                (void)m;
                M[(size_t)(ti * nb + bi)] = std::abs(s); A[(size_t)(ti * nb + bi)] = a; S[(size_t)(ti * nb + bi)] = s;
            }
        }
        size_t bi = 0;
        for (size_t i = 1; i < M.size(); i++) if (M[i] > M[bi]) bi = i;
        std::vector<double> perB;
        for (int b = 0; b < nb; b++) { double mx = 0; for (int ti = 0; ti < kTimingPhases; ti++) mx = std::max(mx, M[(size_t)(ti * nb + b)]); perB.push_back(mx); }
        std::sort(perB.begin(), perB.end());
        const double med = perB[perB.size() / 2];
        res.b = bLo + (int)(bi % (size_t)nb);
        res.tau = (int)(bi / (size_t)nb) * (mt63::kSymSep / kTimingPhases) + 40;
        res.M = M[bi]; res.ratio = M[bi] / (med + 1e-30); res.q = M[bi] / (A[bi] + 1e-30);
        res.delta = 0.5 * std::atan2(S[bi].imag(), S[bi].real()) / (2 * kPi * (mt63::kSymSep / (2.0 * bwi)));
        res.ok = true;
        return res;
    }

    bool trySync(const Cand& c) {
        const int bwi = c.bw;
        double fcm = c.fc;
        Sync s1 = analyse(bwi, fcm, kBnom - kBinSearch, kBnom + kBinSearch);
        if (!s1.ok || s1.q < 0.22) return false;
        Sync s2 = s1;
        for (int it = 0; it < 3; it++) {   // the fractional frequency error from the residual rotation, then the comb position again
            fcm += s2.delta;
            s2 = analyse(bwi, fcm, s2.b - 2, s2.b + 2);
            if (!s2.ok) return false;
            if (std::fabs(s2.delta) < 0.1 * 2.0 * bwi / mt63::kFft) break;
        }
        if (s2.q < 0.22) return false;
        // go live: the mixer continues from here, the decimator and frames from the rebuilt samples
        bw = bwi; dec = 8000 / (2 * bwi); tSym = mt63::symbolSec(bwi);
        fcMix = fcLock = fcm + s2.delta;
        int64_t j0;
        rebuild(bw, fcMix, j0, yr, mt63::kSymSep * (kSyncSyms + 1) + mt63::kFft + 40);
        yrBase = j0;
        dec8.configure(dec, 0.37 * 2 * bw / kFs, 32 * dec + 1);
        // keep the decimator state: feed it the last samples again (silently) so its history matches
        {
            const int64_t from = n8 - (int64_t)(32 * dec + 1 + dec);
            const int64_t s0 = from - ((from % dec) + dec) % dec;
            for (int64_t n = s0; n < n8; n++) {
                const cf32 m = ring[(size_t)(n & (kRing - 1))] * std::polar(1.f, (float)(-std::fmod(2 * kPi * fcMix / kFs * (double)n, 2 * kPi)));
                cf32 o; dec8.push(m, o);
            }
        }
        theta = std::fmod(2 * kPi * fcMix / kFs * (double)n8, 2 * kPi);
        // next frame start: phase of s2.tau relative to the rebuilt block, first one whose window is not complete yet
        const int64_t E = yrBase + (int64_t)yr.size();
        const int64_t ph = yrBase + s2.tau;
        int64_t st = ph + (int64_t)mt63::kSymSep * ((E - mt63::kFft - kEL - ph) / mt63::kSymSep);
        while (st + mt63::kFft + kEL <= E) st += mt63::kSymSep;
        nextStart = st;
        combs.clear();
        for (int off : {0, 4, -4}) { Comb c2; c2.setup(((s2.b + off) % mt63::kFft + mt63::kFft) % mt63::kFft); combs.push_back(std::move(c2)); }
        // the previous symbol's carriers for the differential step
        for (Comb& cb : combs) primePrev(cb);
        locked = true; verified = false; symSinceLock = 0; lowCount = 0; tauAcc = 0; selIntl = 0;
        snrEma = 0; carrPow.fill(0);
        syncs++;
        tel.syncs = syncs;
        char b[160]; snprintf(b, sizeof b, "MT63-%d: synchronising near %.0f Hz (timing %d, comb %d, sync quality %.2f)", bw, c.fc, s2.tau, s2.b, s2.q);
        say(b);
        return true;
    }

    void primePrev(Comb& cb) {
        std::vector<cf32> X(mt63::kFft);
        const int os[3] = {-kEL, 0, kEL};
        for (int o = 0; o < 3; o++) {
            frameAt(yr, yrBase, nextStart - mt63::kSymSep + os[o], X.data());
            for (int i = 0; i < mt63::kCarriers; i++) cb.prev[(size_t)o][(size_t)i] = X[(size_t)cb.bin(i)];
        }
        cb.havePrev = true;
    }

    void unlock(const char* why) {
        if (locked) { char b[120]; snprintf(b, sizeof b, "MT63-%d: %s", bw, why); say(b); }
        locked = verified = false; combs.clear(); yr.clear();
        tel.state = 0; tel.bandwidth = 0; tel.interleave = 0;
        cooldownSec = 2.0;
        sinceSearch = 0;
    }

    // ------------------------------------------------------------ text
    void putChar(int ch) {
        chars++;
        if (ch == 10 && lastCh == 13) { lastCh = ch; return; }
        lastCh = ch;
        if (ch == 13 || ch == 10) {
            lineOpen = false;
            if (!text.empty() && text.back() != '\n') text += '\n';
        } else {
            if (ch == 9) ch = ' ';
            if (ch < 32 || ch > 126) return;
            if (!lineOpen) {
                Mt63Line l; l.timeSec = sigSec();
                lines.push_back(l);
                if (lines.size() > kMt63LinesMax) lines.erase(lines.begin());
                lineOpen = true;
            }
            lines.back().text += (char)ch;
            text += (char)ch;
        }
        if (text.size() > kMt63TextMax) text.erase(0, text.size() - kMt63TextMax);
        if (ch == ' ' || ch == 13 || ch == 10) {
            std::string flat = text;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            calls = cw::callsigns(flat);
        }
    }

    // ------------------------------------------------------------ one symbol
    void symbol() {
        std::vector<cf32> X(mt63::kFft);
        const int os[3] = {-kEL, 0, kEL};
        std::array<std::array<cf32, mt63::kCarriers>, 3> Z;
        double E[3] = {0, 0, 0};
        cf32 S(0, 0);
        double reSq = 0, imSq = 0;
        // the frames: three for the first comb (timing), one for the others
        for (int o = 0; o < 3; o++) {
            frameAt(yr, yrBase, nextStart + os[o], X.data());
            for (size_t c = 0; c < combs.size(); c++) {
                Comb& cb = combs[c];
                if (c > 0 && o != 1) continue;
                std::array<cf32, mt63::kCarriers> z;
                for (int i = 0; i < mt63::kCarriers; i++) z[(size_t)i] = X[(size_t)cb.bin(i)];
                float u[mt63::kCarriers];
                for (int i = 0; i < mt63::kCarriers; i++) {
                    const cf32 p = cb.prev[(size_t)o][(size_t)i];
                    const cf32 r = z[(size_t)i] * std::conj(p) * cb.rot[i];
                    const float den = 0.5f * (std::norm(z[(size_t)i]) + std::norm(p)) + 1e-20f;
                    u[i] = -std::max(-1.f, std::min(1.f, r.real() / den));
                    if (c == 0) {
                        E[o] += std::fabs(r.real());
                        if (o == 1) { S += r * r / (std::abs(r) + 1e-20f); reSq += (double)r.real() * r.real(); imSq += (double)r.imag() * r.imag(); }
                    }
                }
                cb.prev[(size_t)o] = z;
                if (o == 1) {
                    int code; float conf;
                    if (cb.dS.push(u, code, conf)) { cb.qS += 0.12f * (std::min(conf, 15.f) - cb.qS); if (c == 0) lastS = {code, conf}; }
                    if (cb.dL.push(u, code, conf)) { cb.qL += 0.12f * (std::min(conf, 15.f) - cb.qL); if (c == 0) lastL = {code, conf}; }
                    if (c == 0) for (int i = 0; i < mt63::kCarriers; i++) carrPow[(size_t)i] += 0.1f * (std::norm(z[(size_t)i]) - carrPow[(size_t)i]);
                }
            }
        }
        nextStart += mt63::kSymSep;
        symSinceLock++;
        // timing loop
        const float d = (float)((E[2] - E[0]) / (E[0] + E[2] + 1e-20));
        tauAcc += d;
        if (tauAcc > 0.12f) { nextStart += 1; tauAcc = 0; } else if (tauAcc < -0.12f) { nextStart -= 1; tauAcc = 0; }
        // frequency loop
        {
            const double phi = 0.5 * std::atan2((double)S.imag(), (double)S.real());
            double df = phi / (2 * kPi * tSym);
            df = std::max(-2.0, std::min(2.0, df));
            fcMix += 0.15 * df;
        }
        {
            const float snr = (float)(10 * std::log10(std::max(1e-3, reSq - imSq) / (imSq + 1e-20)));
            snrEma += 0.1f * (std::max(-10.f, std::min(30.f, snr)) - snrEma);
        }
        // which comb, which interleave
        if (!verified) {
            size_t bestC = 0; float bq = 0;
            for (size_t c = 0; c < combs.size(); c++) { const float q = std::max(combs[c].qS, combs[c].qL); if (q > bq) { bq = q; bestC = c; } }
            if (bq > kLockConf) {
                std::swap(combs[0], combs[bestC]);
                combs.resize(1);
                verified = true;
                char b[120]; snprintf(b, sizeof b, "MT63-%d: locked, Walsh quality %.1f", bw, bq);
                say(b);
            } else if (symSinceLock > 110) { unlock("no clean codewords, trying again"); return; }
            return;   // the characters of the first symbols are not shown (the interleaver is still filling)
        }
        Comb& cb = combs[0];
        const int forced = intlReq.load();
        int sel = selIntl;
        if (forced) sel = forced;
        else if (sel == 0) sel = cb.qL > cb.qS ? 2 : 1;
        else if (sel == 1 && cb.qL > cb.qS * 1.25f) sel = 2;
        else if (sel == 2 && cb.qS > cb.qL * 1.25f) sel = 1;
        selIntl = sel;
        const float q = sel == 2 ? cb.qL : cb.qS;
        const auto last = sel == 2 ? lastL : lastS;
        if (q < kDropConf) { if (++lowCount > 60) { unlock("signal lost"); return; } } else lowCount = 0;
        if (last.second > kCharConf) { if (last.first != 0) putChar(last.first); } else bad++;
        tel.quality = q; tel.qualityShort = cb.qS; tel.qualityLong = cb.qL;
    }
    std::pair<int, float> lastS{0, 0}, lastL{0, 0};

    // ------------------------------------------------------------ samples
    void process8(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[i];
            ring[(size_t)(n8 & (kRing - 1))] = v;
            fftIn.push_back(v);
            if (fftIn.size() >= (size_t)kN) { spectrumFrame(); fftIn.erase(fftIn.begin(), fftIn.begin() + kHop); }
            n8++;
            if (!locked) continue;
            const cf32 m = v * std::polar(1.f, (float)-theta);
            theta += 2 * kPi * fcMix / kFs;
            if (theta > 2 * kPi) theta -= 2 * kPi;
            cf32 o;
            if (dec8.push(m, o)) yr.push_back(o);
        }
        while (locked && nextStart + mt63::kFft + kEL < yrBase + (int64_t)yr.size()) {
            symbol();
            if (!locked) break;
        }
        if (locked && yr.size() > 16384) {
            const int64_t keep = nextStart - 2 * mt63::kSymSep - kEL - 4 - yrBase;
            if (keep > 0) { yr.erase(yr.begin(), yr.begin() + keep); yrBase += keep; }
        }
        if (!locked) {
            if (cooldownSec > 0) cooldownSec -= (double)n / kFs;
            else if (sinceSearch >= 8 && n8 > kRing / 2) {
                sinceSearch = 0;
                const Cand c = search();
                if (c.bw && c.contrast > 2.0) trySync(c);
            }
        }
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.floorDb = (float)(10 * std::log10(floorLin + 1e-20));
        if (accN > 0) {
            tel.spectrum.resize(acc.size());
            for (size_t i = 0; i < acc.size(); i++) { tel.spectrum[i] = (float)(10 * std::log10(acc[i] / accN + 1e-20)); acc[i] = 0; }
            accN = 0;
            tel.specSeq++;
        }
        tel.autoBandwidth = bwReq.load() == 0; tel.autoInterleave = intlReq.load() == 0;
        if (locked) {
            tel.bandwidth = bw;
            tel.state = verified ? 2 : 1;
            tel.interleave = verified ? selIntl : 0;
            const Comb& c0 = combs[0];
            tel.centreHz = fcMix + (c0.b0 - kBnom) * (2.0 * bw / mt63::kFft);
            tel.cfoHz = fcMix - fcLock;
            tel.snrDb = std::max(-5.f, snrEma);
            tel.dataValid = verified && (selIntl == 2 ? c0.qL : c0.qS) > kDropConf;
            tel.carrierDb.resize(mt63::kCarriers);
            float mx = 1e-20f;
            for (float p : carrPow) mx = std::max(mx, p);
            for (int i = 0; i < mt63::kCarriers; i++) tel.carrierDb[(size_t)i] = 10.f * std::log10(carrPow[(size_t)i] / mx + 1e-6f);
        } else {
            tel.state = 0; tel.dataValid = false; tel.carrierDb.clear(); tel.snrDb = 0;
        }
        tel.chars = chars; tel.blocksOk = chars; tel.blocksBad = bad;
        tel.text = text; tel.lines = lines; tel.calls = calls; tel.syncs = syncs;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

Mt63Receiver::Mt63Receiver() : p_(std::make_unique<Impl>()) {}
Mt63Receiver::~Mt63Receiver() = default;

void Mt63Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void Mt63Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool Mt63Receiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= mt63Tuning().minSampleRate - 1; }
void Mt63Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    const std::string t = p_->pub.text;   // the text stays (clearResults)
    auto l = std::move(p_->pub.lines);
    p_->pub = Mt63Telemetry();
    p_->pub.seq = s; p_->pub.text = t; p_->pub.lines = std::move(l);
}

void Mt63Receiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.front.setOffsetHz(m.offsetHz);
    }
    if (m.clearReq.exchange(false)) { m.text.clear(); m.lines.clear(); m.calls.clear(); m.lineOpen = false; m.chars = m.bad = 0; }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.listen.playBase(m.base.data(), m.base.size());
        std::vector<cf32> y8;
        y8.reserve(m.base.size() / 3 + 1);
        for (const cf32& v : m.base) { cf32 o; if (m.dec24.push(v, o)) y8.push_back(o); }
        m.process8(y8.data(), y8.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

void Mt63Receiver::clearResults() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.text.clear(); p_->pub.lines.clear(); p_->pub.calls.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
bool Mt63Receiver::telemetry(Mt63Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void Mt63Receiver::setListen(bool on) { p_->listen.on = on; }
bool Mt63Receiver::listening() const { return p_->listen.on.load(); }
void Mt63Receiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void Mt63Receiver::setMuted(bool m) { p_->listen.muted = m; }
void Mt63Receiver::setSilent(bool s) { p_->listen.silent = s; }
void Mt63Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void Mt63Receiver::setBandwidth(int hz) { p_->bwReq = (hz == 500 || hz == 1000 || hz == 2000) ? hz : 0; }
void Mt63Receiver::setInterleave(int i) { p_->intlReq = std::max(0, std::min(2, i)); }

ModeTuning mt63Tuning() {
    ModeTuning t;
    t.stdMode = 97; t.id = "mt63"; t.name = "MT63";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.1095;   // a usual 20 m MT63 dial frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as PSK and HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const Mt63Preset* mt63Presets(int& count) {
    static const Mt63Preset kP[] = {{"80 m", 3.583}, {"40 m", 7.0775}, {"20 m", 14.1095}, {"17 m", 18.1025}, {"15 m", 21.1095}, {"10 m", 28.1095}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
