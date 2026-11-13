// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell Waverider buoy HF link monitor (see datawell_rx.h).
#include "dect2/datawell/datawell_rx.h"
#include "dect2/datawell/datawell_tel.h"
#include "dect2/datawell/datawell_frames.h"
#include "dect2/datawell/datawell_hxv.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kFftN = 2048, kHop = 1024;
constexpr int kHyp = 17, kHypC = 8;       // sampling phases tried per symbol, the centre one
constexpr size_t kBitsKeep = 24000;
constexpr int kWinBits = 8192;

using cd = std::complex<double>;

std::vector<float> lowpass(int taps, double cutoffNorm) {   // cutoff as a fraction of the sample rate
    std::vector<float> h((size_t)taps);
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        const double x = i - 0.5 * (taps - 1);
        const double s = std::fabs(x) < 1e-9 ? 2 * cutoffNorm : std::sin(2 * kPi * cutoffNorm * x) / (kPi * x);
        h[(size_t)i] = (float)(s * (0.54 - 0.46 * std::cos(2 * kPi * i / (taps - 1))));
        sum += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

struct Fir {
    std::vector<float> h;
    std::vector<cf32> hist;
    size_t idx = 0;
    void init(std::vector<float> taps) { h = std::move(taps); hist.assign(h.size(), cf32(0, 0)); idx = 0; }
    void push(cf32 v) { idx = idx ? idx - 1 : h.size() - 1; hist[idx] = v; }   // newest at idx
    cf32 out() const {
        float re = 0, im = 0;
        size_t j = idx;
        for (size_t k = 0; k < h.size(); k++) { re += h[k] * hist[j].real(); im += h[k] * hist[j].imag(); if (++j == h.size()) j = 0; }
        return cf32(re, im);
    }
};
}

struct DatawellReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    DatawellTelemetry pub;
    double rateSet = 0, offsetSet = 0;
    std::atomic<double> centerHz{0};
    std::atomic<bool> resetReq{false}, clearReq{false};
    std::atomic<int> forced{0};

    // ---- front end (analysis thread)
    double rate = 0, offset = 0, rA = 0, rB = 0;
    cd mixPh{1, 0}, mixRot{1, 0};
    int mixN = 0;
    int dA1 = 1, dA2 = 1, c1 = 0, c2 = 0;
    cd acc1{0, 0}, acc2{0, 0};
    int64_t nIn = 0, nWide = 0;
    double nextReport = 0;
    double power = 0; int64_t nPower = 0;
    // ---- wide detector
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fw;           // the last kFftN samples, circular
    int fwPos = 0, fwNew = 0;
    std::vector<float> psd;         // averaged power per bin
    bool psdInit = false;
    int present = 0, absent = 0;
    bool found = false;
    double fc = 0, bw90 = 0, peakDb = 0, snr100 = 0;
    int cls = 0;                    // 0 tone, 1 2FSK, 2 4MFSK
    int curCls = -1;
    // ---- narrow chain
    cd nPh{1, 0};
    double fcUsed = 0;
    Fir f4, flp;
    int dec4 = 0;
    cf32 zPrev{0, 0};
    std::vector<double> cum;
    int64_t nB = 0;
    // ---- symbols
    int nl = 2;
    double baudNom = 0, T = 0, Tnom = 0, symEnd = 0, sp = 65;
    double lev[4] = {};
    double err[kHyp] = {};
    int kcur = kHypC;
    double q = 0.25;
    bool locked = false;
    int64_t lockLostAt = -1;
    uint64_t symbols = 0;
    std::deque<float> symMeans;
    // ---- bits and frames
    datawell::HxvDecoder hxv;       // 2FSK: the real-time format of the DWR-MkIII family
    std::vector<uint8_t> bitsAll;
    int64_t absBase = 0;            // absolute index of bitsAll[0]
    double nextAnalysis = 0;
    int period = 0; double periodZ = 0;
    datawell::Sync sync;
    int64_t originMod = -1;         // (absolute sync start) mod period
    int64_t nextFrame = -1;
    datawell::CrcHit hit;
    uint64_t framesOk = 0, framesBad = 0;
    std::deque<DwFrame> frames;
    std::vector<uint8_t> psdOut;

    double sigSec() const { return rate > 0 ? (double)nIn / rate : 0; }

    void initFront() {
        const double rA0 = 16000.0;
        const int DA = std::max(1, (int)std::lround(rate / rA0));
        dA1 = std::max(1, (int)std::ceil(std::sqrt((double)DA)));
        dA2 = std::max(1, (int)std::lround((double)DA / dA1));
        rA = rate / (dA1 * dA2);
        rB = rA / 4;
        fft = std::make_unique<Fft>(kFftN);
        win.resize(kFftN);
        for (int i = 0; i < kFftN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFftN));
        f4.init(lowpass(63, 600.0 / rA));
        flp.init(lowpass(65, 400.0 / rB));
        cum.assign(8192, 0.0);
        initOffset();
    }
    void initOffset() { offset = offsetSet; mixRot = std::polar(1.0, -2 * kPi * offset / std::max(1.0, rate)); mixPh = cd(1, 0); }

    void resetDetector() {
        mixPh = cd(1, 0); mixN = 0; c1 = c2 = 0; acc1 = acc2 = cd(0, 0);
        fw.assign(kFftN, cf32(0, 0)); fwPos = 0; fwNew = 0;
        psd.assign(kFftN, 0.f); psdInit = false;
        present = absent = 0; found = false; curCls = -1; cls = 0;
        resetDemod();
    }
    void resetDemod() {
        nPh = cd(1, 0); dec4 = 0; zPrev = cf32(0, 0);
        f4.init(f4.h); flp.init(flp.h);
        std::fill(cum.begin(), cum.end(), 0.0);
        nB = 0; locked = false; lockLostAt = -1; q = 0.25; kcur = kHypC;
        for (double& e : err) e = 0.25;
        symbols = 0; symMeans.clear();
        bitsAll.clear(); absBase = 0; period = 0; sync = datawell::Sync(); originMod = -1; nextFrame = -1; hit = datawell::CrcHit();
        nextAnalysis = sigSec() + 2;
        hxv.reset();
    }
    void clearAll() {
        resetDemod();
        frames.clear(); framesOk = framesBad = 0;
        std::lock_guard<std::mutex> lk(mu);
        pub.frames.clear(); pub.blocksOk = pub.blocksBad = 0; pub.bits.clear(); pub.seq++;
    }
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (offsetSet != offset) initOffset();
    }
    void startDemod(int c) {
        curCls = c;
        nl = c == 2 ? 4 : 2;
        baudNom = c == 2 ? datawell::kBaud4Mfsk : datawell::kBaud2Fsk;
        sp = c == 2 ? datawell::kSpacing4Mfsk : datawell::kShift2Fsk;
        Tnom = T = rB / baudNom;
        fcUsed = fc;
        for (int i = 0; i < nl; i++) lev[i] = fc + (i - 0.5 * (nl - 1)) * sp;
        symEnd = (double)nB + T;
        for (double& e : err) e = 0.25;
        kcur = kHypC; q = 0.25; locked = false; symbols = 0;
        bitsAll.clear(); absBase = 0; period = 0; sync = datawell::Sync(); originMod = -1; nextFrame = -1; hit = datawell::CrcHit();
        symMeans.clear();
        hxv.reset();
    }

    // ---- wide path: one sample at rA
    void wide(cf32 z) {
        nWide++;
        fw[(size_t)fwPos] = z;
        fwPos = (fwPos + 1) % kFftN;
        if (++fwNew >= kHop) { fwNew = 0; spectrum(); }
        if (!found || cls == 0 || curCls < 0) return;
        // narrow chain: mix to the signal centre, decimate by 4
        nPh *= std::polar(1.0, -2 * kPi * fcUsed / rA);
        if ((nWide & 1023) == 0) nPh /= std::abs(nPh);
        f4.push(cf32((float)(z.real() * nPh.real() - z.imag() * nPh.imag()), (float)(z.real() * nPh.imag() + z.imag() * nPh.real())));
        if (++dec4 < 4) return;
        dec4 = 0;
        narrow(f4.out());
    }

    void spectrum() {
        std::vector<cf32> b((size_t)kFftN);
        for (int i = 0; i < kFftN; i++) b[(size_t)i] = fw[(size_t)((fwPos + i) % kFftN)] * win[(size_t)i];
        fft->forward(b.data());
        const float a = psdInit ? 0.06f : 1.f;
        for (int k = 0; k < kFftN; k++) psd[(size_t)k] += a * (std::norm(b[(size_t)k]) / (float)kFftN - psd[(size_t)k]);
        psdInit = true;
    }

    // ---- narrow path: one sample at rB
    void narrow(cf32 z) {
        flp.push(z);
        const cf32 zf = flp.out();
        const cf32 p = zf * std::conj(zPrev);
        zPrev = zf;
        double d = std::atan2((double)p.imag(), (double)p.real()) * rB / (2 * kPi) + fcUsed;
        if (!std::isfinite(d)) d = fcUsed;
        const size_t mask = cum.size() - 1;
        cum[(size_t)(nB + 1) & mask] = cum[(size_t)nB & mask] + d;
        nB++;
        while ((double)nB >= symEnd + 0.5 * T + 1) symbol();
    }

    int nearest(double m, double& dist) const {
        int bi = 0; double bd = 1e18;
        for (int i = 0; i < nl; i++) { const double dd = std::fabs(m - lev[i]); if (dd < bd) { bd = dd; bi = i; } }
        dist = bd;
        return bi;
    }

    void symbol() {
        const size_t mask = cum.size() - 1;
        const double step = T / 16;
        double m[kHyp];
        for (int k = 0; k < kHyp; k++) {
            const double e = symEnd + (k - kHypC) * step;
            const int64_t b = (int64_t)std::llround(e), a = (int64_t)std::llround(e - T);
            m[k] = b > a ? (cum[(size_t)b & mask] - cum[(size_t)a & mask]) / (double)(b - a) : fcUsed;
            double dist;
            nearest(m[k], dist);
            err[k] += 0.05 * (dist * dist / (sp * sp) - err[k]);
        }
        int kb = kcur;
        for (int k = 0; k < kHyp; k++) if (err[k] < 0.85 * err[kb]) kb = k;
        kcur = kb;
        double dist;
        const int idx = nearest(m[kcur], dist);
        const double dn = dist * dist / (sp * sp);
        q += 0.03 * (std::min(dn, 4.0) - q);
        if (dist < 0.5 * sp) lev[idx] += 0.04 * (m[kcur] - lev[idx]);
        // the tones may not collapse or run away from the nominal spacing
        {
            const double spm = (lev[nl - 1] - lev[0]) / (nl - 1);
            if (spm < 0.6 * (nl == 4 ? datawell::kSpacing4Mfsk : datawell::kShift2Fsk) || spm > 1.5 * (nl == 4 ? datawell::kSpacing4Mfsk : datawell::kShift2Fsk)) {
                const double c0 = 0.5 * (lev[0] + lev[nl - 1]);
                const double s0 = nl == 4 ? datawell::kSpacing4Mfsk : datawell::kShift2Fsk;
                for (int i = 0; i < nl; i++) lev[i] = c0 + (i - 0.5 * (nl - 1)) * s0;
            }
            sp = (lev[nl - 1] - lev[0]) / (nl - 1);
        }
        symbols++;
        const double ctr = 0.5 * (lev[0] + lev[nl - 1]);
        symMeans.push_back((float)(m[kcur] - ctr));
        if (symMeans.size() > kDwSymMax) symMeans.pop_front();
        // clock loop
        const double es = (kcur - kHypC) * step;
        symEnd += T + 0.02 * es;
        T = std::max(Tnom * 0.98, std::min(Tnom * 1.02, T + 0.0003 * es));
        if (std::abs(kcur - kHypC) >= 4) {   // far from the middle: move the grid
            const int sh = kcur - kHypC;
            symEnd += sh * step;
            double e2[kHyp];
            for (int k = 0; k < kHyp; k++) e2[k] = err[std::max(0, std::min(kHyp - 1, k + sh))];
            std::memcpy(err, e2, sizeof err);
            kcur = kHypC;
        }
        // lock state
        const double r = std::sqrt(q);
        if (!locked && r < 0.28 && symbols > 48) locked = true;
        else if (locked && r > 0.45) locked = false;
        if (nl == 2) hxv.pushBit(idx);   // the parity code of the vectors tells good bits from bad: every symbol goes in
        if (locked) {
            lockLostAt = -1;
            if (nl == 2) bitsAll.push_back((uint8_t)idx);
            else { bitsAll.push_back((uint8_t)(idx >> 1)); bitsAll.push_back((uint8_t)(idx & 1)); }
            if (bitsAll.size() > kBitsKeep) { const size_t cut = bitsAll.size() - kBitsKeep + 4096; bitsAll.erase(bitsAll.begin(), bitsAll.begin() + (long)cut); absBase += (int64_t)cut; }
            fcUsed += 0.002 * (ctr - fcUsed);
        } else if (lockLostAt < 0) lockLostAt = nB;
        else if ((double)(nB - lockLostAt) > 3 * rB && !bitsAll.empty()) {   // lost for good: the old bits do not continue
            absBase += (int64_t)bitsAll.size(); bitsAll.clear(); period = 0; sync = datawell::Sync(); originMod = -1; nextFrame = -1; hit = datawell::CrcHit();
        }
    }

    // ---- every report: the detector, the frame analysis
    void detect() {
        if (psd.empty() || !psdInit) { found = false; return; }
        std::vector<float> tmp(psd);
        std::nth_element(tmp.begin(), tmp.begin() + kFftN / 2, tmp.end());
        const double floor = std::max(1e-20, (double)tmp[(size_t)kFftN / 2]);
        const double bin = rA / kFftN;
        auto fq = [&](int k) { return (k < kFftN / 2 ? k : k - kFftN) * bin; };
        std::vector<float> sm((size_t)kFftN);
        for (int k = 0; k < kFftN; k++) sm[(size_t)k] = (psd[(size_t)((k + kFftN - 1) % kFftN)] + psd[(size_t)k] + psd[(size_t)((k + 1) % kFftN)]) / 3.f;
        int kp = 0; float best = 0;
        for (int k = 0; k < kFftN; k++) if (std::fabs(fq(k)) < 0.82 * 0.5 * rA && sm[(size_t)k] > best) { best = sm[(size_t)k]; kp = k; }
        const double pk = 10 * std::log10(best / floor);
        peakDb = pk;
        // the cluster: down to twice the floor, bridging gaps of up to 3 bins (the tones of one signal)
        const double thr = 2.0 * floor;
        int lo = kp, hi = kp;
        for (int dir = -1; dir <= 1; dir += 2) {
            int k = kp, gap = 0, edge = kp;
            for (int s = 0; s < 400; s++) {
                k = (k + dir + kFftN) % kFftN;
                if (sm[(size_t)k] > thr) { edge = s + 1; gap = 0; } else if (++gap > 4) break;
            }
            if (dir < 0) lo = -edge; else hi = edge;
        }
        double tot = 0, wf = 0;
        std::vector<double> w;
        for (int o = lo; o <= hi; o++) {
            const int k = ((kp + o) % kFftN + kFftN) % kFftN;
            const double e = std::max(0.0, (double)psd[(size_t)k] - floor);
            w.push_back(e); tot += e; wf += e * (o * bin);
        }
        double c5 = 0, c95 = 0, acc = 0;
        for (size_t i = 0; i < w.size(); i++) {
            const double prev = acc; acc += w[i];
            const double fpos = ((double)lo + (double)i) * bin;
            if (prev < 0.05 * tot && acc >= 0.05 * tot) c5 = fpos;
            if (prev < 0.95 * tot && acc >= 0.95 * tot) c95 = fpos;
        }
        const bool ok = pk >= 8.0 && tot > 0;
        if (ok) present = std::min(present + 1, 100), absent = 0; else absent = std::min(absent + 1, 100), present = 0;
        if (!found && present >= 3) found = true;
        if (found && absent >= 12) found = false;
        if (!ok) { if (!found) { snr100 = 0; } return; }
        bw90 = std::max(bin, c95 - c5);
        const double cen = fq(kp) + wf / tot;
        snr100 = 10 * std::log10(std::max(1e-6, tot / (floor * 100.0 / bin)));
        int c = bw90 < 35 ? 0 : bw90 < 205 ? 1 : 2;   // measured: the sigidwiki 2FSK recording 161 Hz, 4FSK 248 Hz at 90% of the power; the synthetic ones about 85 and 280
        const int f = forced.load();
        if (f == 1) c = 1; else if (f == 2) c = 2;
        if (!found) { cls = c; fc = cen; return; }
        fc = cen;
        if (c != cls) cls = c;
        if (cls == 0) { curCls = -1; locked = false; return; }
        if (curCls != cls) startDemod(cls);
        if (!locked && std::fabs(fcUsed - fc) > 3) fcUsed += 0.3 * (fc - fcUsed);   // before the lock, follow the spectrum
    }

    std::vector<std::vector<uint8_t>> cutFrames(const std::vector<uint8_t>& win, int64_t winAbs, int64_t org, int L, int maxFrames) {
        std::vector<std::vector<uint8_t>> v;
        const int64_t first = winAbs + (((org - winAbs) % L) + L) % L;
        for (int64_t s = first; s + L <= winAbs + (int64_t)win.size(); s += L) v.emplace_back(win.begin() + (long)(s - winAbs), win.begin() + (long)(s - winAbs + L));
        if ((int)v.size() > maxFrames) v.erase(v.begin(), v.end() - maxFrames);
        return v;
    }

    void analyse() {
        if (nl == 2) return;   // 2FSK: the HXV decoder knows the format
        const int64_t n = (int64_t)bitsAll.size();
        if (n < 1500 || !locked) return;
        const int64_t w = std::min<int64_t>(n, kWinBits);
        std::vector<uint8_t> win(bitsAll.end() - w, bitsAll.end());
        const int64_t winAbs = absBase + n - w;
        const datawell::Period p = datawell::findPeriod(win, 32, 2048);
        if (p.lag == 0 || p.z < 9) { return; }
        if (p.lag != period) { period = p.lag; sync = datawell::Sync(); originMod = -1; nextFrame = -1; hit = datawell::CrcHit(); }
        periodZ = (float)p.z;
        // frames cut at the start of the window; the sync is the constant run among them
        const datawell::Sync s = datawell::findSync(win, period);
        if (!s.found) return;
        sync = s;
        const int64_t org = ((winAbs + s.start) % period + period) % period;
        if (org != originMod) { originMod = org; nextFrame = -1; hit = datawell::CrcHit(); }
        const auto fr = cutFrames(win, winAbs, org, period, 14);
        datawell::CrcHit h = datawell::bestCrc(fr, s.len);
        if (h.name.empty() && hit.specIdx >= 0) {
            int ok = 0;
            for (const auto& f : fr) ok += datawell::frameCrcOk(f, hit) ? 1 : 0;
            if (ok >= 3 && ok * 2 >= (int)fr.size()) { h = hit; h.pass = ok; h.total = (int)fr.size(); }
        }
        hit = h;
        // emit the frames that completed
        const int64_t endAbs = absBase + n;
        if (nextFrame < 0) {   // the frame boundary at or after the last six periods
            const int64_t base = std::max(winAbs, endAbs - 6 * (int64_t)period);
            nextFrame = base + (((org - base) % period) + period) % period;
        }
        while (nextFrame < absBase) nextFrame += period;
        while (nextFrame + period <= endAbs) {
            const size_t o = (size_t)(nextFrame - absBase);
            const std::vector<uint8_t> f(bitsAll.begin() + (long)o, bitsAll.begin() + (long)o + period);
            DwFrame d;
            d.wall = (int64_t)std::time(nullptr);
            d.hex = datawell::bitsToHex(f.data(), f.size(), hit.specIdx >= 0 && hit.lsbFirst, hit.specIdx >= 0 && hit.inverted);
            d.crcOk = hit.specIdx >= 0 && datawell::frameCrcOk(f, hit);
            if (hit.specIdx >= 0) { if (d.crcOk) framesOk++; else framesBad++; }
            frames.push_front(d);
            if (frames.size() > kDwFrameMax) frames.pop_back();
            nextFrame += period;
        }
    }

    void process(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            cf32 s = x[i];
            if (!std::isfinite(s.real()) || !std::isfinite(s.imag())) s = cf32(0, 0);
            mixPh *= mixRot;
            if ((++mixN & 4095) == 0) mixPh /= std::abs(mixPh);
            const cd y = cd(s.real(), s.imag()) * mixPh;
            power += (double)s.real() * s.real() + (double)s.imag() * s.imag();
            nPower++;
            acc1 += y;
            if (++c1 < dA1) continue;
            c1 = 0;
            acc2 += acc1 / (double)dA1; acc1 = cd(0, 0);
            if (++c2 < dA2) continue;
            c2 = 0;
            const cd o = acc2 / (double)dA2; acc2 = cd(0, 0);
            wide(cf32((float)o.real(), (float)o.imag()));
        }
    }

    void report() {
        detect();
        if (sigSec() >= nextAnalysis) { nextAnalysis = sigSec() + 1.5; analyse(); }
        std::lock_guard<std::mutex> lk(mu);
        DatawellTelemetry& t = pub;
        t.seq++;
        t.timeSec = sigSec(); t.inputRate = rate;
        const double cH = centerHz.load();
        t.centerMhz = cH / 1e6;
        const bool hx = found && nl == 2 && hxv.locked();
        t.state = found ? ((locked || hx) ? 2 : 1) : 0;
        t.dataValid = nl == 2 ? hx : locked;
        // the HXV real-time format
        t.hxvLocked = hx; t.hxvInverted = hx && hxv.inverted();
        t.hxvVectors = hxv.vectors(); t.hxvClean = hxv.clean(); t.hxvRepaired = hxv.repaired(); t.hxvLost = hxv.lost(); t.hxvBlocks = hxv.blocks();
        t.lastBlock = hxv.lastBlock();
        t.disp.clear();
        for (const auto& v : hxv.recent()) { DwDisp d; d.h = v.h; d.n = v.n; d.w = v.w; d.fixed = v.fixed; d.wrsg = v.wrsg; t.disp.push_back(d); }
        {
            const datawell::HxvSystem& s = hxv.system();
            DwSystem& o = t.sys;
            o.havePos = s.havePos; o.tn = s.tn; o.tolWeeks = s.tolWeeks; o.battery = s.battery; o.gpsFix = s.gpsFix; o.gpsModuleOk = s.gpsModuleOk;
            o.hrms = s.hrms; o.hs = s.hs; o.tz = s.tz; o.psdMax = s.psdMax; o.tr = s.tr; o.tw = s.tw; o.av0 = s.av0; o.lat = s.lat; o.lon = s.lon;
            o.orientDeg = s.orientDeg; o.inclDeg = s.inclDeg;
            for (int i = 0; i < 16; i++) o.have[i] = s.have[i];
            t.specCount = 0;
            for (int i = 0; i < 64; i++) {
                const datawell::HxvSpecPoint& p = hxv.spectrum()[i];
                DwSpec& q = t.spec[i];
                q.valid = p.valid; q.hz = p.hz; q.dirDeg = p.dirDeg; q.spreadDeg = p.spreadDeg; q.m2 = p.m2; q.n2 = p.n2; q.k = p.k; q.rpsd = p.rpsd; q.psd = p.psd;
                if (p.valid) t.specCount++;
            }
        }
        t.snrDb = found ? (float)snr100 : 0.f;
        t.peakDb = found ? (float)peakDb : 0.f;
        t.bwHz = found ? bw90 : 0;
        t.offsetHz = found ? offset + fc : 0;
        t.freqMhz = (found && cH > 0) ? (cH + offset + fc) / 1e6 : 0;
        t.channel = t.freqMhz > 0 ? datawell::channelNear(t.freqMhz) : std::string();
        t.formatForced = forced.load() != 0;
        t.format = !found ? "" : cls == 0 ? "carrier / narrow tone" : cls == 1 ? "2FSK" : "4MFSK";
        t.baud = (found && curCls >= 0 && T > 0) ? rB / T : 0;
        t.clockPpm = (found && curCls >= 0 && T > 0) ? (Tnom / T - 1) * 1e6 : 0;
        t.stepHz = (found && curCls >= 0) ? sp : 0;
        t.symErr = (float)std::sqrt(q);
        t.symbols = symbols;
        t.bits.clear();
        if (locked || !bitsAll.empty()) {
            const size_t m = std::min(bitsAll.size(), (size_t)kDwBitsShown);
            for (size_t i = bitsAll.size() - m; i < bitsAll.size(); i++) t.bits.push_back(bitsAll[i] ? '1' : '0');
        }
        t.periodBits = period;
        t.periodSec = (period > 0 && baudNom > 0) ? period / (baudNom * (nl == 4 ? 2 : 1)) : 0;
        t.periodZ = (float)periodZ;
        t.syncHex = sync.found ? sync.hex : std::string();
        t.syncBits = sync.found ? sync.len : 0;
        t.crcName = hit.specIdx >= 0 ? hit.name : std::string();
        t.crcPass = hit.pass; t.crcTotal = hit.total;
        t.crcInverted = hit.inverted; t.crcLsbFirst = hit.lsbFirst;
        t.blocksOk = framesOk; t.blocksBad = framesBad;
        t.frames.assign(frames.begin(), frames.end());
        t.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        // the spectrum plot: kDwPsd columns across 3 kHz around the signal (the radio's centre when nothing is found)
        t.psdSpanHz = 3000;
        t.psd.assign(kDwPsd, 0);
        if (psdInit && !psd.empty()) {
            std::vector<float> tmp(psd);
            std::nth_element(tmp.begin(), tmp.begin() + kFftN / 2, tmp.end());
            const double floor = std::max(1e-20, (double)tmp[(size_t)kFftN / 2]);
            const double bin = rA / kFftN, centre = found ? fc : 0;
            for (size_t c = 0; c < kDwPsd; c++) {
                const double f0 = centre + ((double)c / kDwPsd - 0.5) * t.psdSpanHz, f1 = centre + ((double)(c + 1) / kDwPsd - 0.5) * t.psdSpanHz;
                float mx = 0;
                for (int k = (int)std::floor(f0 / bin); k <= (int)std::floor(f1 / bin); k++) mx = std::max(mx, psd[(size_t)(((k % kFftN) + kFftN) % kFftN)]);
                const double db = 10 * std::log10(std::max(1e-3, mx / floor));
                t.psd[c] = (uint8_t)std::max(0.0, std::min(255.0, db * (255.0 / 40.0)));
            }
        }
        t.symMeans.assign(symMeans.begin(), symMeans.end());
    }
};

DatawellReceiver::DatawellReceiver() : p_(new Impl) {}
DatawellReceiver::~DatawellReceiver() = default;

void DatawellReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void DatawellReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetSet = hz; }
void DatawellReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
void DatawellReceiver::setFormat(int f) { p_->forced = std::max(0, std::min(2, f)); }
bool DatawellReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= datawellTuning().minSampleRate - 1; }
void DatawellReceiver::reset() { p_->resetReq = true; }
void DatawellReceiver::clearResults() { p_->clearReq = true; }

void DatawellReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.rate = m.rateSet;
        m.nIn = 0; m.nextReport = 0;
        if (m.rate >= datawellTuning().minSampleRate - 1) { m.initFront(); m.resetDetector(); }
    }
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.rate < datawellTuning().minSampleRate - 1 || !m.fft || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    const size_t step = (size_t)(0.05 * m.rate);
    size_t pos = 0;
    while (pos < n) {
        const size_t c = std::min(step, n - pos);
        m.process(x + pos, c);
        m.nIn += (int64_t)c;
        pos += c;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

bool DatawellReceiver::telemetry(DatawellTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void DatawellReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

namespace datawell {
// sigidwiki.com/wiki/Datawell_Buoy_HF_Link, "Channel frequencies"
static const Channel kChannels[] = {
    {"English Channel, UK", 35.3875}, {"Lyme Bay, UK", 35.2875}, {"South UK", 34.7375}, {"Minehead, Bristol Channel, UK", 31.9275},
    {"Weston, UK", 31.2290}, {"Default", 30.4750}, {"Australia", 29.9250}, {"Morocco", 29.5250},
};
const Channel* channels(int& count) { count = (int)(sizeof kChannels / sizeof kChannels[0]); return kChannels; }
std::string channelNear(double mhz, double tolKhz) {
    for (const Channel& c : kChannels) if (std::fabs(mhz - c.mhz) * 1000 <= tolKhz) return c.name;
    return std::string();
}
} // namespace datawell

ModeTuning datawellTuning() {
    ModeTuning t;
    t.stdMode = 76; t.id = "datawell"; t.name = "Datawell Buoy HF Link";
    t.minMhz = 25.5; t.maxMhz = 45; t.defMhz = 30.475;
    t.sampleRate = 250000;                   // the signal is 200 - 400 Hz wide; 250 ksps leaves room around it for a drifting buoy
    t.basebandHz = 0;
    t.bandwidthMhz = 0.25;
    t.minSampleRate = 24000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
