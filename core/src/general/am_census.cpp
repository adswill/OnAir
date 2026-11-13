// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/general/am_census.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

const char* amKindName(AmKind k) { return k == AmKind::Am ? "AM" : k == AmKind::Drm ? "DRM" : k == AmKind::Ssb ? "SSB" : "carrier"; }

namespace {
constexpr double kFrameSec = 0.25;       // one evaluation per this much signal
constexpr double kHistSec = 30;
}

struct AmCensus::Impl {
    double rate = 0, centre = 0, binHz = 0, step = 0;
    AmCensusConfig cfg;
    int N = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> in, buf;
    std::vector<float> win, power;
    size_t have = 0;
    int nAvg = 0, wantAvg = 1;
    double secs = 0;
    struct Ch { std::deque<float> car, side; AmCensusRow row; int seen = 0; };
    std::map<long, Ch> ch;              // by raster index
    int rasterVotes9 = 0, rasterVotes10 = 0;
    mutable std::mutex mu;
    std::vector<AmCensusRow> table;

    bool configure(double r, double c, const AmCensusConfig& conf) {
        rate = r; centre = c; cfg = conf;
        if (r < 500e3) return false;
        int bits = 12;
        while ((1 << bits) < r / 20 && bits < 20) bits++;   // a bin of 20 Hz or finer
        N = 1 << bits; binHz = r / N;
        fft = std::make_unique<Fft>(N);
        in.assign((size_t)N, cf32(0, 0)); buf.resize((size_t)N); win.resize((size_t)N); power.assign((size_t)N, 0.f);
        for (int i = 0; i < N; i++) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / N);
        wantAvg = std::max(1, (int)std::lround(kFrameSec * r / (N / 2.0)));
        reset();
        return true;
    }
    void reset() {
        have = 0; nAvg = 0; secs = 0; ch.clear(); rasterVotes9 = rasterVotes10 = 0;
        std::fill(power.begin(), power.end(), 0.f);
        std::lock_guard<std::mutex> lk(mu); table.clear();
    }
    void feed(const cf32* x, size_t n) {
        if (!fft) return;
        while (n) {
            const size_t t = std::min(n, (size_t)N - have);
            std::copy(x, x + t, in.begin() + (std::ptrdiff_t)have);
            have += t; x += t; n -= t;
            if (have == (size_t)N) {
                for (int i = 0; i < N; i++) { const cf32 v = in[(size_t)i]; buf[(size_t)i] = std::isfinite(v.real() + v.imag()) ? v * win[(size_t)i] : cf32(0, 0); }
                fft->forward(buf.data());
                for (int k = 0; k < N; k++) power[(size_t)((k + N / 2) % N)] += std::norm(buf[(size_t)k]);
                std::copy(in.begin() + N / 2, in.end(), in.begin());
                have = (size_t)N / 2;
                secs += N / 2.0 / rate;
                if (++nAvg >= wantAvg) { evaluate(); nAvg = 0; std::fill(power.begin(), power.end(), 0.f); }
            }
        }
    }
    double raster() const {
        if (cfg.stepHz > 0) return cfg.stepHz;
        if (centre > 2e6) return 5e3;
        return rasterVotes10 > rasterVotes9 ? 10e3 : 9e3;
    }
    float db(double p) const { return (float)(10 * std::log10(std::max(p, 1e-20))); }
    double norm() const { return (double)std::max(1, nAvg) * N * N * 0.375 * 0.375; }

    double band(int b0, int b1) const {   // mean power of bins [b0, b1)
        double s = 0; int n = 0;
        for (int b = std::max(0, b0); b < std::min(N, b1); b++) { s += power[(size_t)b]; n++; }
        return n ? s / n : 0;
    }

    void evaluate() {
        const double half = rate * 0.46;
        const double nrm = norm();
        // the strong lines: peaks far above their neighbourhood, used to tell the raster (9 or 10 kHz on medium wave) and the carriers
        const int edge = (int)((rate * 0.5 - half) / binHz);
        std::vector<float> sorted(power.begin() + edge, power.end() - edge);
        std::nth_element(sorted.begin(), sorted.begin() + (std::ptrdiff_t)sorted.size() / 4, sorted.end());
        const double floorP = sorted[sorted.size() / 4];
        const int peakHalf = std::max(1, (int)std::ceil(40.0 / binHz));
        for (int b = edge + peakHalf; b < N - edge - peakHalf; b++) {
            const float p = power[(size_t)b];
            if (p < floorP * std::pow(10.0, cfg.onDb / 10)) continue;
            bool top = true;
            for (int j = -peakHalf; j <= peakHalf && top; j++) if (power[(size_t)(b + j)] > p) top = false;
            if (!top) continue;
            const double hz = centre + (b - N / 2) * binHz;
            if (centre < 2e6) {
                const double r9 = std::fabs(std::remainder(hz, 9e3)), r10 = std::fabs(std::remainder(hz, 10e3));
                if (r9 < 60 && r10 > 300) rasterVotes9++;
                if (r10 < 60 && r9 > 300) rasterVotes10++;
            }
        }
        step = raster();
        const double f0 = std::ceil((centre - half) / step) * step;
        struct Cand { double hz; AmKind kind; bool present; double line, snr, band, loDb, upDb, spread; };
        std::vector<Cand> cand;
        for (double hz = f0; hz < centre + half; hz += step) {
            const int b = N / 2 + (int)std::lround((hz - centre) / binHz);
            const int w = std::max(1, (int)std::lround(60.0 / binHz));                 // the carrier: +-60 Hz
            double line = 0;
            for (int j = -w; j <= w; j++) if (b + j >= 0 && b + j < N) line = std::max(line, (double)power[(size_t)(b + j)]);
            const auto bins = [&](double hzFrom, double hzTo) { return band(b + (int)(hzFrom / binHz), b + (int)(hzTo / binHz)); };
            const double lo = bins(-4300, -250), up = bins(250, 4300);        // the first few kHz of the sidebands
            // the channel's floor: beside the channel, on the quieter side
            const double chFloor = std::max(floorP, std::min(bins(-12700, -11300), bins(11300, 12700)));
            const double around = 0.5 * (bins(-1000, -250) + bins(250, 1000));
            const double snr = 10 * std::log10(std::max(line / chFloor, 1e-3));
            const double prom = 10 * std::log10(std::max(line / std::max(around, 1e-30), 1e-3));
            const double loDb = 10 * std::log10(std::max(lo / chFloor, 1e-3)), upDb = 10 * std::log10(std::max(up / chFloor, 1e-3));
            const double spread = std::fabs(loDb - upDb), sideDb = 10 * std::log10(std::max((lo + up) * 0.5 / chFloor, 1e-3));
            const bool wideLo = bins(-9000, -4600) > chFloor * 2, wideUp = bins(4600, 9000) > chFloor * 2;
            Cand c{hz, AmKind::Am, false, line, snr, (lo + up) * 0.5, loDb, upDb, spread};
            if (snr >= cfg.onDb + 4 && prom >= 12) { c.kind = sideDb < 3 ? AmKind::Carrier : AmKind::Am; c.present = true; }   // a line (with nothing around it: a spur)
            else if (sideDb >= cfg.onDb - 2 && spread < 4 && wideLo && wideUp) { c.kind = AmKind::Drm; c.present = true; }     // no line, flat over the whole channel
            else if (std::max(loDb, upDb) >= cfg.onDb && spread >= 6) { c.kind = AmKind::Ssb; c.present = true; }
            cand.push_back(c);
        }
        for (size_t i = 0; i < cand.size(); i++) {
            const Cand& c = cand[i];
            if (!c.present) continue;
            if (c.kind == AmKind::Drm || c.kind == AmKind::Ssb) {
                // the channels beside a signal see parts of its spectrum: only the strongest of the neighbourhood is a signal, and nothing beside a carrier
                bool shadow = false;
                const size_t reach = (size_t)std::lround(10e3 / step);
                for (size_t j = i > reach ? i - reach : 0; j < std::min(cand.size(), i + reach + 1); j++) {
                    if (j == i || !cand[j].present) continue;
                    if (cand[j].kind == AmKind::Am || cand[j].band > c.band) shadow = true;
                }
                if (shadow) continue;
            }
            const double hz = c.hz;
            const long key = std::lround(hz / step);
            Ch& ct = ch[key];
            const float carDb = c.kind == AmKind::Am || c.kind == AmKind::Carrier ? db(c.line / nrm) : db(c.band / nrm);
            ct.car.push_back(carDb); ct.side.push_back(db(c.band / nrm));
            const size_t cap = (size_t)(kHistSec / kFrameSec);
            while (ct.car.size() > cap) { ct.car.pop_front(); ct.side.pop_front(); }
            ct.seen++;
            AmCensusRow& r = ct.row;
            r.khz = hz / 1e3; r.kind = c.kind; r.carrierDb = carDb; r.snrDb = (float)c.snr; r.lastSec = secs;
            if (ct.car.size() >= 40) {
                std::vector<float> v(ct.car.begin(), ct.car.end()), sv(ct.side.begin(), ct.side.end());
                std::sort(v.begin(), v.end()); std::sort(sv.begin(), sv.end());
                r.fadeDb = v[v.size() * 9 / 10] - v[v.size() / 10];
                const float sideFade = sv[sv.size() * 9 / 10] - sv[sv.size() / 10];
                r.fading = r.fadeDb > 6;
                r.selective = r.fading && sideFade < r.fadeDb * 0.5f;
            }
            r.ecss = c.kind == AmKind::Am && c.spread > 8 ? (c.loDb > c.upDb ? 1 : -1) : 0;   // the stronger side holds the interference: listen to the other
            r.score = c.kind == AmKind::Carrier ? -50.f : (c.kind == AmKind::Am ? (float)c.snr : (float)c.snr - 10) - 0.5f * r.fadeDb - (r.ecss ? 3.f : 0.f);
        }
        std::lock_guard<std::mutex> lk(mu);
        table.clear();
        for (auto& kv : ch) if (kv.second.seen >= 2 && secs - kv.second.row.lastSec < 10) table.push_back(kv.second.row);
        std::sort(table.begin(), table.end(), [](const AmCensusRow& a, const AmCensusRow& b) { return a.score > b.score; });
    }
};

AmCensus::AmCensus() : p_(std::make_unique<Impl>()) {}
AmCensus::~AmCensus() = default;
bool AmCensus::configure(double r, double c, const AmCensusConfig& cfg) { return p_->configure(r, c, cfg); }
void AmCensus::reset() { p_->reset(); }
void AmCensus::feed(const cf32* x, size_t n) { p_->feed(x, n); }
std::vector<AmCensusRow> AmCensus::rows() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->table; }
double AmCensus::stepHz() const { return p_->raster(); }

} // namespace dect2
