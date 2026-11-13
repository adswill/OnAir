// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo receiver (see cquam_rx.h): the channel mixed down to 48 kHz, a carrier loop that brings the C-QUAM vector (1 + L + R) + j (L - R)
// onto the real and imaginary axes, the 25 Hz pilot found in the quadrature part, stereo or mono audio on the sound card.
#include "dect2/cquam/cquam_rx.h"
#include "dect2/cquam/cquam_tel.h"
#include "dect2/audio/audioout.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/qo100/qo100_dsp.h"
#include "../hfdig/hfdig_usb.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kFs = kCquamAudioRate;
constexpr int kNoiseN = 512;
constexpr int kNoiseAvg = 8;
constexpr double kMedianToMean = 0.959;     // median / mean of an average of 8 exponential bins
constexpr int kPilotDecim = 48;             // the quadrature channel goes to 1 kHz for the pilot analysis
constexpr int kPilotWin = 1000;             // one second
constexpr int kPilotHop = 500;
constexpr double kWideLoopHz = 30.0;        // natural frequency of the carrier loop while it pulls in
constexpr double kNarrowLoopHz = 4.0;       // and once locked: well under the 25 Hz pilot
constexpr double kPilotRatioDb = 12.0;      // the pilot bin over the mean of the four bins beside it
constexpr double kPilotMinAmp = 0.01;       // and at least 1 % of the carrier
constexpr double kSquelchOnDb = 4.0, kSquelchOffDb = 2.5;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void highpass(double fc, double fs) {   // 2nd order Butterworth
        const double w = std::tan(kPi * fc / fs), k = std::sqrt(2.0);
        const double n = 1.0 / (1 + k * w + w * w);
        b0 = n; b1 = -2 * n; b2 = n;
        a1 = 2 * (w * w - 1) * n; a2 = (1 - k * w + w * w) * n;
        z1 = z2 = 0;
    }
    float push(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
};

}  // namespace

struct CquamReceiver::Impl {
    std::mutex mu;
    double rate = 0, curRate = 0;
    std::atomic<double> sigOff{0}, chanOff{0}, width{5000.0};
    std::atomic<int> stereoMode{0};
    std::atomic<bool> sqOn{true}, cfgDirty{false}, resetReq{false}, silent{false}, muted{false};
    std::atomic<float> volume{0.8f};
    std::function<void(const std::string&)> logCb;
    std::function<void(const float*, const float*, size_t)> tap;
    bool built = false;

    // front end
    qo100::Nco nco; double mixHz = 1e300;
    qo100::Decimator dec;
    ExactResampler rs;
    std::vector<cf32> clean, decd, base;
    std::vector<float> taps; std::vector<cf32> hist; int hpos = 0; double firWidth = 0;
    // noise
    std::unique_ptr<Fft> fft; std::vector<float> win; double wsum2 = 0;
    std::vector<cf32> fbuf; std::vector<double> acc; int accN = 0; double noisePsd = -1;
    // carrier loop
    double theta = 0, omega = 0, kp = 0, ki = 0, loopHz = 0;
    cf32 prevZ{0, 0}; std::complex<double> cohSum{0, 0}; double cohPow = 0; int cohN = 0;
    double pr = 0, pm = 0, ps = 0, carA = 0, envA = 0;
    double goodSec = 0, badSec = 0; bool locked = false;
    // audio
    Biquad hpL, hpR;
    double gStereo = 0, gSquelch = 0; bool sqOpen = false; double sqHang = 0;
    double lPow = 0, rPow = 0; int lrN = 0;
    double qPow = 0, mPow = 0; double widthSm = -200;
    float modPeak = 0; int modN = 0; float modOut = 0;
    std::vector<float> pcm, tapL, tapR;
    std::unique_ptr<AudioOut> out;
    // pilot
    int pdAcc = 0; double pdSum = 0;
    std::vector<float> p1k; int p1kFill = 0, hopCnt = 0;
    std::vector<double> hann; double hannSum = 0;
    bool pilotSeen = false, stereoNow = false; int hits = 0, misses = 0;
    double pilotRatioDb = -200, pilotAmp = 0;
    float qSpec[kCquamPilotBins] = {};
    uint64_t okBlocks = 0, badBlocks = 0;
    // telemetry
    CquamTelemetry pub; double pubAcc = 0, sigSec = 0; double lvlDbFs = -200, snrDb = 0, noiseDb = -200;
    float leftDb = -200, rightDb = -200;
    double cosPhi = 1;

    void log(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = logCb; } if (cb) cb(s); }

    void setLoop(double hz) {
        loopHz = hz;
        const double wn = 2 * kPi * hz / kFs, zeta = 0.707;
        kp = 2 * zeta * wn; ki = wn * wn;
    }

    void designFir() {
        firWidth = std::clamp(width.load(), 3000.0, 10000.0);
        const int T = 191;
        const auto lp = hfdig::lowpass(T, firWidth / kFs, 7.0);
        taps.assign((size_t)T, 0.f);
        for (int k = 0; k < T; k++) taps[(size_t)(T - 1 - k)] = (float)lp[(size_t)k];
        hist.assign(2 * (size_t)T, cf32(0, 0));
        hpos = 0;
        acc.assign(kNoiseN, 0.0); accN = 0; fbuf.clear(); noisePsd = -1;
    }

    void build() {
        built = false;
        if (curRate <= 0) return;
        dec.configure(curRate, 26000.0, kFs * 2.0);
        if (!rs.configure(dec.outRate(), kFs)) return;
        fft = std::make_unique<Fft>(kNoiseN);
        win.resize(kNoiseN); wsum2 = 0;
        for (int i = 0; i < kNoiseN; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kNoiseN)); wsum2 += (double)win[(size_t)i] * win[(size_t)i]; }
        hann.resize(kPilotWin); hannSum = 0;
        for (int i = 0; i < kPilotWin; i++) { hann[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kPilotWin); hannSum += hann[(size_t)i]; }
        p1k.assign(kPilotWin, 0.f);
        hpL.highpass(50.0, kFs); hpR.highpass(50.0, kFs);
        designFir();
        built = true;
        clearState();
    }

    void clearState() {
        nco.reset(); dec.reset(); rs.reset(); mixHz = 1e300;
        std::fill(hist.begin(), hist.end(), cf32(0, 0)); hpos = 0;
        acc.assign(kNoiseN, 0.0); accN = 0; fbuf.clear(); noisePsd = -1;
        theta = 0; omega = 0; setLoop(kWideLoopHz);
        prevZ = cf32(0, 0); cohSum = {0, 0}; cohPow = 0; cohN = 0;
        pr = pm = ps = carA = envA = 0; goodSec = badSec = 0; locked = false;
        hpL.highpass(50.0, kFs); hpR.highpass(50.0, kFs);
        gStereo = 0; gSquelch = 0; sqOpen = false; sqHang = 0;
        lPow = rPow = 0; lrN = 0; qPow = mPow = 0; widthSm = -200; modPeak = 0; modN = 0; modOut = 0;
        pdAcc = 0; pdSum = 0; std::fill(p1k.begin(), p1k.end(), 0.f); p1kFill = 0; hopCnt = 0;
        pilotSeen = stereoNow = false; hits = misses = 0; pilotRatioDb = -200; pilotAmp = 0;
        std::memset(qSpec, 0, sizeof qSpec);
        pubAcc = 0; lvlDbFs = -200; snrDb = 0; noiseDb = -200; leftDb = rightDb = -200; cosPhi = 1;
        if (out) out->flush();
    }

    void noiseFeed(const cf32* x, size_t n) {
        if (!fft) return;
        for (size_t i = 0; i < n; i++) {
            fbuf.push_back(x[i]);
            if ((int)fbuf.size() < kNoiseN) continue;
            for (int k = 0; k < kNoiseN; k++) fbuf[(size_t)k] *= win[(size_t)k];
            fft->forward(fbuf.data());
            for (int k = 0; k < kNoiseN; k++) acc[(size_t)k] += std::norm(fbuf[(size_t)k]);
            fbuf.clear();
            if (++accN < kNoiseAvg) continue;
            std::vector<double> inb;
            const double lo = firWidth + 800.0, hi = 19000.0;   // beside the channel filter, inside the clean part of the stream
            for (int k = 0; k < kNoiseN; k++) {
                const double f = std::fabs((k < kNoiseN / 2 ? k : k - kNoiseN) * kFs / kNoiseN);
                if (f >= lo && f <= hi) inb.push_back(acc[(size_t)k] / kNoiseAvg);
            }
            if (inb.size() >= 6) {
                std::nth_element(inb.begin(), inb.begin() + inb.size() / 2, inb.end());
                const double psd = inb[inb.size() / 2] / kMedianToMean / wsum2 / kFs;
                noisePsd = noisePsd < 0 ? psd : 0.7 * noisePsd + 0.3 * psd;
            }
            acc.assign(kNoiseN, 0.0); accN = 0;
        }
    }

    // the quadrature channel at 1 kHz: Goertzel at 0 .. 60 Hz over the last second, the pilot against the bins beside it
    void analysePilot() {
        // order the ring: p1k is a ring written at p1kFill
        std::vector<double> x((size_t)kPilotWin);
        double mean = 0;
        for (int i = 0; i < kPilotWin; i++) { x[(size_t)i] = p1k[(size_t)((p1kFill + i) % kPilotWin)]; mean += x[(size_t)i]; }
        mean /= kPilotWin;
        for (int i = 0; i < kPilotWin; i++) x[(size_t)i] = (x[(size_t)i] - mean) * hann[(size_t)i];
        double pw[kCquamPilotBins];
        for (int k = 0; k < kCquamPilotBins; k++) {
            const double w = 2 * kPi * k / kPilotWin, c = 2 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (int i = 0; i < kPilotWin; i++) { const double s = x[(size_t)i] + c * s1 - s2; s2 = s1; s1 = s; }
            pw[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
            qSpec[k] = (float)(10 * std::log10(4 * pw[k] / (hannSum * hannSum) + 1e-12));
        }
        const double ref = 0.25 * (pw[17] + pw[19] + pw[31] + pw[33]);
        pilotRatioDb = 10 * std::log10((pw[25] + 1e-30) / (ref + 1e-30));
        // the noise adds its own power to the bin: take it off the amplitude
        pilotAmp = 2 * std::sqrt(std::max(0.0, pw[25] - ref)) / hannSum;
        const bool hit = locked && pilotRatioDb >= kPilotRatioDb && pilotAmp >= kPilotMinAmp;
        if (locked) { if (hit) okBlocks++; else badBlocks++; }
        if (hit) { hits++; misses = 0; if (hits >= 2) stereoNow = true; }
        else { misses++; hits = 0; if (misses >= 3 || !locked) stereoNow = false; }
        pilotSeen = stereoNow;
    }

    void proc(cf32 z) {
        // channel filter
        const int T = (int)taps.size();
        hist[(size_t)hpos] = z; hist[(size_t)(hpos + T)] = z;
        if (++hpos >= T) hpos = 0;
        const cf32* w = &hist[(size_t)hpos];
        float ar = 0, ai = 0;
        for (int k = 0; k < T; k++) { ar += taps[(size_t)k] * w[k].real(); ai += taps[(size_t)k] * w[k].imag(); }
        z = cf32(ar, ai);
        const double dt = 1.0 / kFs;
        const double pnCh = noisePsd > 0 ? noisePsd * 2 * firWidth : -1;   // noise power in the channel filter

        // coarse carrier frequency while the loop is not locked: the mean phase step of the carrier-dominated signal
        if (!locked) {
            const std::complex<double> d = std::complex<double>(z.real(), z.imag()) * std::conj(std::complex<double>(prevZ.real(), prevZ.imag()));
            cohSum += d; cohPow += std::norm(std::complex<double>(z.real(), z.imag()));
            if (++cohN >= (int)(0.25 * kFs)) {
                if (std::abs(cohSum) > 0.35 * cohPow && cohPow > 0) {
                    const double target = std::arg(cohSum);
                    omega += 0.7 * (target - omega);    // pull the loop's own frequency there (the loop does the rest)
                }
                cohSum = {0, 0}; cohPow = 0; cohN = 0;
            }
        }
        prevZ = z;

        // carrier loop
        const std::complex<double> zd(z.real(), z.imag());
        const std::complex<double> y = zd * std::complex<double>(std::cos(theta), -std::sin(theta));
        const double err = std::atan2(y.imag(), y.real());
        omega += ki * err;
        omega = std::clamp(omega, -2 * kPi * 800.0 / kFs, 2 * kPi * 800.0 / kFs);
        theta += omega + kp * err;
        if (theta > kPi) theta -= 2 * kPi; else if (theta < -kPi) theta += 2 * kPi;

        // levels and lock
        const double kLp = 1 - std::exp(-dt / 0.1), kCar = 1 - std::exp(-dt / 0.4);
        pr += kLp * (y.real() - pr);
        pm += kLp * (std::abs(y) - pm);
        ps += kLp * (std::norm(y) - ps);
        cosPhi = pm > 1e-9 ? std::clamp(pr / pm, 0.0, 1.0) : 0.0;
        carA += kCar * (y.real() - carA);
        envA += kCar * (std::abs(y) - envA);
        const double carPow = carA > 0 ? carA * carA : 0;
        const double cn = pnCh > 0 ? ps / pnCh : 1e9;      // signal plus noise over noise
        const bool good = cn > 2.5 && cosPhi > 0.6 && pnCh > 0;
        if (good) { goodSec += dt; badSec = 0; } else { badSec += dt; goodSec = 0; }
        if (!locked && goodSec > 0.4) { locked = true; }
        if (locked && badSec > 0.5) { locked = false; setLoop(kWideLoopHz); stereoNow = false; hits = misses = 0; }
        if (locked && loopHz > kNarrowLoopHz + 0.1 && goodSec > 1.0) setLoop(kNarrowLoopHz);
        if (pnCh > 0) {
            lvlDbFs = carPow > 0 ? 10 * std::log10(carPow) : -200;
            const double c2n = locked && carPow > 0 ? carPow / pnCh : ps / pnCh;
            snrDb = 10 * std::log10(std::max(c2n, 1e-6));
            noiseDb = 10 * std::log10(pnCh);
        }

        // demodulation: the carrier is 1, the real part 1 + L + R, the imaginary part L - R
        double m, q;
        if (locked && carA > 1e-9) { m = y.real() / carA - 1; q = y.imag() / carA; }
        else if (envA > 1e-9) { m = std::abs(y) / envA - 1; q = 0; }
        else { m = q = 0; }
        m = std::clamp(m, -2.0, 2.0); q = std::clamp(q, -2.0, 2.0);

        // pilot: the quadrature channel at 1 kHz
        pdSum += q;
        if (++pdAcc >= kPilotDecim) {
            p1k[(size_t)p1kFill] = (float)(pdSum / kPilotDecim);
            p1kFill = (p1kFill + 1) % kPilotWin;
            pdAcc = 0; pdSum = 0;
            if (++hopCnt >= kPilotHop) { hopCnt = 0; analysePilot(); }
        }

        // stereo gain, squelch
        const int sm = stereoMode.load();
        const double gT = sm == 1 ? 0.0 : sm == 2 ? 1.0 : (stereoNow ? 1.0 : 0.0);
        gStereo += std::clamp(gT - gStereo, -dt / 0.04, dt / 0.04);
        bool want = !sqOn.load() || (locked && snrDb >= (sqOpen ? kSquelchOffDb : kSquelchOnDb));
        if (want) { sqOpen = true; sqHang = 0.15; } else if (sqOpen) { sqHang -= dt; if (sqHang <= 0) sqOpen = false; }
        gSquelch += std::clamp((sqOpen ? 1.0 : 0.0) - gSquelch, -dt / 0.005, dt / 0.005);

        const float lraw = (float)(0.5 * (m + gStereo * q)), rraw = (float)(0.5 * (m - gStereo * q));
        const float l = hpL.push(lraw) * (float)gSquelch, r = hpR.push(rraw) * (float)gSquelch;
        pcm.push_back(l); pcm.push_back(r);
        lPow += (double)l * l; rPow += (double)r * r; lrN++;
        qPow += (double)gStereo * q * q; mPow += (double)m * m;
        modPeak = std::max(modPeak, (float)std::fabs(m));
        if (++modN >= (int)kFs) { modOut = modPeak; modPeak = 0; modN = 0; }

        if (++pubCount >= (int)(0.1 * kFs)) { pubCount = 0; publish(); }
    }
    int pubCount = 0;

    void publish() {
        CquamTelemetry t;
        t.seq = pub.seq + 1;
        t.state = !built ? 0 : !locked ? 0 : stereoNow ? 2 : 1;
        t.cfoHz = omega * kFs / (2 * kPi);
        t.snrDb = (float)snrDb;
        t.levelDb = (float)lvlDbFs;
        t.dataValid = locked && noisePsd > 0;
        t.blocksOk = okBlocks; t.blocksBad = badBlocks;
        t.stereo = stereoNow && stereoMode.load() != 1;
        t.pilotSeen = pilotSeen;
        t.pilotDb = (float)pilotRatioDb;
        t.pilotPct = (float)(pilotAmp * 100);
        t.lockQuality = (float)cosPhi;
        if (lrN) {
            leftDb = (float)(10 * std::log10(lPow / lrN + 1e-20));
            rightDb = (float)(10 * std::log10(rPow / lrN + 1e-20));
        }
        lPow = rPow = 0; lrN = 0;
        t.leftDb = leftDb; t.rightDb = rightDb;
        if (mPow > 1e-9 && qPow > 0) widthSm = 10 * std::log10(qPow / mPow); else widthSm = -200;
        qPow = mPow = 0;
        t.widthDb = stereoNow ? (float)widthSm : -200.f;
        t.modPct = modOut * 100.f;
        t.mode = stereoMode.load();
        t.widthHz = firWidth;
        t.inputRate = curRate;
        sigSec += 0.1; t.timeSec = sigSec;
        std::memcpy(t.qSpecDb, qSpec, sizeof qSpec);
        t.noiseDb = (float)noiseDb;
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(t);
    }
};

CquamReceiver::CquamReceiver() : p_(std::make_unique<Impl>()) {}
CquamReceiver::~CquamReceiver() = default;

void CquamReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->cfgDirty = true; p_->resetReq = true; }
void CquamReceiver::setSignalOffset(double hz) { p_->sigOff = hz; }
bool CquamReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= cquamTuning().minSampleRate - 1; }
void CquamReceiver::reset() { p_->resetReq = true; }
void CquamReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void CquamReceiver::setChannelOffsetHz(double hz) { p_->chanOff = hz; }
void CquamReceiver::setStereoMode(int m) { p_->stereoMode = std::max(0, std::min(2, m)); }
void CquamReceiver::setWidthHz(double hz) { p_->width = std::clamp(hz, 3000.0, 10000.0); p_->cfgDirty = true; }
void CquamReceiver::setSquelch(bool on) { p_->sqOn = on; }
void CquamReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void CquamReceiver::setMuted(bool m) { p_->muted = m; }
void CquamReceiver::setSilent(bool s) { p_->silent = s; }
void CquamReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->tap = std::move(cb); }

bool CquamReceiver::telemetry(CquamTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void CquamReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.cfgDirty.exchange(false)) {
        double r;
        { std::lock_guard<std::mutex> lk(m.mu); r = m.rate; }
        if (!m.built || r != m.curRate) { m.curRate = r; m.build(); }
        else if (std::fabs(std::clamp(m.width.load(), 3000.0, 10000.0) - m.firWidth) > 0.5) m.designFir();
    }
    if (m.resetReq.exchange(false) && m.built) m.clearState();
    if (!m.built || n == 0) return;
    const double f = m.sigOff.load() + m.chanOff.load();
    if (f != m.mixHz) { m.mixHz = f; m.nco.set(f, m.curRate); }
    m.clean.resize(n);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.clean[i] = m.nco.mix(cf32(std::clamp(re, -2.f, 2.f), std::clamp(im, -2.f, 2.f)));
    }
    m.decd.clear();
    m.dec.process(m.clean.data(), n, m.decd);
    m.base.clear();
    m.rs.process(m.decd.data(), m.decd.size(), m.base);
    m.noiseFeed(m.base.data(), m.base.size());
    m.pcm.clear();
    for (const cf32 v : m.base) m.proc(v);
    const size_t frames = m.pcm.size() / 2;
    if (!frames) return;
    std::function<void(const float*, const float*, size_t)> tap;
    { std::lock_guard<std::mutex> lk(m.mu); tap = m.tap; }
    if (tap) {
        m.tapL.resize(frames); m.tapR.resize(frames);
        for (size_t i = 0; i < frames; i++) { m.tapL[i] = m.pcm[2 * i]; m.tapR[i] = m.pcm[2 * i + 1]; }
        tap(m.tapL.data(), m.tapR.data(), frames);
    }
    if (m.silent.load()) return;
    if (!m.out) { m.out = std::make_unique<AudioOut>(); m.out->start((int)kFs); m.out->setStartThreshold((int)kFs / 10); }
    m.out->setVolume(m.volume.load()); m.out->setMuted(m.muted.load());
    if (m.out->bufferedFrames() > (int)kFs / 2) m.out->flush();   // the radio's clock runs ahead of the sound card: catch up
    if (!m.out->playing()) m.out->setStartThreshold((int)kFs / 10);
    m.out->write(m.pcm.data(), (int)frames);
}

ModeTuning cquamTuning() {
    ModeTuning t;
    t.stdMode = 109; t.id = "cquam"; t.name = "AM stereo (C-QUAM)";
    t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = 0.999;   // medium wave (an HF / direct-sampling radio), the shortwave broadcast bands above it
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.02;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 250000;      // as the AM broadcast entry: the radio sits above the dial, away from its DC spike
    return t;
}

const CquamPreset* cquamPresets(int& count) {
    static const CquamPreset kP[] = {{"MW 531", 0.531}, {"MW 810", 0.810}, {"MW 999", 0.999}, {"MW 1170", 1.170}, {"MW 1368", 1.368}, {"MW 1570", 1.570}, {"MW 1602", 1.602}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
