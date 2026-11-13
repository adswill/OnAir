// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum analyser receiver: wide sweeps put together from FFT snapshots, either from the HackRF's own sweep (blocks tagged with their
// tuning, through SaSweepTap) or by stepping the radio over the range. See specan_rx.h.
#include "dect2/specan/specan_rx.h"
#include "dect2/specan/specan_dsp.h"
#include "dect2/specan/specan_tel.h"
#include "dect2/specan/specan_res.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dect2 {

struct SpecanReceiver::Impl {
    std::mutex mu;                       // settings and telemetry
    double rate = 0, centre = 100e6;
    double lo = 1e6, hi = 6000e6, binHz = 1e5;
    int avgN = 8;
    bool spurs = true;
    bool dirty = true, peakReset = false;
    std::function<void(const std::string&)> logCb;
    std::function<bool(double)> retuneCb;

    // the sweep (feed thread)
    SaStitcher st;
    SaTraces tr;
    int nfft = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win, powAcc;
    std::vector<cf32> buf;
    double winSum = 1;
    bool native = false;
    double lastLo = -1;
    std::vector<double> plan;            // stepped: the radio's centres
    size_t step = 0;
    size_t settle = 0, frames = 0;
    double stepLo = 0;
    std::chrono::steady_clock::time_point sweepT0 = std::chrono::steady_clock::now();
    float sweepMs = 0;
    uint64_t seq = 0, sweeps = 0, gaps = 0;
    SpecanTelemetry tel;
    bool haveTel = false;

    static constexpr int kFrames = 4;    // FFTs averaged per step

    void log(const std::string& s) { if (logCb) logCb(s); }

    void setup(bool nat) {               // under mu
        native = nat;
        const double r = native ? 20e6 : rate;
        if (r <= 0) return;
        const int n = saPickRes(binHz, rate, native).nfft;
        nfft = n;
        fft = std::make_unique<Fft>(n);
        win.resize(n);
        winSum = 0;
        for (int i = 0; i < n; i++) { win[i] = 0.5f - 0.5f * std::cos(2 * M_PI * (i + 0.5) / n); winSum += win[i]; }
        powAcc.assign(n, 0.f);
        double glo = lo, ghi = hi;
        if (!native && !retuneCb) { glo = centre - rate * 0.5; ghi = centre + rate * 0.5; }   // a file: the band it holds
        st.setGrid(saMakeGrid(glo, ghi, std::max(binHz, r / n)));
        tr.reset();
        tr.avgN = avgN;
        plan.clear();
        if (!native) {
            if (retuneCb) {
                const double span = rate * 0.75;
                for (double c = lo + span / 2; c - span / 2 < hi; c += span) plan.push_back(c);
                if (plan.empty()) plan.push_back((lo + hi) / 2);
            } else plan.push_back(centre);
        }
        step = 0;
        startStep();
        st.beginSweep();
        lastLo = -1;
        sweepT0 = std::chrono::steady_clock::now();
        dirty = false;
    }

    void startStep() {
        frames = 0;
        std::fill(powAcc.begin(), powAcc.end(), 0.f);
        buf.clear();
        if (native || plan.empty()) return;
        stepLo = plan[step];
        if (retuneCb && plan.size() > 1) { retuneCb(stepLo); settle = (size_t)(rate * 0.004) + 32768; }   // the PLL and the samples on their way
        else settle = 0;
    }

    void spectrum(const cf32* x, double r, double loHz, bool addNow, double usable, double gap) {
        std::vector<cf32> v((size_t)nfft);
        for (int i = 0; i < nfft; i++) v[(size_t)i] = x[i] * win[(size_t)i];
        fft->forward(v.data());
        const float k = (float)(1.0 / (winSum * winSum));
        for (int i = 0; i < nfft; i++) powAcc[(size_t)((i + nfft / 2) % nfft)] += std::norm(v[(size_t)i]) * k;
        frames++;
        if (!addNow) return;
        for (auto& p : powAcc) p /= (float)frames;
        st.add(loHz, r, powAcc.data(), (size_t)nfft, usable, gap);
        frames = 0;
        std::fill(powAcc.begin(), powAcc.end(), 0.f);
    }

    void finishSweep() {
        std::vector<float> db = st.finishDb();
        const SaGrid& g = st.grid();
        const size_t cov = st.covered();
        if (spurs) {
            const double fb = (native ? 20e6 : rate) / nfft;
            saRemoveSpurs(db, g, st.stepLos(), std::max(g.binHz, 3 * fb));
        }
        tr.update(db);
        const auto now = std::chrono::steady_clock::now();
        sweepMs = (float)std::chrono::duration<double, std::milli>(now - sweepT0).count();
        sweepT0 = now;
        sweeps++;
        if (cov < g.bins) gaps++;
        size_t pk = 0;
        for (size_t i = 0; i < db.size(); i++) if (db[i] > db[pk]) pk = i;
        std::lock_guard<std::mutex> lk(mu);
        SpecanTelemetry& t = tel;
        t.seq = ++seq;
        t.state = 2;
        t.dataValid = true;
        t.blocksOk = sweeps; t.blocksBad = gaps;
        t.inputRate = rate;
        t.native = native;
        t.steps = (int)st.stepLos().size();
        t.stepLos = st.stepLos();
        t.sweepMs = sweepMs;
        t.loHz = g.loHz; t.binHz = g.binHz;
        t.levelDb = db.empty() ? -200 : db[pk];
        t.peakHz = g.centre(pk);
        if (peakReset) { tr.resetPeak(); peakReset = false; }
        t.live = tr.live; t.peak = tr.peak; t.avg = tr.avg;
        haveTel = true;
        st.beginSweep();
    }

    void feedNative() {
        std::vector<SaBlock> blocks;
        if (!SaSweepTap::get().take(blocks)) return;
        for (const auto& b : blocks) {
            if ((int)b.iq.size() < nfft) continue;
            if (lastLo >= 0 && b.loHz < lastLo - 1e6) finishSweep();
            lastLo = b.loHz;
            // hackrf_sweep's split: 2.5 .. 7.5 MHz either side of the LO at 20 Msps (no DC, no filter edges); the interleaved steps fill the rest
            spectrum(b.iq.data() + (b.iq.size() - (size_t)nfft), b.rateHz, b.loHz, true, b.rateHz * 0.375, b.rateHz * 0.125);
        }
    }

    void feedStepped(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (settle) { const size_t d = std::min(settle, n - i); settle -= d; i += d; continue; }
            const size_t want = (size_t)nfft - buf.size();
            const size_t m = std::min(want, n - i);
            buf.insert(buf.end(), x + i, x + i + m);
            i += m;
            if (buf.size() < (size_t)nfft) break;
            const bool last = frames + 1 >= (size_t)kFrames;
            spectrum(buf.data(), rate, stepLo, last, rate * 0.375, 0);
            buf.clear();
            if (!last) continue;
            step++;
            if (step >= plan.size()) { step = 0; finishSweep(); }
            startStep();
        }
    }
};

SpecanReceiver::SpecanReceiver() : p_(std::make_unique<Impl>()) {}
SpecanReceiver::~SpecanReceiver() = default;

void SpecanReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->dirty = true; p_->tel = SpecanTelemetry(); p_->haveTel = false; }
void SpecanReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->centre) { p_->centre = hz; if (!p_->retuneCb) p_->dirty = true; } }
bool SpecanReceiver::ready() const { return p_->rate >= 1e6; }
void SpecanReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->dirty = true; }
void SpecanReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void SpecanReceiver::setRetuneCallback(std::function<bool(double)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->retuneCb = std::move(cb); p_->dirty = true; }
void SpecanReceiver::setRange(double lo, double hi) {
    if (hi < lo) std::swap(lo, hi);
    std::lock_guard<std::mutex> lk(p_->mu);
    if (lo != p_->lo || hi != p_->hi) { p_->lo = lo; p_->hi = std::max(hi, lo + 1e5); p_->dirty = true; }
}
void SpecanReceiver::range(double& lo, double& hi) const { std::lock_guard<std::mutex> lk(p_->mu); lo = p_->lo; hi = p_->hi; }
void SpecanReceiver::setBinHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->binHz) { p_->binHz = std::max(100.0, hz); p_->dirty = true; } }
void SpecanReceiver::setAveraging(int n) { std::lock_guard<std::mutex> lk(p_->mu); p_->avgN = std::max(1, n); p_->tr.avgN = p_->avgN; }
void SpecanReceiver::setRemoveSpurs(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->spurs = on; }
void SpecanReceiver::resetPeak() { std::lock_guard<std::mutex> lk(p_->mu); p_->peakReset = true; }

void SpecanReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    const bool nat = SaSweepTap::get().active();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.dirty || nat != s.native) s.setup(nat);
    }
    if (!s.fft) return;
    if (s.native) s.feedNative(); else s.feedStepped(x, n);
}

bool SpecanReceiver::telemetry(SpecanTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->haveTel || p_->tel.seq == lastSeq) return false;   // nothing until the first sweep is done
    out = p_->tel;
    return true;
}

ModeTuning specanTuning() {
    ModeTuning t;
    t.stdMode = 44; t.id = "specan"; t.name = "Spectrum analyser";
    t.minMhz = 1; t.maxMhz = 6000; t.defMhz = 100;
    t.sampleRate = 20e6;          // the HackRF's sweep runs at 20 Msps; other radios step at their own fastest rate
    t.basebandHz = 15e6;
    t.bandwidthMhz = 15;
    t.minSampleRate = 1e6;
    return t;
}

} // namespace dect2
