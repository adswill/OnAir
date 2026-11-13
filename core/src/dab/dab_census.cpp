// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dab/dab_census.h"
#include "dect2/dab/dab.h"
#include "dect2/dsp/carrierfind.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/threadpool.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace dect2 {

const std::vector<DabChannelPlan>& dabBandIII() {
    static const std::vector<DabChannelPlan> v = {
        {"5A", 174.928}, {"5B", 176.640}, {"5C", 178.352}, {"5D", 180.064}, {"6A", 181.936}, {"6B", 183.648}, {"6C", 185.360}, {"6D", 187.072},
        {"7A", 188.928}, {"7B", 190.640}, {"7C", 192.352}, {"7D", 194.064}, {"8A", 195.936}, {"8B", 197.648}, {"8C", 199.360}, {"8D", 201.072},
        {"9A", 202.928}, {"9B", 204.640}, {"9C", 206.352}, {"9D", 208.064}, {"10A", 209.936}, {"10N", 210.096}, {"10B", 211.648}, {"10C", 213.360}, {"10D", 215.072},
        {"11A", 216.928}, {"11N", 217.088}, {"11B", 218.640}, {"11C", 220.352}, {"11D", 222.064}, {"12A", 223.936}, {"12N", 224.096}, {"12B", 225.648}, {"12C", 227.360}, {"12D", 229.072},
        {"13A", 230.784}, {"13B", 232.496}, {"13C", 234.208}, {"13D", 235.776}, {"13E", 237.488}, {"13F", 239.200}};
    return v;
}

std::vector<DabCensusMerged> dabCensusMerge(const std::vector<DabCensusRow>& rows) {
    std::map<uint32_t, DabCensusMerged> m;
    for (const auto& r : rows) {
        if (!r.locked) continue;
        for (const auto& s : r.services) {
            auto it = m.find(s.sid);
            if (it == m.end()) {
                DabCensusMerged d; d.sid = s.sid; d.label = s.label; d.dabPlus = s.dabPlus; d.bitrate = s.bitrate; d.channel = r.channel; d.mhz = r.mhz; d.snrDb = r.snrDb;
                m[s.sid] = d;
            } else {
                DabCensusMerged& d = it->second;
                if (r.snrDb > d.snrDb) { d.alsoOn.push_back(d.channel); d.channel = r.channel; d.mhz = r.mhz; d.snrDb = r.snrDb; if (!s.label.empty()) d.label = s.label; d.bitrate = s.bitrate; d.dabPlus = s.dabPlus; }
                else d.alsoOn.push_back(r.channel);
            }
        }
    }
    std::vector<DabCensusMerged> v;
    for (auto& kv : m) v.push_back(std::move(kv.second));
    std::sort(v.begin(), v.end(), [](const DabCensusMerged& a, const DabCensusMerged& b) { return a.label != b.label ? a.label < b.label : a.sid < b.sid; });
    return v;
}

struct DabCensus::Impl {
    struct Slot {
        int idx = 0;
        Channelizer ch;
        std::unique_ptr<DabReceiver> rx;
        std::vector<cf32> out;
        DabTelemetry tel;
        uint64_t seq = 0;
    };
    struct Chan { const DabChannelPlan* p = nullptr; double relHz = 0; double power = 0; DabCensusRow row; };
    mutable std::mutex mu;
    DabCensusConfig cfg;
    double rate = 0, centre = 0, now = 0;
    bool ok = false;
    std::vector<Chan> ch;
    std::vector<std::unique_ptr<Slot>> slots;
    int nfft = 0, nAvg = 0, done = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win; std::vector<cf32> frame, work; std::vector<double> acc; size_t fill = 0;

    void spectrum() {
        const double hz = rate / nfft;
        std::vector<double> bins((size_t)nfft);
        for (int i = 0; i < nfft; i++) bins[(size_t)i] = acc[(size_t)((i + nfft / 2) % nfft)] / done;
        std::vector<double> sorted = bins; std::sort(sorted.begin(), sorted.end());
        double floorV = sorted[(size_t)(sorted.size() * 0.15)];
        for (auto& c : ch) {
            const int a = (int)((c.relHz - 768000.0) / hz) + nfft / 2, b = (int)((c.relHz + 768000.0) / hz) + nfft / 2;
            double s = 0; int n = 0;
            for (int i = std::max(0, a); i <= std::min(nfft - 1, b); i++) { s += bins[(size_t)i]; n++; }
            c.power = n ? s / n : 0;
            c.row.levelDb = (float)(10 * std::log10(std::max(c.power, 1e-20)));
            c.row.aboveDb = (float)(10 * std::log10(std::max(c.power / std::max(floorV, 1e-20), 1e-6)));
        }
        // the strongest channels over the threshold get a receiver; a channel that fell under for good leaves when a newcomer needs the place
        std::vector<int> order;
        for (size_t i = 0; i < ch.size(); i++) {
            if (ch[i].row.aboveDb < cfg.onDb) continue;
            bool beaten = false;     // 10N, 11N, 12N overlap the A channel beside them: one signal can only be one of them, the N one only when clearly centred there
            for (size_t j = 0; j < ch.size(); j++) if (j != i && std::fabs(ch[i].relHz - ch[j].relHz) < 400000.0) {
                const bool iN = ch[i].row.channel.back() == 'N';
                if (ch[j].power * (iN ? 1.2 : 1.0) > ch[i].power && (iN || ch[j].row.channel.back() != 'N')) beaten = true;
            }
            if (!beaten) order.push_back((int)i);
        }
        std::sort(order.begin(), order.end(), [&](int x, int y) { return ch[(size_t)x].power > ch[(size_t)y].power; });
        if ((int)order.size() > cfg.maxEnsembles) order.resize((size_t)cfg.maxEnsembles);
        for (int idx : order) {
            bool have = false; for (auto& s : slots) if (s->idx == idx) have = true;
            if (have) continue;
            if ((int)slots.size() >= cfg.maxEnsembles) {
                auto w = std::min_element(slots.begin(), slots.end(), [&](const auto& x, const auto& y) { return ch[(size_t)x->idx].power < ch[(size_t)y->idx].power; });
                if (ch[(size_t)(*w)->idx].power * 4 > ch[(size_t)idx].power) continue;     // 6 dB better to take a place
                DIAG_DEBUG(424167, "The DAB census gives the receiver of channel %s to channel %s, which is over 6 dB stronger.", ch[(size_t)(*w)->idx].row.channel.c_str(), ch[(size_t)idx].row.channel.c_str());
                ch[(size_t)(*w)->idx].row.active = false;
                slots.erase(w);
            }
            auto s = std::make_unique<Slot>();
            s->idx = idx; s->rx.reset(new DabReceiver());
            s->rx->configure(2048000.0);
            s->rx->audio().setSilent(true);
            if (!s->ch.configure(rate, 2048000.0, ch[(size_t)idx].relHz, 1712000.0)) {
                DIAG_DEBUG(518968, "The channelizer for DAB channel %s (%.0f Hz from the centre) did not configure at %.0f samples per second, so it is not decoded.",
                           ch[(size_t)idx].row.channel.c_str(), ch[(size_t)idx].relHz, rate);
                continue;
            }
            ch[(size_t)idx].row.active = true;
            slots.push_back(std::move(s));
        }
    }
    void feed(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> g(mu);
        if (!ok) return;
        size_t i = 0;
        while (i < n) {
            const size_t k = std::min(n - i, (size_t)nfft - fill);
            std::copy(x + i, x + i + k, frame.begin() + (long)fill);
            fill += k; i += k;
            if (fill == (size_t)nfft) {
                fill = 0;
                for (int b = 0; b < nfft; b++) work[(size_t)b] = frame[(size_t)b] * win[(size_t)b];
                fft->forward(work.data());
                for (int b = 0; b < nfft; b++) acc[(size_t)b] += std::norm(work[(size_t)b]);
                if (++done >= nAvg) { spectrum(); std::fill(acc.begin(), acc.end(), 0.0); done = 0; }
            }
        }
        now += (double)n / rate;
        ThreadPool::shared().parallelFor(slots.size(), [&](size_t j) {
            Slot& s = *slots[j]; s.out.clear();
            s.ch.process(x, n, s.out);
            if (!s.out.empty()) s.rx->feed(s.out.data(), s.out.size());
        });
        for (auto& sp : slots) {
            Slot& s = *sp;
            if (!s.rx->telemetry(s.tel, s.seq)) continue;
            s.seq = s.tel.seq;
            DabCensusRow& r = ch[(size_t)s.idx].row;
            r.locked = s.tel.state == 2; r.snrDb = (float)s.tel.snrDb; r.lastSeenSec = now;
            if (s.tel.state == 2) {
                const DabEnsemble e = s.rx->ensemble();
                if (e.valid) {
                    r.eid = e.eid; r.ensemble = e.label; r.services.clear();
                    for (const auto& kv : e.services) {
                        DabCensusService v; v.sid = kv.first; v.label = kv.second.label;
                        for (const auto& c : kv.second.comps) { auto it = e.subs.find(c.subId); if (it != e.subs.end()) { v.bitrate = it->second.bitrate; v.dabPlus = c.ascty == 63; break; } }
                        r.services.push_back(v);
                    }
                }
                r.tii.clear();
                for (const auto& t : s.tel.tii) r.tii.push_back({t.mainId, t.subId});
            }
        }
    }
};

DabCensus::DabCensus() : p_(new Impl()) {}
DabCensus::~DabCensus() = default;

bool DabCensus::configure(double rate, double centreHz, const DabCensusConfig& c) {
    std::lock_guard<std::mutex> g(p_->mu);
    auto& P = *p_;
    P.cfg = c; P.rate = rate; P.centre = centreHz; P.now = 0; P.slots.clear(); P.ch.clear(); P.ok = false; P.done = 0; P.fill = 0;
    if (rate < 3.0e6) { DIAG_DEBUG(807841, "The DAB census needs at least 3 Msps for two channels, so it does not run at %.0f samples per second.", rate); return false; }
    for (const auto& pl : dabBandIII()) {
        const double rel = pl.mhz * 1e6 - centreHz;
        if (std::fabs(rel) + 900000.0 > 0.5 * rate) continue;
        Impl::Chan k; k.p = &pl; k.relHz = rel; k.row.channel = pl.name; k.row.mhz = pl.mhz;
        P.ch.push_back(k);
    }
    if (P.ch.empty()) {
        DIAG_DEBUG(700237, "No Band III DAB channel fits within %.0f samples per second around %.3f MHz, so the census does not run.", rate, centreHz / 1e6);
        return false;
    }

    P.nfft = 4096; P.nAvg = std::max(4, (int)(0.2 * rate / P.nfft));
    P.fft.reset(new Fft(P.nfft));
    P.win.resize((size_t)P.nfft);
    for (int i = 0; i < P.nfft; i++) P.win[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * (i + 0.5) / P.nfft);
    P.frame.assign((size_t)P.nfft, cf32(0, 0)); P.work = P.frame; P.acc.assign((size_t)P.nfft, 0.0);
    P.ok = true;
    return true;
}
bool DabCensus::ready() const { std::lock_guard<std::mutex> g(p_->mu); return p_->ok; }
void DabCensus::reset() { std::lock_guard<std::mutex> g(p_->mu); p_->slots.clear(); p_->now = 0; for (auto& c : p_->ch) c.row = [&] { DabCensusRow r; r.channel = c.p->name; r.mhz = c.p->mhz; return r; }(); }
void DabCensus::feed(const cf32* x, size_t n) { p_->feed(x, n); }
std::vector<DabCensusRow> DabCensus::rows() const { std::lock_guard<std::mutex> g(p_->mu); std::vector<DabCensusRow> v; for (const auto& c : p_->ch) if (c.row.locked || c.row.active || !c.row.ensemble.empty()) v.push_back(c.row); return v; }
int DabCensus::ensemblesDecoding() const { std::lock_guard<std::mutex> g(p_->mu); return (int)p_->slots.size(); }

} // namespace dect2
