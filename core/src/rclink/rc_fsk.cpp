// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst finder and FSK slicer of the hopping links; see rc_fsk.h.
#include "dect2/rclink/rc_fsk.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace rclink {

static void fftInPlace(std::vector<cf32>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / (double)len;
        const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf32 w(1.f, 0.f);
            for (size_t j = 0; j < len / 2; j++) {
                const cf32 u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void BurstFinder::configure(double rate, double thresholdDb, double maxSec) {
    fs_ = rate;
    thr_ = std::pow(10.0, thresholdDb / 10.0);
    maxSec_ = maxSec;
    int n = 64;
    while (n < 2048 && rate / n > 40e3) n <<= 1;
    N_ = n;
    hop_ = n / 2;
    win_.resize(n);
    for (int i = 0; i < n; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / n));
    fft_.resize(n);
    power_.assign(n, 0.f);
    smooth_.assign(n, 0.f);
    floor_.assign(n, 0.f);
    reset();
}

void BurstFinder::reset() {
    frames_ = 0;
    base_ = 0;
    next_ = 0;
    hist_.clear();
    open_.clear();
    floorSet_ = false;
}

void BurstFinder::close(const Open& o, const std::function<void(const Burst&)>& cb) {
    if (o.frames < 2) return;                                     // one lit frame is noise or a splatter
    Burst b;
    const int64_t pre = (int64_t)N_, post = (int64_t)N_;
    b.start = std::max<int64_t>(base_, o.startFrame * hop_ - pre);
    b.end = std::min<int64_t>(end(), (o.lastFrame + 1) * (int64_t)hop_ + N_ + post);
    const double binHz = fs_ / N_;
    b.freqHz = (o.e > 0 ? o.ef / o.e : (o.lo + o.hi) * 0.5) * binHz;
    b.bwHz = o.width / o.frames * binHz;                           // the mean over its frames: the switch-on splatter must not count
    b.snrDb = (float)(10.0 * std::log10(std::max(1e-3, o.peak)));
    if (b.end > b.start) cb(b);
}

void BurstFinder::frame(const cf32* w, const std::function<void(const Burst&)>& cb) {
    for (int i = 0; i < N_; i++) fft_[i] = w[i] * win_[i];
    fftInPlace(fft_);
    for (int i = 0; i < N_; i++) power_[i] = std::norm(fft_[i]);
    // bin k of the shifted spectrum: k - N/2
    for (int i = 0; i < N_; i++) {
        const int a = (i + N_ / 2) % N_, l = (a + N_ - 1) % N_, r = (a + 1) % N_;
        smooth_[i] = (power_[l] + 2.f * power_[a] + power_[r]) * 0.25f;
    }
    if (!floorSet_) {
        floor_ = smooth_;
        if (frames_ == 0) {
            // the first frame may hold a signal: start from the median of the spectrum for every bin
            std::vector<float> s = smooth_;
            std::nth_element(s.begin(), s.begin() + N_ / 2, s.end());
            std::fill(floor_.begin(), floor_.end(), s[N_ / 2]);
        }
        floorSet_ = true;
    }
    std::vector<std::array<int, 2>> segs;
    for (int i = 0; i < N_;) {
        if (smooth_[i] > (float)thr_ * floor_[i]) {
            int j = i;
            while (j + 1 < N_ && (smooth_[j + 1] > (float)thr_ * floor_[j + 1] || (j + 2 < N_ && smooth_[j + 2] > (float)thr_ * floor_[j + 2]))) j++;
            segs.push_back({i, j});
            i = j + 2;
        } else i++;
    }
    for (int i = 0; i < N_; i++) {                                 // the noise floor follows slowly, and only where nothing is lit up
        if (smooth_[i] <= (float)thr_ * floor_[i]) floor_[i] += 0.02f * (smooth_[i] - floor_[i]);
        else floor_[i] *= 1.002f;                                  // a constant carrier eventually becomes the floor
    }
    for (const auto& s : segs) {
        const int lo = s[0], hi = s[1];
        double e = 0, ef = 0, pk = 0;
        for (int k = lo; k <= hi; k++) {
            e += smooth_[k];
            ef += smooth_[k] * (k - N_ / 2);
            pk = std::max(pk, (double)(smooth_[k] / floor_[k]));
        }
        Open* o = nullptr;
        for (Open& c : open_) if (lo <= c.hi + 1 && hi >= c.lo - 1) { o = &c; break; }
        if (o) {
            o->lo = lo; o->hi = hi;
            o->lastFrame = (int)frames_; o->e += e; o->ef += ef; o->peak = std::max(o->peak, pk);
            o->frames++; o->width += hi - lo + 1;
        } else open_.push_back({lo, hi, (int)frames_, frames_, e, ef, pk, 1, (double)(hi - lo + 1)});
    }
    const int maxFrames = (int)(maxSec_ * fs_ / hop_);
    for (size_t i = 0; i < open_.size();) {
        Open& o = open_[i];
        if ((int)frames_ - o.lastFrame > 2 || (int)frames_ - (int)o.startFrame > maxFrames) {
            close(o, cb);
            open_.erase(open_.begin() + (std::ptrdiff_t)i);
        } else i++;
    }
    frames_++;
}

void BurstFinder::feed(const cf32* x, size_t n, const std::function<void(const Burst&)>& cb) {
    if (fs_ <= 0) return;
    hist_.insert(hist_.end(), x, x + n);
    std::vector<cf32> w(N_);
    while (next_ + N_ <= end()) {
        frame(at(next_), cb);
        next_ += hop_;
    }
    int64_t keep = next_ - N_ * 3;
    for (const Open& o : open_) keep = std::min<int64_t>(keep, o.startFrame * hop_ - 2 * N_);
    keep = std::max(keep, base_);
    const size_t drop = (size_t)(keep - base_);
    if (drop > (1u << 18)) {
        hist_.erase(hist_.begin(), hist_.begin() + (std::ptrdiff_t)drop);
        base_ += (int64_t)drop;
    }
}

void BurstFinder::flush(const std::function<void(const Burst&)>& cb) {
    for (const Open& o : open_) close(o, cb);
    open_.clear();
}

std::vector<float> discriminate(const BurstFinder& f, const Burst& b, double cutoffHz, double outRate, double* actualRate) {
    const double fs = f.rate();
    const int64_t n = b.end - b.start;
    std::vector<float> out;
    if (n <= 0) return out;
    int D = std::max(1, (int)std::floor(fs / outRate));
    const int taps = std::max(15, (int)(4 * fs / cutoffHz)) | 1;
    std::vector<float> h(taps);
    const int M = taps / 2;
    double sum = 0;
    for (int i = -M; i <= M; i++) {
        const double x = 2.0 * cutoffHz / fs * i;
        const double s = i == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double wn = 0.54 + 0.46 * std::cos(M_PI * i / (double)M);
        h[i + M] = (float)(s * wn);
        sum += s * wn;
    }
    for (float& v : h) v = (float)(v / sum);
    const cf32* x = f.at(b.start);
    std::vector<cf32> mixed((size_t)n);
    const double inc = -2.0 * M_PI * b.freqHz / fs;
    double ph = 0;
    for (int64_t i = 0; i < n; i++) {
        mixed[(size_t)i] = x[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
        ph += inc;
        if (ph > M_PI) ph -= 2.0 * M_PI; else if (ph < -M_PI) ph += 2.0 * M_PI;
    }
    std::vector<cf32> dec;
    dec.reserve((size_t)(n / D + 2));
    for (int64_t i = 0; i + taps <= n; i += D) {
        float re = 0, im = 0;
        for (int k = 0; k < taps; k++) { re += mixed[(size_t)(i + k)].real() * h[k]; im += mixed[(size_t)(i + k)].imag() * h[k]; }
        dec.emplace_back(re, im);
    }
    if (actualRate) *actualRate = fs / D;
    out.resize(dec.size() > 0 ? dec.size() - 1 : 0);
    for (size_t i = 1; i < dec.size(); i++) out[i - 1] = std::arg(dec[i] * std::conj(dec[i - 1]));
    return out;
}

SyncHit findSync(const std::vector<float>& d, double sps, const std::vector<uint8_t>& pat, int maxErrors, bool invertedOk) {
    SyncHit best;
    const size_t L = pat.size();
    if (L == 0 || d.size() < (size_t)(L * sps) + 2) return best;
    // running sum for a boxcar of one symbol
    std::vector<double> run(d.size() + 1, 0.0);
    for (size_t i = 0; i < d.size(); i++) run[i + 1] = run[i] + d[i];
    const int w = std::max(1, (int)std::lround(sps));
    const auto sym = [&](double pos) {                             // mean over one symbol
        const int64_t a = (int64_t)std::lround(pos);
        if (a < 0 || (size_t)(a + w) > d.size()) return 0.0;
        return (run[(size_t)(a + w)] - run[(size_t)a]) / w;
    };
    const int stepPts = std::max(1, (int)(sps / 8));
    const int64_t last = (int64_t)d.size() - (int64_t)((L + 2) * sps) - w;
    double bestScore = -1;
    std::vector<double> vals(L);
    for (int64_t p = 0; p <= last; p += stepPts) {
        double one = 0, zero = 0; int n1 = 0, n0 = 0;
        for (size_t i = 0; i < L; i++) {
            vals[i] = sym((double)p + i * sps);
            if (pat[i]) { one += vals[i]; n1++; } else { zero += vals[i]; n0++; }
        }
        if (!n1 || !n0) continue;
        const double m1 = one / n1, m0 = zero / n0, mid = 0.5 * (m1 + m0);
        const double sep = m1 - m0;
        if (std::fabs(sep) < 1e-6) continue;
        const bool inv = sep < 0;
        if (inv && !invertedOk) continue;
        int err = 0;
        for (size_t i = 0; i < L; i++) if (((vals[i] - mid) > 0) != ((pat[i] != 0) != inv)) err++;
        const double score = std::fabs(sep) * (double)(L - err);
        if (err <= maxErrors && score > bestScore) {
            bestScore = score;
            best.found = true;
            best.errors = err;
            best.position = (double)p + L * sps;
            best.offset = mid;
        }
    }
    return best;
}

std::vector<uint8_t> sliceBytes(const std::vector<float>& d, double sps, const SyncHit& hit, size_t nBytes, bool msbFirst) {
    std::vector<uint8_t> out;
    const int w = std::max(1, (int)std::lround(sps));
    double pos = hit.position;
    for (size_t b = 0; b < nBytes; b++) {
        uint8_t v = 0;
        bool complete = true;
        for (int i = 0; i < 8; i++) {
            int64_t a = (int64_t)std::lround(pos);
            if (a < 0 || (size_t)(a + w) > d.size()) { complete = false; break; }
            double s = 0;
            for (int k = 0; k < w; k++) s += d[(size_t)(a + k)];
            const int bit = (s / w - hit.offset) > 0;
            if (msbFirst) v = (uint8_t)((v << 1) | bit);
            else v = (uint8_t)(v | (bit << i));
            pos += sps;
        }
        if (!complete) break;
        out.push_back(v);
    }
    return out;
}

std::vector<uint8_t> bitsOfBytes(const std::vector<uint8_t>& bytes, bool msbFirst) {
    std::vector<uint8_t> out;
    for (uint8_t v : bytes)
        for (int i = 0; i < 8; i++) out.push_back(msbFirst ? (uint8_t)((v >> (7 - i)) & 1) : (uint8_t)((v >> i) & 1));
    return out;
}

} // namespace rclink
} // namespace dect2
