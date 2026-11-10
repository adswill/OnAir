#include "dect2/hdr/hdr_find.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2 {

struct HdrFinder::Impl {
    struct P { double hz; int count; double t; };
    mutable std::mutex mu;
    double rate = 0, centre = 0, now = 0;
    int nfft = 0, nAvg = 0, done = 0; size_t fill = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win; std::vector<cf32> frame, work; std::vector<double> acc;
    std::vector<P> pending; std::vector<HdrSpanHit> hits; bool changed = false;

    void cfg(double r, double c) {
        rate = r; centre = c; now = 0; done = 0; fill = 0; pending.clear(); hits.clear(); changed = true;
        nfft = 256; while (nfft < r / 1000.0) nfft <<= 1;
        nAvg = std::max(4, (int)std::lround(0.4 * r / nfft));
        fft.reset(new Fft(nfft)); win.resize((size_t)nfft);
        for (int i = 0; i < nfft; i++) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * (i + 0.5) / nfft);
        frame.assign((size_t)nfft, cf32(0, 0)); work = frame; acc.assign((size_t)nfft, 0.0);
    }
    void spectrum() {
        const double hz = rate / nfft;
        std::vector<double> p((size_t)nfft);
        for (int i = 0; i < nfft; i++) p[(size_t)i] = acc[(size_t)((i + nfft / 2) % nfft)] / done;
        std::vector<double> sorted = p; std::nth_element(sorted.begin(), sorted.begin() + nfft / 5, sorted.end());
        const double flr = std::max(sorted[(size_t)(nfft / 5)], 1e-20);
        auto band = [&](double a, double b, double* spreadDb = nullptr) {   // mean power in [a, b] Hz from the centre of the span
            const int ia = std::max(0, (int)std::lround(a / hz) + nfft / 2), ib = std::min(nfft - 1, (int)std::lround(b / hz) + nfft / 2);
            double s = 0; int n = 0; std::vector<double> v;
            for (int i = ia; i <= ib; i++) { s += p[(size_t)i]; n++; v.push_back(p[(size_t)i]); }
            if (spreadDb && v.size() >= 8) { std::sort(v.begin(), v.end()); *spreadDb = 10 * std::log10(std::max(v[v.size() * 9 / 10], 1e-20) / std::max(v[v.size() / 10], 1e-20)); }
            return n ? s / n : 0.0;
        };
        // the raster: whole 100 kHz in absolute frequency when the centre is known, else from the centre of the span
        const double base = centre > 0 ? std::fmod(centre, 100000.0) : 0.0;
        const double lim = rate / 2 - 230000.0;
        std::vector<HdrSpanHit> found;
        for (double f = -std::floor(lim / 100000.0) * 100000.0 - base; f <= lim; f += 100000.0) {
            double spL = 99, spU = 99;
            const double lo = band(f - 194000, f - 136000, &spL), up = band(f + 136000, f + 194000, &spU);
            const double outL = band(f - 225000, f - 208000), outU = band(f + 208000, f + 225000);
            const double dL = 10 * std::log10(std::max(lo, 1e-20) / flr), dU = 10 * std::log10(std::max(up, 1e-20) / flr);
            if (dL < 5 || dU < 5 || spL > 12 || spU > 12) continue;
            if (std::fabs(dL - dU) > 12) continue;                                       // a real pair is of about equal level; a neighbour's skirt is on one side
            (void)outL; (void)outU;   // what lies beyond 198 kHz is not used: extended and neighbouring signals vary; the receiver proves the station by locking
            HdrSpanHit h; h.hz = f; h.lowerDb = (float)dL; h.upperDb = (float)dU; h.lastSec = now;
            found.push_back(h);
        }
        for (const auto& h : found) {
            auto it = std::find_if(hits.begin(), hits.end(), [&](const HdrSpanHit& x) { return std::fabs(x.hz - h.hz) < 1000; });
            if (it != hits.end()) { *it = h; continue; }
            auto q = std::find_if(pending.begin(), pending.end(), [&](const P& x) { return std::fabs(x.hz - h.hz) < 1000; });
            if (q == pending.end()) { pending.push_back({h.hz, 1, now}); continue; }
            q->t = now;
            if (++q->count >= 3) { hits.push_back(h); pending.erase(q); changed = true; }
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const P& x) { return now - x.t > 3; }), pending.end());
        const size_t before = hits.size();
        hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const HdrSpanHit& x) { return now - x.lastSec > 30; }), hits.end());
        if (hits.size() != before) changed = true;
    }
    void feed(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> g(mu);
        if (rate <= 0) return;
        size_t i = 0;
        while (i < n) {
            const size_t k = std::min(n - i, (size_t)nfft - fill);
            std::copy(x + i, x + i + k, frame.begin() + (long)fill); fill += k; i += k;
            if (fill == (size_t)nfft) {
                fill = 0;
                for (int b = 0; b < nfft; b++) work[(size_t)b] = frame[(size_t)b] * win[(size_t)b];
                fft->forward(work.data());
                for (int b = 0; b < nfft; b++) acc[(size_t)b] += std::norm(work[(size_t)b]);
                if (++done >= nAvg) { spectrum(); std::fill(acc.begin(), acc.end(), 0.0); done = 0; }
            }
        }
        now += (double)n / rate;
    }
};

HdrFinder::HdrFinder() : p_(new Impl()) {}
HdrFinder::~HdrFinder() = default;
void HdrFinder::configure(double r, double c) { std::lock_guard<std::mutex> g(p_->mu); p_->cfg(r, c); }
void HdrFinder::reset() { std::lock_guard<std::mutex> g(p_->mu); if (p_->rate > 0) p_->cfg(p_->rate, p_->centre); }
void HdrFinder::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool HdrFinder::takeChanged(std::vector<HdrSpanHit>& out) { std::lock_guard<std::mutex> g(p_->mu); if (!p_->changed) return false; p_->changed = false; out = p_->hits; return true; }
std::vector<HdrSpanHit> HdrFinder::hits() const { std::lock_guard<std::mutex> g(p_->mu); return p_->hits; }

} // namespace dect2
