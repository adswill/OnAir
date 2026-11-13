// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NDB census (ndb_census.h). A 1 Hz-resolution spectrum is not needed: carriers on the 0.5 / 1 kHz grids are found with bins of about 16 Hz (the Morse sidebands
// sit 400 or 1020 Hz away and are lower); a carrier found in a scan gets a channel: a mixer, a decimator to 48 kHz and a decoder of its own.
#include "dect2/ndb/ndb_census.h"
#include "dect2/ndb/ndb_rx.h"
#include "dect2/ndb/ndb_tel.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kDecodeRate = 48000.0;  // the decoder's lowest input rate
constexpr int kMaxChannels = 40;
constexpr double kSpacing = 300.0;       // two carriers closer than this are one (sideband tones are 400 Hz and up)
constexpr double kPeriodSec = 2.0;

struct Channel {
    double f = 0;
    double phase = 0, step = 0;
    cf32 acc{0, 0};
    double count = 0, need = 1;
    std::vector<cf32> out;
    std::unique_ptr<NdbReceiver> rx;
    NdbCensusRow row;
    double lastSeen = 0;
};

} // namespace

struct NdbCensus::Impl {
    mutable std::mutex mu;
    double rate = 0, dialAt = 0, t = 0, nextScan = 0;
    int nfft = 0;
    std::vector<float> avg;
    std::vector<cf32> window, buf;
    int fill = 0, nAvg = 0;
    std::vector<std::unique_ptr<Channel>> ch;
    std::unique_ptr<Fft> fft;

    void setup(double r, double d) {
        rate = r; dialAt = d; t = 0; nextScan = kPeriodSec;
        nfft = 1;
        while ((double)nfft < r / 8.0) nfft <<= 1;   // bins about 8 Hz
        nfft = std::min(nfft, 1 << 17);
        fft = std::make_unique<Fft>(nfft);
        window.assign((size_t)nfft, cf32(0, 0));
        buf.assign((size_t)nfft, cf32(0, 0));
        avg.assign((size_t)nfft, 0.f);
        fill = 0; nAvg = 0;
        ch.clear();
    }

    void addSpectrum() {
        for (int i = 0; i < nfft; i++) {
            const float w = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / nfft);
            buf[(size_t)i] = window[(size_t)i] * w;
        }
        fft->forward(buf.data());
        for (int i = 0; i < nfft; i++) avg[(size_t)i] += std::norm(buf[(size_t)i]);
        nAvg++;
    }

    void scan() {
        if (nAvg < 2) return;
        std::vector<float> s((size_t)nfft);
        for (int i = 0; i < nfft; i++) s[(size_t)((i + nfft / 2) & (nfft - 1))] = avg[(size_t)i] / (float)nAvg;
        std::fill(avg.begin(), avg.end(), 0.f);
        nAvg = 0;
        std::vector<float> sorted(s);
        std::nth_element(sorted.begin(), sorted.begin() + nfft / 2, sorted.end());
        const float floorP = std::max(sorted[(size_t)(nfft / 2)], 1e-14f);
        const double binHz = rate / nfft;
        const int minDist = std::max(1, (int)(kSpacing / binHz));
        struct Pk { float p; int i; };
        std::vector<Pk> pk;
        for (int i = 2; i < nfft - 2; i++) {
            if (s[(size_t)i] < 16.f * floorP) continue;
            if (s[(size_t)i] >= s[(size_t)i - 1] && s[(size_t)i] > s[(size_t)i + 1]) pk.push_back({s[(size_t)i], i});
        }
        std::sort(pk.begin(), pk.end(), [](const Pk& a, const Pk& b) { return a.p > b.p; });
        std::vector<int> chosen;
        for (const auto& p : pk) {
            bool close = false;
            for (int c : chosen) {
                const double hz = std::fabs(c - p.i) * binHz;
                // the AM sidebands of a stronger carrier: 400 and 1020 Hz away, by the rule of the beacons
                if (std::abs(c - p.i) < minDist || std::fabs(hz - 400.0) < 25.0 || std::fabs(hz - 1020.0) < 25.0) { close = true; break; }
            }
            if (!close) chosen.push_back(p.i);
        }
        const int64_t wall = (int64_t)time(nullptr);
        for (int i : chosen) {
            const double f = (i - nfft / 2) * binHz;
            Channel* found = nullptr;
            for (auto& c : ch) if (std::fabs(c->f - f) < kSpacing * 0.5) { found = c.get(); break; }
            const float level = 10.f * std::log10(s[(size_t)i] + 1e-20f) - 10.f * std::log10((float)nfft * (float)nfft * 0.25f);
            if (!found) {
                if ((int)ch.size() >= kMaxChannels) continue;
                auto c = std::make_unique<Channel>();
                c->f = f;
                c->step = -2 * M_PI * f / rate;
                c->need = rate / kDecodeRate;
                c->rx = std::make_unique<NdbReceiver>();
                c->rx->setSilent(true);
                c->rx->configure(kDecodeRate);
                c->rx->setSignalOffset(0.0);
                found = c.get();
                ch.push_back(std::move(c));
            }
            found->row.offsetHz = f - dialAt;
            found->row.levelDb = level;
            found->row.snrDb = 10.f * std::log10(s[(size_t)i] / floorP);
            found->row.lastWall = wall;
            found->lastSeen = t;
        }
        // a channel not seen for a minute is given up
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](const std::unique_ptr<Channel>& c) { return t - c->lastSeen > 60.0; }), ch.end());
    }

    void chanFeed(Channel& c, const cf32* x, size_t n) {
        c.out.clear();
        for (size_t i = 0; i < n; i++) {
            const cf32 lo((float)std::cos(c.phase), (float)std::sin(c.phase));
            c.phase += c.step;
            if (c.phase > M_PI) c.phase -= 2 * M_PI; else if (c.phase < -M_PI) c.phase += 2 * M_PI;
            c.acc += x[i] * lo;
            c.count += 1;
            if (c.count >= c.need) {
                c.out.push_back(c.acc / (float)c.count);
                c.acc = cf32(0, 0);
                c.count -= c.need;
                if (c.count > 1) c.count = 0;
            }
        }
        if (!c.out.empty()) {
            c.rx->feed(c.out.data(), c.out.size());
            NdbTelemetry nt;
            if (c.rx->telemetry(nt, 0)) { if (!nt.ident.empty()) c.row.ident = nt.ident; c.row.toneHz = nt.toneHz; c.row.wpm = nt.wpm; }
        }
    }
};

NdbCensus::NdbCensus() : p_(std::make_unique<Impl>()) {}
NdbCensus::~NdbCensus() = default;

void NdbCensus::configure(double rate, double dialAt) { std::lock_guard<std::mutex> lk(p_->mu); p_->setup(rate, dialAt); }
void NdbCensus::clear() { std::lock_guard<std::mutex> lk(p_->mu); if (p_->rate > 0) p_->setup(p_->rate, p_->dialAt); }

void NdbCensus::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (m.rate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        m.window[(size_t)m.fill] = x[i];
        if (++m.fill == m.nfft) { m.addSpectrum(); m.fill = 0; }
    }
    m.t += (double)n / m.rate;
    if (m.t >= m.nextScan) { m.scan(); m.nextScan = m.t + kPeriodSec; }
    for (auto& c : m.ch) m.chanFeed(*c, x, n);
}

void NdbCensus::snapshot(std::vector<NdbCensusRow>& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    out.clear();
    for (const auto& c : p_->ch) out.push_back(c->row);
    std::sort(out.begin(), out.end(), [](const NdbCensusRow& a, const NdbCensusRow& b) { return a.offsetHz < b.offsetHz; });
}

} // namespace dect2
