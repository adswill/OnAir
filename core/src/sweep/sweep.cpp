// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/sweep/sweep.h"
#include "dect2/automation/engine.h"
#include "dect2/common/threadpool.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/spectrum.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace dect2 {
namespace sweep {

namespace {
using Clock = std::chrono::steady_clock;
double wallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// inverse of the normal CDF, Abramowitz-Stegun 26.2.23 (error < 5e-4)
double normInv(double p) {
    if (p < 0.5) return -normInv(1 - p);
    const double t = std::sqrt(-2.0 * std::log(1.0 - p));
    return t - (2.515517 + 0.802853 * t + 0.010328 * t * t) / (1 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t);
}
// quantile p of the mean of K unit exponentials (Gamma(K, 1/K)), Wilson-Hilferty
double meanQuantile(double K, double p) {
    const double a = 1.0 / (9.0 * K), z = normInv(std::min(0.999, std::max(0.001, p)));
    const double v = 1 - a + z * std::sqrt(a);
    return v * v * v;
}
int log2i(int n) { int l = 0; while ((1 << l) < n) l++; return l; }
}

HopPlan planHops(const SweepConfig& c, double srcRate) {
    HopPlan p;
    p.rateHz = srcRate;
    const double want = p.rateHz / std::max(1.0, c.binHz);
    int l = (int)std::lround(std::log2(std::max(16.0, want)));
    l = std::min(18, std::max(4, l));
    p.fftSize = 1 << l;
    p.binHz = p.rateHz / p.fftSize;
    int u = (int)std::floor(p.fftSize * (1.0 - 2.0 * std::min(0.45, std::max(0.0, c.cropFrac))));
    u &= ~1;
    p.binsPerHop = std::max(2, u);
    const double span = std::max(0.0, c.stopHz - c.startHz);
    p.totalBins = (size_t)std::floor(span / p.binHz) + 1;
    const size_t hops = (p.totalBins + p.binsPerHop - 1) / p.binsPerHop;
    const double step = std::max(0.05, 1.0 - c.overlap) * p.fftSize;
    const double dwellSamples = std::max(1.0, c.dwellSec * p.rateHz);
    p.averages = std::max(1, (int)std::floor((dwellSamples - p.fftSize) / step) + 1);
    p.samplesPerHop = (size_t)(p.fftSize + (p.averages - 1) * step);
    for (size_t h = 0; h < hops; h++) p.centers.push_back(c.startHz + (h * (double)p.binsPerHop + p.binsPerHop / 2) * p.binHz);
    return p;
}

float noiseFloorDb(const float* db, size_t n, double percentile, int averages) {
    if (n == 0) return -150;
    const int K = std::max(1, averages);
    const double p = std::min(0.999, std::max(0.001, percentile / 100.0));
    std::vector<float> v(db, db + n);
    auto kth = [&](std::vector<float>& x, double frac) {
        const size_t k = std::min(x.size() - 1, (size_t)std::floor(frac * (x.size() - 1) + 0.5));
        std::nth_element(x.begin(), x.begin() + k, x.end());
        return x[k];
    };
    // 1. percentile of all bins; the percentile of the dB values is the percentile of the linear power, the bias of the average of K
    //    exponentially distributed values is corrected in linear terms
    double m = kth(v, p) - 10.0 * std::log10(meanQuantile(K, p));   // dB
    // 2. trim: drop what stands above the 99.5 % point of the noise and take the percentile of the rest (signals in the band no longer
    //    pull the estimate up), correcting for the 0.5 % that noise itself loses to the trim
    const double keep = 0.995;
    const double over = 10.0 * std::log10(meanQuantile(K, keep));
    for (int it = 0; it < 6; it++) {
        const float lim = (float)(m + over);
        std::vector<float> w;
        w.reserve(n);
        for (float x : v) if (x <= lim) w.push_back(x);
        if (w.size() < 4) break;
        const double nm = kth(w, p) - 10.0 * std::log10(meanQuantile(K, p * keep));
        const bool done = std::fabs(nm - m) < 0.02;
        m = nm;
        if (done) break;
    }
    return (float)m;
}

// Averaged power of one hop into p[0..fft), DC-centred, normalised so that a full-scale complex tone reads 1.0 on its peak bin.
static void hopPower(const cf32* x, const HopPlan& plan, const SweepConfig& c, const std::vector<float>& win, float winSum, std::vector<float>& out) {
    const int N = plan.fftSize, L = log2i(N);
    const double step = std::max(0.05, 1.0 - c.overlap) * N;
    const int K = plan.averages;
    const int chunks = std::min(K, 4);   // fixed split: the sum order (so the result) does not depend on the pool size
    std::vector<std::vector<float>> acc(chunks, std::vector<float>(N, 0.f));
    auto job = [&](size_t ci) {
        std::vector<float> re(N), im(N);
        for (int k = (int)ci; k < K; k += chunks) {
            const cf32* b = x + (size_t)(k * step);
            for (int i = 0; i < N; i++) { re[i] = b[i].real() * win[i]; im[i] = b[i].imag() * win[i]; }
            fftSplit(re.data(), im.data(), L, false);
            float* a = acc[ci].data();
            for (int i = 0; i < N; i++) a[i] += re[i] * re[i] + im[i] * im[i];
        }
    };
    // small hops: the pool costs more than it saves
    if (c.threads == 1 || chunks == 1 || (size_t)K * N < ((size_t)1 << 19)) for (int ci = 0; ci < chunks; ci++) job(ci);
    else ThreadPool::shared().parallelFor((size_t)chunks, job);
    const float norm = 1.0f / ((float)K * winSum * winSum);
    out.assign(N, 0.f);
    for (int ci = 0; ci < chunks; ci++) for (int i = 0; i < N; i++) out[i] += acc[ci][i];
    for (int i = 0; i < N; i++) out[i] *= norm;
    // DC-centre
    std::rotate(out.begin(), out.begin() + N / 2, out.end());
}

bool runSweep(SweepSource& src, const SweepConfig& c, Sweep& out, std::string& err, const std::atomic<bool>* cancel) {
    const auto t0 = Clock::now();
    const HopPlan plan = planHops(c, src.sampleRate());
    if (plan.centers.empty() || c.stopHz < c.startHz) { err = "empty range"; return false; }
    const int N = plan.fftSize, U = plan.binsPerHop;
    std::vector<float> win(N);
    double ws = 0;
    for (int i = 0; i < N; i++) { win[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); ws += win[i]; }
    Sweep s;
    s.startHz = c.startHz; s.binHz = plan.binHz; s.timeSec = wallNow(); s.rateHz = plan.rateHz;
    s.fftSize = N; s.averages = plan.averages; s.hops = (int)plan.centers.size(); s.binsPerHop = U;
    s.db.assign(plan.totalBins, -150.f);
    s.floorDb.assign(plan.totalBins, -150.f);
    std::vector<cf32> buf(plan.samplesPerHop);
    std::vector<float> pw, hopDb(U);
    std::vector<float> hopFloors;
    for (size_t h = 0; h < plan.centers.size(); h++) {
        if (cancel && cancel->load()) { err = "cancelled"; return false; }
        if (!src.retune(plan.centers[h], err)) return false;
        if (c.settleSec > 0) src.discard(c.settleSec);
        if (src.capture(buf.data(), buf.size()) < buf.size()) { err = "the source ended"; return false; }
        if (c.dcSkip) {
            cf32 m = 0;
            for (auto& v : buf) m += v;
            m /= (float)buf.size();
            for (auto& v : buf) v -= m;
        }
        hopPower(buf.data(), plan, c, win, (float)ws, pw);
        for (int j = 0; j < U; j++) hopDb[j] = spectrumDb(std::max(pw[N / 2 - U / 2 + j], 1e-14f));
        if (c.dcSkip && U > 8) {   // bin j=0 is index U/2: interpolate the DC bin and its neighbours from the bins two away
            const int d = U / 2;
            const float a = hopDb[d - 3], b = hopDb[d + 3];
            for (int k = -2; k <= 2; k++) hopDb[d + k] = a + (b - a) * (float)(k + 3) / 6.f;
        }
        const size_t g0 = h * (size_t)U;
        const size_t n = std::min<size_t>(U, plan.totalBins - g0);
        const float fl = noiseFloorDb(hopDb.data(), n, c.floorPercentile, plan.averages);
        hopFloors.push_back(fl);
        for (size_t j = 0; j < n; j++) { s.db[g0 + j] = hopDb[j]; s.floorDb[g0 + j] = fl; }
    }
    std::vector<float> hf = hopFloors;
    std::nth_element(hf.begin(), hf.begin() + hf.size() / 2, hf.end());
    s.floorOverallDb = hf[hf.size() / 2];
    s.signals = findSignals(s, c.thresholdDb);
    s.elapsedSec = std::chrono::duration<double>(Clock::now() - t0).count();
    out = std::move(s);
    return true;
}

bool runSchedule(SweepSource& src, const SweepConfig& c, const std::function<bool(const Sweep&)>& onSweep, std::string& err, const std::atomic<bool>* cancel) {
    int n = 0;
    auto last = Clock::now() - std::chrono::hours(1);
    while (!cancel || !cancel->load()) {
        const double since = std::chrono::duration<double>(Clock::now() - last).count();
        if (since < c.minIntervalSec) {   // sleep in slices so a cancel is quick
            const double wait = c.minIntervalSec - since;
            const auto until = Clock::now() + std::chrono::duration<double>(wait);
            while (Clock::now() < until && !(cancel && cancel->load())) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        last = Clock::now();
        Sweep s;
        if (!runSweep(src, c, s, err, cancel)) return err == "cancelled";
        if (!onSweep(s)) return true;
        if (c.maxSweeps > 0 && ++n >= c.maxSweeps) return true;
    }
    return true;
}

std::vector<Detection> findSignals(const Sweep& s, double thr) {
    std::vector<Detection> out;
    const size_t n = s.bins();
    size_t i = 0;
    while (i < n) {
        if (s.db[i] < s.floorDb[i] + thr) { i++; continue; }
        size_t a = i, b = i, gap = 0;
        for (size_t j = i; j < n; j++) {
            if (s.db[j] >= s.floorDb[j] + thr) { b = j; gap = 0; }
            else if (++gap > 1) break;
        }
        size_t pk = a;
        for (size_t j = a; j <= b; j++) if (s.db[j] > s.db[pk]) pk = j;
        Detection d;
        d.peakDb = s.db[pk];
        d.snrDb = s.db[pk] - s.floorDb[pk];
        d.bwHz = (double)(b - a + 1) * s.binHz;
        double f = s.freqOf(pk);
        if (b - a + 1 <= 4 && pk > 0 && pk + 1 < n) {   // parabolic peak interpolation on the dB values
            const double l = s.db[pk - 1], m = s.db[pk], r = s.db[pk + 1];
            const double den = l - 2 * m + r;
            if (den < 0) f += std::max(-0.5, std::min(0.5, 0.5 * (l - r) / den)) * s.binHz;
        } else {   // wide signal: power-weighted centre
            double sw = 0, sf = 0;
            for (size_t j = a; j <= b; j++) { const double w = std::pow(10.0, (s.db[j] - s.floorDb[j]) / 10.0) - 1.0; sw += w; sf += w * s.freqOf(j); }
            if (sw > 0) f = sf / sw;
        }
        d.freqHz = f;
        out.push_back(d);
        i = b + 1;
    }
    return out;
}

void SignalLog::update(const Sweep& s) {
    for (const auto& d : s.signals) {
        SignalRecord* hit = nullptr;
        for (auto& r : rec_) {
            const double tol = std::max(r.bwHz, d.bwHz) / 2 + 2 * s.binHz;
            if (std::fabs(r.freqHz - d.freqHz) <= tol && (!hit || std::fabs(r.freqHz - d.freqHz) < std::fabs(hit->freqHz - d.freqHz))) hit = &r;
        }
        if (!hit) {
            SignalRecord r;
            r.freqHz = d.freqHz; r.bwHz = d.bwHz; r.peakDb = d.peakDb; r.snrDb = r.maxSnrDb = d.snrDb;
            r.firstSeen = r.lastSeen = s.timeSec; r.count = 1;
            rec_.push_back(r);
            if (automation::wants(automation::FSignal)) {   // the automations (automation/engine.h): a signal that was not in the log before
                automation::Event ev("signal.new");
                ev.at(automation::wallNow(), "Sweep", r.freqHz);
                ev.num("bw_hz", r.bwHz).num("peak_db", r.peakDb).num("snr_db", r.snrDb);
                automation::emit(std::move(ev));
            }
        } else {
            hit->count++;
            hit->lastSeen = s.timeSec;
            hit->snrDb = d.snrDb;
            hit->peakDb = std::max(hit->peakDb, d.peakDb);
            hit->maxSnrDb = std::max(hit->maxSnrDb, d.snrDb);
            hit->bwHz = std::max(hit->bwHz, d.bwHz);
            hit->freqHz += (d.freqHz - hit->freqHz) / hit->count;   // running mean of the centre
        }
    }
    std::sort(rec_.begin(), rec_.end(), [](const SignalRecord& a, const SignalRecord& b) { return a.freqHz < b.freqHz; });
}

double calibrateSettle(SweepSource& src, double loHz, double hiHz, std::string& err) {
    const double rate = src.sampleRate();
    const size_t blk = 512;
    const int nb = std::max(8, (int)(0.080 * rate / blk));   // watch 80 ms
    double worst = 0;
    std::vector<cf32> b(blk);
    for (int rep = 0; rep < 6; rep++) {
        const double f = (rep & 1) ? hiHz : loHz;
        const auto t0 = Clock::now();
        if (!src.retune(f, err)) return -1;
        const double tune = std::chrono::duration<double>(Clock::now() - t0).count();
        std::vector<double> p(nb);
        for (int i = 0; i < nb; i++) {
            if (src.capture(b.data(), blk) < blk) { err = "the source ended"; return -1; }
            double e = 0;
            for (auto& v : b) e += std::norm(v);
            p[i] = e / blk;
        }
        // reference: the median power of the last third; the radio has settled once the blocks stay within 1.5 dB of it
        std::vector<double> tail(p.end() - nb / 3, p.end());
        std::nth_element(tail.begin(), tail.begin() + tail.size() / 2, tail.end());
        const double ref = std::max(tail[tail.size() / 2], 1e-14);
        int lastBad = -1;
        for (int i = 0; i < nb - nb / 3; i++) if (std::fabs(10 * std::log10(std::max(p[i], 1e-14) / ref)) > 1.5) lastBad = i;
        worst = std::max(worst, tune + (lastBad + 1) * (double)blk / rate);
    }
    return worst * 1.25 + 0.001;
}

// ---- radio adapter ----
IqSweepSource::IqSweepSource(const DeviceInfo& dev, const TuneSettings& base) : dev_(dev), base_(base) {}
IqSweepSource::~IqSweepSource() { close(); }
bool IqSweepSource::open(std::string& err) {
    src_ = makeSource(dev_);
    if (!src_) { err = "no driver for this radio"; return false; }
    if (!src_->start(base_, ring_, err)) { src_.reset(); return false; }
    open_ = true;
    return true;
}
void IqSweepSource::close() {
    if (src_ && open_) src_->stop();
    open_ = false;
    src_.reset();
}
bool IqSweepSource::retune(double hz, std::string& err) {
    if (!src_) { err = "not open"; return false; }
    TuneSettings s = base_;
    s.centerHz = hz;
    return src_->retune(s, err);
}
size_t IqSweepSource::capture(cf32* out, size_t n) {
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        const size_t m = ring_.read(out + got, n - got);
        got += m;
        if (m == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            if (++idle > 20000) break;   // 10 s without data
        } else idle = 0;
    }
    return got;
}
double IqSweepSource::sampleRate() const { return src_ ? src_->sampleRate() : base_.sampleRate; }
void IqSweepSource::discard(double seconds) {
    // wait out the settle time, then drop everything that arrived meanwhile (samples from before and during the retune)
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    ring_.skipToNewest(0);
}

} // namespace sweep
} // namespace dect2
