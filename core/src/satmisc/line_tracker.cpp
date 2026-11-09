// Spectral line tracker (see line_tracker.h).
#include "dect2/satmisc/line_tracker.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace satmisc {

void LineTracker::configure(const LineTrackerConfig& c) {
    cfg_ = c;
    n_ = 1 << c.fftLog2;
    hop_ = n_ / 2;
    fill_ = 0; avgN_ = 0; samples_ = 0; frames_ = 0; nextId_ = 1;
    binHz_ = c.rate / n_;
    frameSec_ = (double)hop_ * std::max(1, c.average) / c.rate;
    buf_.assign((size_t)n_, cf32(0, 0));
    win_.resize((size_t)n_);
    for (int i = 0; i < n_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / n_));
    acc_.assign((size_t)n_, 0.f);
    lastDb_.assign((size_t)n_, 0.f);
    active_.clear(); done_.clear();
}

void LineTracker::push(const cf32* x, size_t n) {
    if (n_ == 0) return;
    for (size_t i = 0; i < n; i++) {
        buf_[(size_t)fill_++] = x[i];
        if (fill_ == n_) {
            samples_ += (uint64_t)hop_;
            frame();
            std::copy(buf_.begin() + hop_, buf_.end(), buf_.begin());
            fill_ = n_ - hop_;
        }
    }
}

std::vector<LineTrack> LineTracker::takeFinished() { std::vector<LineTrack> v; v.swap(done_); return v; }

void LineTracker::frame() {
    static thread_local std::vector<cf32> w;
    w.resize((size_t)n_);
    for (int i = 0; i < n_; i++) w[(size_t)i] = buf_[(size_t)i] * win_[(size_t)i];
    Fft f(n_);
    f.forward(w.data());
    for (int i = 0; i < n_; i++) acc_[(size_t)i] += std::norm(w[(size_t)i]);
    if (++avgN_ < std::max(1, cfg_.average)) return;
    avgN_ = 0;
    frames_++;
    const double t = timeSec();

    // DC centred power, background: median of blocks across the band, interpolated
    std::vector<float> p((size_t)n_);
    for (int i = 0; i < n_; i++) { p[(size_t)i] = acc_[(size_t)((i + n_ / 2) % n_)]; }
    std::fill(acc_.begin(), acc_.end(), 0.f);
    const int blocks = std::max(4, std::min(64, n_ / 64)), bw = n_ / blocks;
    std::vector<float> med((size_t)blocks), tmp((size_t)bw);
    for (int b = 0; b < blocks; b++) {
        std::copy(p.begin() + b * bw, p.begin() + (b + 1) * bw, tmp.begin());
        std::nth_element(tmp.begin(), tmp.begin() + bw / 2, tmp.end());
        med[(size_t)b] = std::max(tmp[(size_t)(bw / 2)], 1e-20f);
    }
    struct Peak { double hz; float snr; bool used; };
    std::vector<Peak> peaks;
    const int edge = n_ / 40;
    for (int i = 0; i < n_; i++) {
        const int b = std::min(blocks - 1, i / bw);
        const float db = 10 * std::log10(std::max(p[(size_t)i], 1e-20f) / med[(size_t)b]);
        lastDb_[(size_t)i] = db;
    }
    for (int i = std::max(edge, 1); i < n_ - std::max(edge, 1); i++) {
        const float db = lastDb_[(size_t)i];
        if (db < cfg_.thresholdDb || p[(size_t)i] < p[(size_t)i - 1] || p[(size_t)i] < p[(size_t)i + 1]) continue;
        double hz = (i - n_ / 2) * binHz_;
        if (cfg_.spanHz > 0 && std::fabs(hz) > cfg_.spanHz) continue;
        if (cfg_.excludeHz > 0 && std::fabs(hz - cfg_.excludeCentreHz) < cfg_.excludeHz) continue;
        // parabolic interpolation on dB
        const double a = 10 * std::log10(std::max(p[(size_t)i - 1], 1e-20f)), bb = 10 * std::log10(std::max(p[(size_t)i], 1e-20f)), c = 10 * std::log10(std::max(p[(size_t)i + 1], 1e-20f));
        const double den = a - 2 * bb + c;
        if (std::fabs(den) > 1e-9) hz += 0.5 * (a - c) / den * binHz_;
        peaks.push_back({hz, db, false});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.snr > b.snr; });
    if (peaks.size() > 200) peaks.resize(200);

    // the strongest tracks choose first, each the nearest peak to where it should be now
    std::sort(active_.begin(), active_.end(), [](const LineTrack& a, const LineTrack& b) { return a.peakSnrDb > b.peakSnrDb; });
    for (auto& tr : active_) {
        const auto& q = tr.pts;
        double slope = 0;
        if (q.size() >= 4) {   // least squares over the last points
            const size_t m = std::min<size_t>(q.size(), 12), s0 = q.size() - m;
            double st = 0, sf = 0, stt = 0, stf = 0;
            for (size_t i = s0; i < q.size(); i++) { const double u = q[i].t - q[s0].t; st += u; sf += q[i].hz; stt += u * u; stf += u * q[i].hz; }
            const double d = (double)m * stt - st * st;
            if (d > 1e-9) slope = ((double)m * stf - st * sf) / d;
        }
        const double dt = t - q.back().t, want = q.back().hz + slope * dt;
        const double gate = std::max(3 * binHz_, 0.25 * cfg_.maxSlopeHzS * dt + 2 * binHz_);
        Peak* best = nullptr;
        double bd = gate;
        for (auto& pk : peaks) if (!pk.used && std::fabs(pk.hz - want) < bd) { bd = std::fabs(pk.hz - want); best = &pk; }
        if (best) {
            best->used = true;
            if (tr.pts.size() < cfg_.maxPoints) tr.pts.push_back({t, best->hz, 0});
            else tr.pts.back() = {t, best->hz, 0};
            tr.endSec = t;
            tr.peakSnrDb = std::max(tr.peakSnrDb, best->snr);
        }
    }
    for (size_t i = 0; i < active_.size();) {
        if (t - active_[i].endSec > cfg_.gapSec) {
            if (active_[i].durationSec() >= cfg_.minDurSec && (int)active_[i].pts.size() >= cfg_.minPoints) done_.push_back(std::move(active_[i]));
            active_.erase(active_.begin() + (long)i);
        } else i++;
    }
    for (const auto& pk : peaks) {
        if (pk.used || active_.size() >= cfg_.maxActive) continue;
        LineTrack tr;
        tr.id = nextId_++; tr.startSec = tr.endSec = t; tr.peakSnrDb = pk.snr;
        tr.pts.push_back({t, pk.hz, 0});
        active_.push_back(std::move(tr));
    }
}

} // namespace satmisc
} // namespace dect2
