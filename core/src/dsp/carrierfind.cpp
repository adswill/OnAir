// Carrier finder and channelizer: see carrierfind.h.
#include "dect2/dsp/carrierfind.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

struct CarrierFinder::FftHolder { explicit FftHolder(int n) : f(n) {} Fft f; };

void CarrierFinder::configure(double spanRateHz, const CarrierCfg& cfg) {
    cfg_ = cfg; rate_ = spanRateHz;
    // about 24 bins across a raster cell
    const double want = spanRateHz / std::max(1.0, cfg.stepHz / 24.0);
    nfft_ = 256; while (nfft_ < want && nfft_ < 65536) nfft_ <<= 1;
    win_.assign((size_t)nfft_, 0.f);
    for (int i = 0; i < nfft_; i++) win_[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * (i + 0.5) / nfft_);
    fft_ = std::make_shared<FftHolder>(nfft_);
    avgTarget_ = std::max(2, (int)std::lround(cfg.avgSec * spanRateHz / nfft_));
    reset();
}

void CarrierFinder::reset() {
    acc_.assign((size_t)nfft_, 0.0); buf_.assign((size_t)nfft_, cf32(0, 0)); work_.assign((size_t)nfft_, cf32(0, 0));
    fill_ = 0; nAvg_ = 0; trk_.clear(); cars_.clear(); removed_.clear(); noiseDb_ = -200; noiseInit_ = false; floorDb_.clear();
}

void CarrierFinder::setThresholds(double onDb, double offDb, double minSnrDb, double debounceSec, double holdSec) {
    cfg_.onDb = onDb; cfg_.offDb = std::min(offDb, onDb - 0.5); cfg_.minSnrDb = minSnrDb; cfg_.debounceSec = std::max(0.0, debounceSec); cfg_.holdSec = std::max(0.0, holdSec);
}

std::vector<int> CarrierFinder::takeRemoved() { std::vector<int> r; r.swap(removed_); return r; }

void CarrierFinder::shift(double deltaHz) {
    if (rate_ <= 0) return;
    const double step = cfg_.stepHz, half = rate_ / 2 * (1.0 - cfg_.edgeFrac) - step / 2;
    for (size_t i = trk_.size(); i-- > 0;) {
        Track& t = trk_[i];
        t.c.hz -= deltaHz;
        t.c.cellHz = cfg_.gridOriginHz + std::round((t.c.hz - cfg_.gridOriginHz) / step) * step;
        if (std::fabs(t.c.cellHz) > half) { if (t.reported) removed_.push_back(t.c.id); trk_.erase(trk_.begin() + (long)i); }
    }
    // two carriers that landed in one cell: the older one stays
    for (size_t i = 0; i < trk_.size(); i++)
        for (size_t j = trk_.size(); j-- > i + 1;)
            if (std::fabs(trk_[j].c.cellHz - trk_[i].c.cellHz) < 1.0) { if (trk_[j].reported) removed_.push_back(trk_[j].c.id); trk_.erase(trk_.begin() + (long)j); }
    std::fill(acc_.begin(), acc_.end(), 0.0); fill_ = 0; nAvg_ = 0; floorDb_.clear(); noiseInit_ = false;
    cars_.clear();
    for (auto& t : trk_) if (t.reported) cars_.push_back(t.c);
    std::sort(cars_.begin(), cars_.end(), [](const Carrier& a, const Carrier& b) { return a.hz < b.hz; });
}

void CarrierFinder::feed(const cf32* x, size_t n) {
    if (nfft_ <= 0) return;
    // whole pieces of the block at a time (the samples are copied once, no buffer is allocated per call)
    if (wbuf_.size() != (size_t)nfft_) wbuf_.resize((size_t)nfft_);
    cf32* w = wbuf_.data();
    size_t i = 0;
    while (i < n) {
        const size_t take = std::min(n - i, (size_t)(nfft_ - fill_));
        std::copy(x + i, x + i + take, buf_.begin() + fill_);
        fill_ += (int)take; i += take;
        if (fill_ == nfft_) {
            const float* wn = win_.data(); const cf32* b = buf_.data();
            for (int k = 0; k < nfft_; k++) w[k] = b[k] * wn[k];
            fft_->f.forward(w);
            double* acc = acc_.data();
            for (int k = 0; k < nfft_; k++) acc[k] += (double)(w[k].real() * w[k].real() + w[k].imag() * w[k].imag());   // std::norm without its infinity checks
            fill_ = 0;
            if (++nAvg_ >= avgTarget_) { update(); std::fill(acc_.begin(), acc_.end(), 0.0); nAvg_ = 0; }
        }
    }
}

void CarrierFinder::update() {
    const double dt = (double)nfft_ * avgTarget_ / rate_;
    updates_++;
    double wsum = 0; for (float v : win_) wsum += (double)v * v;
    const double norm = 1.0 / ((double)nAvg_ * wsum * (double)nfft_);   // power per bin as mean square per sample (full scale rms 1)
    const double binHz = rate_ / nfft_, step = cfg_.stepHz;
    auto binPow = [&](int k) { return acc_[(size_t)(((k % nfft_) + nfft_) % nfft_)] * norm; };   // k: signed bin, 0 = DC
    auto db = [](double p) { return 10 * std::log10(std::max(1e-20, p)); };
    auto lin = [](double d) { return std::pow(10.0, d / 10.0); };
    // raster cells inside the searched part of the span
    const double half = rate_ / 2 * (1.0 - cfg_.edgeFrac) - step / 2;
    const int kMin = (int)std::ceil((-half - cfg_.gridOriginHz) / step), kMax = (int)std::floor((half - cfg_.gridOriginHz) / step);
    if (kMax < kMin) return;
    const int nc = kMax - kMin + 1;
    std::vector<double> cell((size_t)nc), cen((size_t)nc);
    std::vector<int> nbins((size_t)nc);
    for (int c = 0; c < nc; c++) {
        const double f = cfg_.gridOriginHz + (kMin + c) * step; cen[(size_t)c] = f;
        const int b0 = (int)std::ceil((f - step * 0.4) / binHz), b1 = (int)std::floor((f + step * 0.4) / binHz);
        double p = 0; int nb = 0;
        for (int b = b0; b <= b1; b++) { p += binPow(b); nb++; }
        cell[(size_t)c] = p; nbins[(size_t)c] = std::max(1, nb);
    }
    // noise floor: a low percentile of the cells (a busy band has many carriers; the quiet cells between them are the floor). One for the span,
    // and one per cell from the cells around it (a front end's slope or ripple), kept within -10 / +12 dB of the span's; both smoothed over time.
    std::vector<char> ex((size_t)nc, 0);
    if (!excl_.empty()) for (int c = 0; c < nc; c++) for (const auto& z : excl_) if (cen[(size_t)c] >= z.first - step / 2 && cen[(size_t)c] <= z.second + step / 2) { ex[(size_t)c] = 1; break; }
    std::vector<double> srt;
    for (int c = 0; c < nc; c++) if (!ex[(size_t)c]) srt.push_back(cell[(size_t)c]);
    if (srt.empty()) srt = cell;
    std::sort(srt.begin(), srt.end());
    const double gDb = db(std::max(1e-20, srt[srt.size() / 4]) * 1.15);
    const double alpha = noiseInit_ ? std::min(1.0, dt / std::max(1e-3, cfg_.floorSec)) : 1.0;
    std::vector<double> raw((size_t)nc, gDb);
    if (cfg_.localCells > 0 && nc > 8) {
        std::vector<double> win;
        for (int c = 0; c < nc; c++) {
            const int a = std::max(0, c - cfg_.localCells), b = std::min(nc - 1, c + cfg_.localCells);
            win.clear();
            for (int j = a; j <= b; j++) if (!ex[(size_t)j]) win.push_back(cell[(size_t)j]);
            if (win.empty()) { raw[(size_t)c] = gDb; continue; }
            const size_t q = win.size() / 5;
            std::nth_element(win.begin(), win.begin() + (long)q, win.end());
            raw[(size_t)c] = std::clamp(db(std::max(1e-20, win[q]) * 1.15), gDb - 10.0, gDb + 12.0);
        }
    }
    if (!noiseInit_ || (int)floorDb_.size() != nc || floorKMin_ != kMin) { floorDb_ = raw; floorKMin_ = kMin; }
    else for (int c = 0; c < nc; c++) floorDb_[(size_t)c] += alpha * (raw[(size_t)c] - floorDb_[(size_t)c]);
    noiseDb_ = noiseInit_ ? noiseDb_ + alpha * (gDb - noiseDb_) : gDb; noiseInit_ = true;
    // known channels
    std::vector<char> prior((size_t)nc, 0);
    for (double p : cfg_.priorHz) {
        const int c = (int)std::lround((p - cfg_.gridOriginHz) / step) - kMin;
        if (c >= 0 && c < nc && std::fabs(p - cen[(size_t)c]) < step / 2) prior[(size_t)c] = 1;
    }
    // candidates: cells above the threshold (lower for a tracked one, lower for a known channel) that are not the skirt of a neighbour, nor a line
    auto cellOf = [&](const Track& t) { return (int)std::lround((t.c.cellHz - cfg_.gridOriginHz) / step) - kMin; };
    std::vector<char> isTracked((size_t)nc, 0), cand((size_t)nc, 0);
    std::vector<int> hiOf((size_t)nc);   // the last cell of the signal that starts at this cell (mergeSplit)
    for (int c = 0; c < nc; c++) hiOf[(size_t)c] = c;
    for (auto& t : trk_) { const int c = cellOf(t); if (c >= 0 && c < nc) isTracked[(size_t)c] = 1; }
    auto perBin = [&](int c) { return lin(floorDb_[(size_t)c]) / nbins[(size_t)c]; };
    for (int c = 0; c < nc; c++) {
        if (ex[(size_t)c]) continue;
        if (cfg_.priorOnly && !prior[(size_t)c] && !isTracked[(size_t)c]) continue;
        const double lvl = db(cell[(size_t)c]), snr = lvl - floorDb_[(size_t)c];
        const double bonus = prior[(size_t)c] ? cfg_.priorDb : 0.0;
        if (snr < (isTracked[(size_t)c] ? cfg_.offDb : cfg_.onDb) - bonus) continue;
        bool skirt = false;
        for (int d = -2; d <= 2 && !skirt; d++) {
            const int j = c + d;
            if (d && j >= 0 && j < nc && db(cell[(size_t)j]) > lvl + cfg_.skirtDb / (std::abs(d) == 1 ? 1.0 : 1.6)) skirt = true;
        }
        if (skirt) continue;
        if (cfg_.rejectTones && !isTracked[(size_t)c]) {   // a new line: the best 3 adjacent bins (a windowed line puts 98 % there) hold nearly all of the power above the floor, also over the cell's edges (a tracked carrier that goes unmodulated for a moment stays)
            const double pb = perBin(c);   // (a carrier that is only partly inside the cell, drifting or between raster points, is not a line)
            const int b0 = (int)std::ceil((cen[(size_t)c] - step * 0.75) / binHz), b1 = (int)std::floor((cen[(size_t)c] + step * 0.75) / binHz);
            std::vector<double> w;
            for (int b = b0; b <= b1; b++) w.push_back(std::max(0.0, binPow(b) - pb));
            double tot = 0, best = 0, run = 0;
            for (size_t i = 0; i < w.size(); i++) { tot += w[i]; run += w[i]; if (i >= 3) run -= w[i - 3]; best = std::max(best, run); }
            if (tot > 0 && best > 0.85 * tot) continue;
        }
        cand[(size_t)c] = 1;
    }
    // one signal over two adjacent cells: no dip at the boundary between them (two carriers on adjacent channels have one)
    std::vector<double> sigPow = cell;
    if (cfg_.mergeSplit) {
        for (int c = 0; c + 1 < nc; c++) {
            if (!cand[(size_t)c] || !cand[(size_t)c + 1] || (isTracked[(size_t)c] && isTracked[(size_t)c + 1])) continue;   // two tracked: two carriers
            const double fb = cen[(size_t)c] + step / 2;
            const int b0 = (int)std::ceil((fb - step * 0.08) / binHz), b1 = (int)std::floor((fb + step * 0.08) / binHz);
            double pb = 0; int nb = 0; for (int b = b0; b <= b1; b++) { pb += binPow(b); nb++; }
            if (nb == 0) continue;
            pb /= nb;
            const double mA = cell[(size_t)c] / nbins[(size_t)c], mB = cell[(size_t)c + 1] / nbins[(size_t)c + 1];
            if (pb < 0.1 * std::min(mA, mB) || pb < 4 * std::min(perBin(c), perBin(c + 1))) continue;   // a dip of 10 dB, or down to 6 dB over the floor: two carriers
            const bool keepB = isTracked[(size_t)c + 1] && !isTracked[(size_t)c] ? true : isTracked[(size_t)c] ? false : cell[(size_t)c + 1] > cell[(size_t)c];
            const int keep = keepB ? c + 1 : c, other = keepB ? c : c + 1;
            cand[(size_t)other] = 0;
            sigPow[(size_t)keep] = cell[(size_t)c] + cell[(size_t)c + 1];
            hiOf[(size_t)keep] = other;   // the other end of the signal (lower or higher)
            c++;   // the cell after the pair starts a new search
        }
    }
    // a tracked carrier whose own cell went quiet while the next cell (the side it was drifting to) has a new signal: it moved
    if (cfg_.trackDrift) {
        for (auto& t : trk_) {
            const int c = cellOf(t);
            if (c < 0 || c >= nc || cand[(size_t)c]) continue;
            const int d = t.c.hz > t.c.cellHz ? 1 : -1;
            const int j = c + d;
            if (j < 0 || j >= nc || !cand[(size_t)j] || isTracked[(size_t)j]) continue;
            if (std::fabs(t.c.hz - t.c.cellHz) < 0.15 * step) continue;
            t.c.cellHz = cen[(size_t)j]; isTracked[(size_t)c] = 0; isTracked[(size_t)j] = 1;
        }
    }
    for (int c = 0; c < nc; c++) if (cand[(size_t)c] && !isTracked[(size_t)c]) { Track t; t.c.cellHz = t.c.hz = cen[(size_t)c]; trk_.push_back(t); isTracked[(size_t)c] = 1; }
    for (auto& t : trk_) {
        t.c.active = false;
        const int c = cellOf(t);
        if (c < 0 || c >= nc) continue;
        t.c.prior = prior[(size_t)c] != 0;
        if (!cand[(size_t)c]) continue;
        t.c.active = true; t.c.levelDb = db(sigPow[(size_t)c]); t.c.snrDb = t.c.levelDb - floorDb_[(size_t)c] - (hiOf[(size_t)c] != c ? 3.0 : 0.0);
        t.peakSnr = std::max(t.peakSnr, t.c.snrDb);
        // centroid of the power above the floor inside the cell (both cells of a merged signal): where the carrier really is; and its width
        const int lo = std::min(c, hiOf[(size_t)c]), hi = std::max(c, hiOf[(size_t)c]);
        const double pb = perBin(c);
        double sw = 0, sf = 0; int first = 1 << 30, last = -(1 << 30);
        const int b0 = (int)std::ceil((cen[(size_t)lo] - step / 2) / binHz), b1 = (int)std::floor((cen[(size_t)hi] + step / 2) / binHz);
        for (int b = b0; b <= b1; b++) {
            const double p = binPow(b), w = std::max(0.0, p - 2 * pb);
            sw += w; sf += w * b * binHz;
            if (p > 4 * pb) { first = std::min(first, b); last = std::max(last, b); }
        }
        if (sw > 0) {
            const double lim = lo == hi ? step * 0.45 : step * 0.5;
            const double cf = std::clamp(sf / sw, cen[(size_t)lo] - lim, cen[(size_t)hi] + lim);
            t.c.hz = t.reported ? 0.6 * t.c.hz + 0.4 * cf : cf;
        }
        if (last >= first) { const double w = (last - first + 1) * binHz; t.c.widthHz = t.c.widthHz > 0 ? 0.7 * t.c.widthHz + 0.3 * w : w; }
    }
    for (auto& t : trk_) {
        if (t.c.active) { t.aboveSec += dt; t.c.idleSec = 0; } else { t.c.idleSec += dt; if (!t.reported) { t.aboveSec = 0; t.peakSnr = -100; } }
        if (!t.reported && t.aboveSec >= cfg_.debounceSec && t.peakSnr >= cfg_.minSnrDb) { t.reported = true; t.c.id = nextId_++; }
        if (t.reported) t.c.ageSec += dt;
    }
    // drop: never-confirmed ones that went quiet, and confirmed ones past the hold time
    for (size_t i = trk_.size(); i-- > 0;) {
        auto& t = trk_[i];
        if (!t.reported && !t.c.active) trk_.erase(trk_.begin() + (long)i);
        else if (t.reported && t.c.idleSec > cfg_.holdSec) { removed_.push_back(t.c.id); trk_.erase(trk_.begin() + (long)i); }
    }
    // the cap: active and strong first; an old one counts 6 dB stronger than it is (a newcomer must beat it clearly), a long quiet one a little
    // less; the kept one (the carrier listened to) always stays
    std::vector<size_t> idx;
    for (size_t i = 0; i < trk_.size(); i++) if (trk_[i].reported) idx.push_back(i);
    const int capN = std::max(1, cap());
    if ((int)idx.size() > capN) {
        auto score = [&](size_t i) { return (keep_ && trk_[i].c.id == keep_ ? 1e6 : 0.0) + (trk_[i].c.active ? 1000.0 : 0.0) + trk_[i].c.levelDb + (trk_[i].c.ageSec > 1.0 ? 6.0 : 0.0) - std::min(trk_[i].c.idleSec, 100.0) * 0.1; };
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return score(a) > score(b); });
        std::vector<char> drop(trk_.size(), 0);
        for (size_t k = (size_t)capN; k < idx.size(); k++) drop[idx[k]] = 1;
        for (size_t i = trk_.size(); i-- > 0;) if (drop[i]) { removed_.push_back(trk_[i].c.id); trk_.erase(trk_.begin() + (long)i); }
    }
    cars_.clear();
    for (auto& t : trk_) if (t.reported) cars_.push_back(t.c);
    std::sort(cars_.begin(), cars_.end(), [](const Carrier& a, const Carrier& b) { return a.hz < b.hz; });
}

bool Channelizer::configure(double inRate, double outRate, double shiftHz, double occupiedHz) {
    inRate_ = inRate; outRate_ = outRate; shift_ = shiftHz;
    const double w = inRate > 0 ? -2 * M_PI * shiftHz / inRate : 0;
    tre_.resize(kTab); tim_.resize(kTab);
    for (size_t k = 0; k < kTab; k++) { tre_[k] = (float)std::cos(w * (double)k); tim_[k] = (float)std::sin(w * (double)k); }
    stepRe_ = std::cos(w * (double)kTab); stepIm_ = std::sin(w * (double)kTab);
    phRe_ = 1; phIm_ = 0; pos_ = 0;
    return rs_.configure(inRate, outRate, nullptr, occupiedHz);
}

void Channelizer::reset() { rs_.reset(); phRe_ = 1; phIm_ = 0; pos_ = 0; }

void Channelizer::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    if (mix_.size() < n) mix_.resize(n);
    if (shift_ == 0 || tre_.empty()) std::copy(x, x + n, mix_.begin());
    else {
        size_t i = 0;
        while (i < n) {
            const size_t seg = std::min(n - i, kTab - pos_);
            const float pr = (float)phRe_, pi = (float)phIm_;
            const float* __restrict tr = tre_.data() + pos_;
            const float* __restrict ti = tim_.data() + pos_;
            const float* __restrict in = reinterpret_cast<const float*>(x + i);
            float* __restrict o = reinterpret_cast<float*>(mix_.data() + i);
            for (size_t k = 0; k < seg; k++) {
                const float lr = tr[k] * pr - ti[k] * pi, li = tr[k] * pi + ti[k] * pr;
                const float xr = in[2 * k], xi = in[2 * k + 1];
                o[2 * k] = xr * lr - xi * li;
                o[2 * k + 1] = xr * li + xi * lr;
            }
            i += seg; pos_ += seg;
            if (pos_ == kTab) {
                pos_ = 0;
                const double nr = phRe_ * stepRe_ - phIm_ * stepIm_, ni = phRe_ * stepIm_ + phIm_ * stepRe_;
                const double m = std::sqrt(nr * nr + ni * ni);
                phRe_ = nr / m; phIm_ = ni / m;
            }
        }
    }
    rs_.process(mix_.data(), n, out);
}

} // namespace dect2
