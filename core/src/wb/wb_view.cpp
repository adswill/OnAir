// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/wb/wb_view.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace dect2 {
namespace wb {

// ---- overview ------------------------------------------------------------------------------------------------------------------

Overview::Overview(std::shared_ptr<Source> s, size_t blocks) : s_(std::move(s)) {
    const uint64_t len = s_->length();
    nb_ = (size_t)std::max<uint64_t>(1, std::min<uint64_t>(blocks, len));
    bl_ = std::max<uint64_t>(1, (len + nb_ - 1) / nb_);
    nb_ = (size_t)((len + bl_ - 1) / bl_);
    b_.assign(nb_, OverviewBlock{1e30f, -1e30f, 1e30f, -1e30f, 0, 0});
    pwAcc_.assign(nb_, 0.0);
    th_ = dect2::namedThread("onair-wbview", [this]() {
        const uint64_t n = s_->length();
        std::vector<cf32> buf(1u << 18);
        uint64_t pos = 0;
        while (pos < n && !stop_.load()) {
            const size_t got = s_->read(pos, buf.size(), buf.data());
            if (!got) break;
            std::vector<OverviewBlock> local;   // the blocks touched by this chunk
            const size_t b0 = (size_t)(pos / bl_), b1 = (size_t)((pos + got - 1) / bl_);
            local.assign(b1 - b0 + 1, OverviewBlock{1e30f, -1e30f, 1e30f, -1e30f, 0, 0});
            std::vector<double> pw(local.size(), 0.0);
            double chunkPower = 0, pk = peak_.load();
            cf32 mean(0, 0);
            for (size_t i = 0; i < got; i++) {
                const size_t bi = (size_t)((pos + i) / bl_) - b0;
                OverviewBlock& o = local[bi];
                const float re = buf[i].real(), im = buf[i].imag();
                o.minI = std::min(o.minI, re); o.maxI = std::max(o.maxI, re);
                o.minQ = std::min(o.minQ, im); o.maxQ = std::max(o.maxQ, im);
                const float e = re * re + im * im;
                pw[bi] += e;
                const float mg = std::sqrt(e);
                o.maxMag = std::max(o.maxMag, mg);
                chunkPower += e;
                if (mg > pk) pk = mg;
                mean += buf[i];
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (size_t k = 0; k < local.size(); k++) {
                    OverviewBlock& d = b_[b0 + k];
                    const OverviewBlock& o = local[k];
                    // a block spanning two chunks: combine; the power is kept as a sum until the block is complete
                    d.minI = std::min(d.minI, o.minI); d.maxI = std::max(d.maxI, o.maxI);
                    d.minQ = std::min(d.minQ, o.minQ); d.maxQ = std::max(d.maxQ, o.maxQ);
                    d.maxMag = std::max(d.maxMag, o.maxMag);
                    pwAcc_[b0 + k] += pw[k];
                    d.power = (float)(pwAcc_[b0 + k] / (double)bl_);
                }
                sumPower_ += chunkPower;
                sumMean_ += mean;
            }
            peak_ = pk;
            pos += got;
            prog_ = (double)pos / (double)n * 0.999;
        }
        {   // block powers become means
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t k = 0; k < b_.size(); k++) {
                const uint64_t a = (uint64_t)k * bl_, e = std::min<uint64_t>(n, a + bl_);
                b_[k].power = (float)(pwAcc_[k] / (double)std::max<uint64_t>(1, e - a));
            }
        }
        prog_ = 1.0;
    });
}

Overview::~Overview() { stop_ = true; if (th_.joinable()) th_.join(); }

std::vector<OverviewBlock> Overview::snapshot() const { std::lock_guard<std::mutex> lk(mu_); return b_; }
double Overview::rmsDb() const {
    std::lock_guard<std::mutex> lk(mu_);
    return 10 * std::log10(std::max(1e-30, sumPower_ / (double)std::max<uint64_t>(1, s_->length())));
}
cf32 Overview::mean() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sumMean_ / (float)std::max<uint64_t>(1, s_->length());
}
std::vector<float> Overview::powerDb() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<float> o(b_.size());
    const bool fin = prog_.load() >= 1;
    for (size_t i = 0; i < b_.size(); i++) o[i] = (float)(10 * std::log10(std::max(1e-30, (double)b_[i].power))) - (fin ? 0.f : 0.f);
    return o;
}

void waveformColumns(const Source& s, const Overview* ov, uint64_t t0, uint64_t t1, int cols, WaveColumns& o) {
    cols = std::max(1, cols);
    t1 = std::min<uint64_t>(t1, s.length());
    o.minI.assign((size_t)cols, 0); o.maxI.assign((size_t)cols, 0); o.minQ.assign((size_t)cols, 0); o.maxQ.assign((size_t)cols, 0); o.mag.assign((size_t)cols, 0);
    if (t1 <= t0) return;
    const double span = (double)(t1 - t0);
    const uint64_t kMax = 6u << 20;
    if (t1 - t0 <= kMax) {
        std::vector<cf32> buf((size_t)(t1 - t0));
        const size_t got = s.read(t0, buf.size(), buf.data());
        for (int c = 0; c < cols; c++) {
            const size_t a = (size_t)((double)c * span / cols), b = std::min(got, std::max(a + 1, (size_t)((double)(c + 1) * span / cols)));
            float mn = 1e30f, mx = -1e30f, qn = 1e30f, qx = -1e30f, mg = 0;
            for (size_t i = a; i < b; i++) {
                mn = std::min(mn, buf[i].real()); mx = std::max(mx, buf[i].real());
                qn = std::min(qn, buf[i].imag()); qx = std::max(qx, buf[i].imag());
                mg = std::max(mg, std::abs(buf[i]));
            }
            if (mn > mx) { mn = mx = qn = qx = 0; }
            o.minI[(size_t)c] = mn; o.maxI[(size_t)c] = mx; o.minQ[(size_t)c] = qn; o.maxQ[(size_t)c] = qx; o.mag[(size_t)c] = mg;
        }
        return;
    }
    if (!ov) return;
    const std::vector<OverviewBlock> b = ov->snapshot();
    const double bl = (double)ov->blockLen();
    for (int c = 0; c < cols; c++) {
        const double a = ((double)t0 + (double)c * span / cols) / bl, e = ((double)t0 + (double)(c + 1) * span / cols) / bl;
        size_t i0 = (size_t)a, i1 = std::max(i0 + 1, (size_t)std::ceil(e));
        i1 = std::min(i1, b.size());
        float mn = 1e30f, mx = -1e30f, qn = 1e30f, qx = -1e30f, mg = 0;
        for (size_t i = i0; i < i1; i++) {
            mn = std::min(mn, b[i].minI); mx = std::max(mx, b[i].maxI);
            qn = std::min(qn, b[i].minQ); qx = std::max(qx, b[i].maxQ);
            mg = std::max(mg, b[i].maxMag);
        }
        if (mn > mx) { mn = mx = qn = qx = 0; }
        o.minI[(size_t)c] = mn; o.maxI[(size_t)c] = mx; o.minQ[(size_t)c] = qn; o.maxQ[(size_t)c] = qx; o.mag[(size_t)c] = mg;
    }
}

// ---- spectrogram ---------------------------------------------------------------------------------------------------------------

SpecEngine::SpecEngine(std::shared_ptr<Source> s) : s_(std::move(s)) { th_ = dect2::namedThread("onair-wbspec", [this]() { run(); }); }

SpecEngine::~SpecEngine() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
        curGen_ = ++gen_;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void SpecEngine::request(const SpecParams& p) {
    std::lock_guard<std::mutex> lk(mu_);
    if (gen_ > 0 && p == req_) return;
    req_ = p;
    pending_ = p;
    havePending_ = true;
    curGen_ = ++gen_;
    cv_.notify_all();
}

bool SpecEngine::poll(SpecImage& out, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (doneSeq_ <= seq) return false;
    out = img_;
    seq = doneSeq_;
    return true;
}

void SpecEngine::run() {
    for (;;) {
        SpecParams p;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this]() { return quit_ || havePending_; });
            if (quit_) return;
            p = pending_;
            havePending_ = false;
            gen = gen_;
            busy_ = true;
        }
        compute(p, gen);
        busy_ = false;
    }
}

void SpecEngine::compute(const SpecParams& p, uint64_t gen) {
    const auto t0c = std::chrono::steady_clock::now();
    const bool real = s_->real();
    const int N = std::max(16, p.fftSize);
    const double rate = s_->rate();
    const uint64_t len = s_->length();
    const uint64_t a = std::min(p.t0, len), b = std::min(std::max(p.t1, a + 1), len);
    const int cols = std::max(1, p.cols);
    const double binHz = rate / N;
    // bin range of the band in view
    const double fmin = real ? 0 : -rate / 2, fmax = real ? rate / 2 : rate / 2 - binHz;
    const double lo = std::max(fmin, std::min(p.f0, fmax)), hi = std::min(fmax, std::max(p.f1, fmin));
    int k0, k1;   // indices into the shifted spectrum (complex: bin k is (k - N/2) * binHz; real: k * binHz)
    if (real) { k0 = (int)std::floor(lo / binHz); k1 = (int)std::ceil(hi / binHz); k1 = std::min(k1, N / 2); }
    else { k0 = (int)std::floor(lo / binHz) + N / 2; k1 = (int)std::ceil(hi / binHz) + N / 2; k1 = std::min(k1, N - 1); }
    k0 = std::max(0, k0); k1 = std::max(k0, k1);
    const int nb = k1 - k0 + 1;
    const int rows = std::max(1, std::min(nb, p.maxRows));
    SpecImage img;
    img.p = p; img.w = cols; img.h = rows;
    img.db.assign((size_t)cols * rows, -200.f);
    img.binHz = binHz;
    img.fLo = (real ? k0 : k0 - N / 2) * binHz - binHz / 2;
    img.fHi = (real ? k1 : k1 - N / 2) * binHz + binHz / 2;
    const std::vector<float> w = makeWindow(p.window, N);
    double sw = 0;
    for (float v : w) sw += v;
    const double norm = 1.0 / (sw * sw) * (real ? 4.0 : 1.0);
    const double span = (double)(b - a) / cols;
    const double hop = std::max(1.0, N * (1.0 - std::min(0.95, std::max(0.0, p.overlap))));
    const double ideal = span / hop;
    const bool maxHold = ideal > 4.0;
    const int nsub = maxHold ? 4 : std::max(1, (int)std::lround(ideal));
    unsigned hw = std::thread::hardware_concurrency();
    const int nth = (int)std::max(1u, std::min(3u, hw / 2));
    auto worker = [&](int t) {
        std::vector<cf32> buf((size_t)N);
        std::vector<float> re, im;
        std::vector<double> acc((size_t)nb);
        for (int c = t; c < cols; c += nth) {
            if (curGen_.load() != gen) return;
            std::fill(acc.begin(), acc.end(), 0.0);
            for (int j = 0; j < nsub; j++) {
                double centre;
                if (maxHold) centre = (double)a + ((double)c + (j + 0.5) / nsub) * span;
                else centre = (double)a + ((double)c + 0.5) * span + (j - (nsub - 1) / 2.0) * hop;
                double st = centre - N / 2.0;
                st = std::max(0.0, std::min(st, (double)len - N));
                std::fill(buf.begin(), buf.end(), cf32(0, 0));
                const size_t got = s_->read((uint64_t)st, (size_t)N, buf.data());
                for (size_t i = 0; i < got; i++) buf[i] *= w[i];
                fft(buf.data(), N, re, im);
                for (int k = 0; k < nb; k++) {
                    const int bin = real ? k0 + k : (k0 + k + N / 2) % N;
                    const double pw = (double)std::norm(buf[(size_t)bin]);
                    if (maxHold) acc[(size_t)k] = std::max(acc[(size_t)k], pw);
                    else acc[(size_t)k] += pw / nsub;
                }
            }
            for (int r = 0; r < rows; r++) {   // row 0 on top: the highest frequency
                const int kk0 = (int)((long long)(rows - 1 - r) * nb / rows), kk1 = std::max(kk0 + 1, (int)((long long)(rows - r) * nb / rows));
                double m = 0;
                for (int k = kk0; k < std::min(kk1, nb); k++) m = std::max(m, acc[(size_t)k]);
                img.db[(size_t)r * cols + c] = (float)dbOf(m * norm);
            }
        }
    };
    std::vector<std::thread> th;
    for (int t = 1; t < nth; t++) th.emplace_back(dect2::namedThread("onair-wbw", worker, t));
    worker(0);
    for (auto& t : th) t.join();
    if (curGen_.load() != gen) return;
    std::vector<float> sample;
    for (size_t i = 0; i < img.db.size(); i += 5) sample.push_back(img.db[i]);
    if (!sample.empty()) {
        std::sort(sample.begin(), sample.end());
        img.p5 = sample[sample.size() * 5 / 100];
        img.p995 = sample[std::min(sample.size() - 1, sample.size() * 995 / 1000)];
    }
    img.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0c).count();
    std::lock_guard<std::mutex> lk(mu_);
    if (curGen_.load() != gen) return;
    img_ = std::move(img);
    img_.seq = ++doneSeq_;
}

} // namespace wb
} // namespace dect2
