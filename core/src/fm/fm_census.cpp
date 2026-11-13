// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/fm/fm_census.h"
#include "dect2/common/threadpool.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/fm/fm_rx.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr int kM = 4096;                // inverse FFT per station
constexpr double kPass = 110e3, kStop = 190e3;
constexpr double kAvgSec = 0.5;         // one detection pass per this much signal
constexpr double kDropSec = 8;          // a station that fell below the threshold is dropped after this long
}

struct FmCensus::Impl {
    double rate = 0, centre = 0, binHz = 0, outRate = 0;
    FmCensusConfig cfg;
    int N = 0, D = 1, maxSt = 8;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> in, block;
    std::vector<float> win, power, mask;
    size_t have = 0;
    uint64_t frame = 0;
    int avgN = 0, avgWant = 1;
    double secs = 0;
    struct Station {
        double mhz = 0; int bin = 0;
        float level = -200, above = 0, lastAbove = 0;
        double lastOn = 0;
        std::unique_ptr<FmReceiver> rx;
        Fft ifft{kM};
        std::vector<cf32> sp;
        uint64_t seq = 0;
    };
    std::vector<std::unique_ptr<Station>> st;
    mutable std::mutex mu;
    std::vector<FmCensusRow> table;
    std::atomic<int> nDecoding{0};

    bool configure(double fs, double c, const FmCensusConfig& conf) {
        rate = fs; centre = c; cfg = conf;
        if (fs < 1e6) return false;
        D = 1; while (fs / (D * 2) >= 500e3) D *= 2;
        outRate = fs / D;
        N = kM * D;
        binHz = fs / N;
        fft = std::make_unique<Fft>(N);
        in.assign((size_t)N, cf32(0, 0)); block.resize((size_t)N);
        win.resize((size_t)N);
        for (int i = 0; i < N; i++) win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / N);
        power.assign((size_t)N, 0.f);
        // the station's channel filter in the frequency domain: flat to +-110 kHz, a raised cosine to zero at +-190 kHz
        mask.resize(kM);
        for (int k = 0; k < kM; k++) {
            const double f = std::fabs((k < kM / 2 ? k : k - kM) * binHz);
            mask[(size_t)k] = f <= kPass ? 1.f : f >= kStop ? 0.f : (float)(0.5 + 0.5 * std::cos(M_PI * (f - kPass) / (kStop - kPass)));
        }
        maxSt = cfg.maxStations > 0 ? cfg.maxStations : std::max(8, 8 * (int)std::thread::hardware_concurrency());
        avgWant = std::max(1, (int)std::lround(kAvgSec * fs / (N / 2.0)));
        reset();
        return true;
    }

    void reset() {
        have = 0; frame = 0; avgN = 0; secs = 0;
        std::fill(power.begin(), power.end(), 0.f);
        st.clear();
        std::lock_guard<std::mutex> lk(mu);
        table.clear();
    }

    void feed(const cf32* x, size_t n) {
        if (!fft) return;
        while (n) {
            const size_t take = std::min(n, (size_t)N - have);
            std::copy(x, x + take, in.begin() + (std::ptrdiff_t)have);
            have += take; x += take; n -= take;
            if (have == (size_t)N) {
                process();
                std::copy(in.begin() + N / 2, in.end(), in.begin());
                have = (size_t)N / 2;
            }
        }
    }

    void process() {
        // the block as it is (the mask does the filtering) for the stations, windowed for the spectrum
        for (int i = 0; i < N; i++) block[(size_t)i] = std::isfinite(in[(size_t)i].real() + in[(size_t)i].imag()) ? in[(size_t)i] : cf32(0, 0);
        fft->forward(block.data());
        for (auto& s : st) {
            s->sp.resize(kM);
            for (int k = 0; k < kM; k++) {
                const int r = k < kM / 2 ? k : k - kM;   // relative bin
                s->sp[(size_t)k] = mask[(size_t)k] > 0 ? block[(size_t)((s->bin + r + N) % N)] * mask[(size_t)k] : cf32(0, 0);
            }
        }
        const bool flip = frame & 1;   // the hop is N/2: an odd bin turns by 180 degrees from one block to the next
        ThreadPool::shared().parallelFor(st.size(), [&](size_t i) {
            Station& s = *st[i];
            s.ifft.inverse(s.sp.data());
            cf32* y = s.sp.data() + kM / 2;     // overlap-save: the second half is free of the circular wrap
            const float g = (flip && (s.bin & 1) ? -1.f : 1.f) / (float)kM;
            for (int k = 0; k < kM / 2; k++) y[k] *= g;
            s.rx->feed(y, kM / 2);
        });
        frame++;
        secs += (N / 2.0) / rate;
        // the spectrum for finding stations: the magnitude-squared of the unwindowed FFT would smear a strong station across the band; every fourth
        // frame is windowed apart (that is enough for an average over half a second)
        if ((frame & 3) == 0 || avgWant < 4) {
            for (int i = 0; i < N; i++) block[(size_t)i] = in[(size_t)i] * win[(size_t)i];
            fft->forward(block.data());
            for (int k = 0; k < N; k++) { const float p = std::norm(block[(size_t)k]); power[(size_t)((k + N / 2) % N)] += std::isfinite(p) ? p : 0.f; }
            avgN++;
        }
        if ((int)(frame % (uint64_t)avgWant) == 0 && avgN > 0) { detect(); avgN = 0; std::fill(power.begin(), power.end(), 0.f); }
        publish();
    }

    void detect() {
        // the score of a raster point: the median power over +-75 kHz (an FM channel is flat there; the radio's DC spike or a narrow carrier is not)
        const double step = 100e3, firstHz = std::ceil((centre - rate * 0.5) / step) * step;
        const int half = std::max(2, (int)(75e3 / binHz));
        const double lim = 0.46 * rate - kPass;
        struct C { double hz; float db; };
        std::vector<C> cells;
        std::vector<float> w;
        const double norm = std::max(1.0, (double)avgN) * N * N * 0.375 / (200e3 / binHz);   // dB full scale in a 200 kHz channel
        for (double hz = firstHz; hz < centre + rate * 0.5; hz += step) {
            const double off = hz - centre;
            if (std::fabs(off) > lim) continue;
            const int b = N / 2 + (int)std::lround(off / binHz);
            w.clear();
            // groups of about 2 kHz, so that the lines of a tone-modulated or sparse programme do not vanish between the bins
            const int grp = std::max(1, (int)(2000.0 / binHz));
            for (int j = b - half; j + grp <= b + half + 1; j += grp) {
                if (j < 0 || j + grp > N) continue;
                float sum = 0; for (int q = 0; q < grp; q++) sum += power[(size_t)(j + q)];
                w.push_back(sum / (float)grp);
            }
            if (w.empty()) continue;
            std::nth_element(w.begin(), w.begin() + (std::ptrdiff_t)(w.size() / 2), w.end());
            cells.push_back({hz, (float)(10 * std::log10(std::max(w[w.size() / 2] / norm, 1e-20)))});
        }
        if (cells.size() < 3) return;
        std::vector<float> s;
        for (const auto& c : cells) s.push_back(c.db);
        std::sort(s.begin(), s.end());
        const float floorDb = s[s.size() / 10];
        struct Found { double hz; float db; };
        std::vector<Found> found;
        for (size_t i = 0; i < cells.size(); i++) {
            const float me = cells[i].db;
            if (me - floorDb < (float)cfg.onDb) continue;
            if ((i > 0 && cells[i - 1].db > me) || (i + 1 < cells.size() && cells[i + 1].db >= me)) continue;   // the top of the station, its skirts are not stations
            found.push_back({cells[i].hz, me});
        }
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.db > b.db; });
        for (const auto& f : found) {
            Station* have_ = nullptr;
            for (auto& x : st) if (std::fabs(x->mhz * 1e6 - f.hz) < 1e3) have_ = x.get();
            if (have_) { have_->level = f.db; have_->above = f.db - floorDb; have_->lastOn = secs; continue; }
            if ((int)st.size() >= maxSt) {
                auto w_ = std::min_element(st.begin(), st.end(), [](const auto& a, const auto& b) { return a->level < b->level; });
                if ((*w_)->level + 6 > f.db) continue;
                st.erase(w_);
            }
            auto n = std::make_unique<Station>();
            n->mhz = f.hz / 1e6; n->bin = (int)std::lround((f.hz - centre) / binHz);
            n->level = f.db; n->above = f.db - floorDb; n->lastOn = secs;
            n->rx = std::make_unique<FmReceiver>();
            n->rx->setAnalysisOnly(true); n->rx->setSilent(true); n->rx->setStationFinder(false);
            n->rx->configure(outRate);
            n->rx->setRdsRegion(cfg.rdsRegion, n->mhz);
            st.push_back(std::move(n));
        }
        st.erase(std::remove_if(st.begin(), st.end(), [&](const auto& x) { return secs - x->lastOn > kDropSec; }), st.end());
    }

    void publish() {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : table) r.active = false;
        for (auto& s : st) {
            auto it = std::find_if(table.begin(), table.end(), [&](const FmCensusRow& r) { return std::fabs(r.mhz - s->mhz) < 0.001; });
            if (it == table.end()) { FmCensusRow r; r.mhz = s->mhz; table.push_back(r); it = table.end() - 1; }
            FmCensusRow& r = *it;
            r.active = true; r.levelDb = s->level; r.aboveDb = s->above; r.lastSeenSec = secs;
            FmTelemetry t;
            if (!s->rx->telemetry(t, s->seq)) continue;
            s->seq = t.seq;
            r.snrDb = t.snrDb; r.stereo = t.stereo; r.rds = t.rdsSync; r.tp = t.trafficProgram; r.ta = t.trafficAlert; r.music = t.music;
            if (t.piCode) r.pi = t.piCode;
            if (t.rds.ecc >= 0) r.ecc = t.rds.ecc;
            if (!t.psName.empty()) r.ps = t.psName;
            if (!t.radioText.empty()) r.rt = t.radioText;
            if (!t.ptyText.empty()) r.pty = t.ptyText;
            if (!t.rds.afMhz.empty()) r.afMhz = t.rds.afMhz;
        }
        std::sort(table.begin(), table.end(), [](const FmCensusRow& a, const FmCensusRow& b) { return a.mhz < b.mhz; });
        nDecoding = (int)st.size();
    }
};

FmCensus::FmCensus() : p_(std::make_unique<Impl>()) {}
FmCensus::~FmCensus() = default;
bool FmCensus::configure(double rate, double centreHz, const FmCensusConfig& cfg) { return p_->configure(rate, centreHz, cfg); }
bool FmCensus::ready() const { return p_->fft != nullptr; }
void FmCensus::reset() { p_->reset(); }
void FmCensus::feed(const cf32* x, size_t n) { p_->feed(x, n); }
std::vector<FmCensusRow> FmCensus::rows() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->table; }
int FmCensus::stationsDecoding() const { return p_->nDecoding.load(); }

} // namespace dect2
