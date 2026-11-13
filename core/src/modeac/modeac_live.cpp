// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C receiver for the engine (see modeac_live.h).
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_dsp.h"
#include "dect2/modeac/modeac_proto.h"
#include "dect2/modeac/modeac_tel.h"
#include "dect2/adsb/adsb_rx.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
struct Entry {
    uint16_t bits = 0;
    uint32_t count = 0;
    bool spi = false;
    double first = 0, last = 0;
    float levelDbfs = -120, snrDb = 0;
    bool announced = false;
};
}  // namespace

struct ModeAcReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0;
    std::atomic<bool> resetReq{false}, clearReq{false};
    std::atomic<int> confirm{2};
    std::atomic<double> expiry{60.0};
    AdsbReceiver adsb;
    ModeAcDemod demod;
    std::deque<std::pair<double, double>> maskIv;          // Mode S messages on the air (receiver's clock), oldest first
    std::map<uint16_t, Entry> codes;
    std::deque<double> recent;                              // accepted reply times, for the rate
    AdsbTelemetry lastAdsb; uint64_t adsbSeq = 0;
    uint64_t nIn = 0, nextPub = 0;
    double lastReply = -1e9;
    float snrAcc = 0;
    ModeAcTelemetry pub;

    Impl() {
        adsb.setFrameCallback([this](const AdsbFrame& f) {
            const double t = f.timeSec, d = (8.0 + f.bits) * 1e-6;
            maskIv.push_back({t - 1e-6, t + d + 1e-6});
        });
        demod.setMask([this](double a, double b) {
            for (const auto& iv : maskIv) if (iv.first < b && iv.second > a) return true;
            return false;
        });
        demod.setSink([this](const ModeAcDemod::Raw& r) { onReply(r); });
    }

    std::function<void(const ModeAcDemod::Raw&)> replyCb;
    void onReply(const ModeAcDemod::Raw& r) {
        if (replyCb) replyCb(r);
        Entry& e = codes[r.bits];
        if (e.count == 0) { e.bits = r.bits; e.first = r.timeSec; }
        e.count++; e.last = r.timeSec; e.spi = r.spi; e.levelDbfs = r.levelDbfs; e.snrDb = r.snrDb;
        recent.push_back(r.timeSec);
        lastReply = r.timeSec;
        snrAcc += 0.1f * (r.snrDb - snrAcc);
        if (log && !e.announced && (int)e.count >= confirm.load()) {
            e.announced = true;
            const int sq = modeac::squawkOf(r.bits);
            if (modeac::isEmergencySquawk(sq) && !modeac::altitudeOf(r.bits).valid) {
                char b[120];
                snprintf(b, sizeof b, "Mode A/C: squawk %04d (%s)", sq, sq == 7500 ? "hijack" : sq == 7600 ? "radio failure" : "general emergency");
                log(b);
            }
        }
    }

    void housekeeping(double now) {
        while (!maskIv.empty() && maskIv.front().second < now - 0.5) maskIv.pop_front();
        while (!recent.empty() && recent.front() < now - 5.0) recent.pop_front();
        const double ex = expiry.load(), pend = std::min(20.0, ex);
        const int conf = confirm.load();
        for (auto it = codes.begin(); it != codes.end();) {
            const double age = now - it->second.last;
            if (age > ((int)it->second.count >= conf ? ex : pend)) it = codes.erase(it); else ++it;
        }
    }

    void publish(double now) {
        pub.seq++;
        const int conf = confirm.load();
        pub.timeSec = now;
        pub.noiseDbfs = (float)demod.noiseDbfs();
        pub.candidates = demod.candidates(); pub.masked = demod.masked();
        pub.blocksOk = demod.accepted(); pub.blocksBad = demod.rejectedShape();
        pub.repliesPerSec = (float)recent.size() / 5.f;
        pub.snrDb = snrAcc;
        pub.cfoHz = 0;
        std::vector<const Entry*> shown;
        uint64_t pend = 0;
        for (const auto& kv : codes) { if ((int)kv.second.count >= conf) shown.push_back(&kv.second); else pend++; }
        std::sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) { return a->last > b->last; });
        pub.pending = pend;
        pub.levelDbfs = shown.empty() ? -120.f : shown.front()->levelDbfs;
        pub.adsb = lastAdsb;
        pub.modeSGood = lastAdsb.blocksOk;
        pub.codes.clear();
        pub.matched = 0;
        for (const Entry* e : shown) {
            if (pub.codes.size() >= kModeAcMax) break;
            ModeAcCode c;
            c.bits = e->bits; c.squawk = modeac::squawkOf(e->bits);
            const modeac::Altitude alt = modeac::altitudeOf(e->bits);
            c.altValid = alt.valid; c.altFt = alt.feet;
            c.spi = e->spi; c.count = e->count; c.levelDbfs = e->levelDbfs; c.snrDb = e->snrDb;
            c.ageSec = (float)(now - e->last); c.firstAgeSec = (float)(now - e->first);
            if (!alt.valid) { c.kind = 1; c.how = 1; }
            const AdsbAircraft* hit = nullptr;
            for (const auto& ac : lastAdsb.aircraft) if (ac.hasSquawk && ac.squawk == c.squawk && ac.ageSec < 120) { hit = &ac; break; }
            if (hit) { c.kind = 1; c.how = 3; }
            else if (alt.valid) {
                int n = 0;
                for (const auto& ac : lastAdsb.aircraft) if (ac.hasAlt && ac.ageSec < 30 && std::abs(ac.altFt - alt.feet) <= 200) { n++; hit = &ac; }
                if (n == 1) { c.kind = 2; c.how = 4; } else hit = nullptr;
            }
            if (hit) { c.icao = hit->icao; c.callsign = hit->callsign; pub.matched++; }
            pub.codes.push_back(std::move(c));
        }
        pub.codeCount = (uint32_t)shown.size();
        pub.state = (now - lastReply < 10.0) ? 2 : (lastAdsb.state == 2 ? 1 : 0);
        pub.dataValid = pub.state == 2;
    }

    void resetState() {
        demod.configure(rateSet > 0 ? rateSet : 2e6);
        adsb.reset();
        maskIv.clear(); recent.clear();
        nIn = 0; nextPub = 0;
    }
};

ModeAcReceiver::ModeAcReceiver() : p_(std::make_unique<Impl>()) {}
ModeAcReceiver::~ModeAcReceiver() = default;

void ModeAcReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = inputRateHz;
    p_->adsb.configure(inputRateHz);
    p_->resetReq = true;
}
void ModeAcReceiver::setSignalOffset(double) {}
bool ModeAcReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet >= modeacTuning().minSampleRate - 1;
}
void ModeAcReceiver::reset() { p_->resetReq = true; }
void ModeAcReceiver::clearResults() { p_->clearReq = true; }
void ModeAcReceiver::setReference(double la, double lo) { p_->adsb.setReference(la, lo); }
void ModeAcReceiver::setConfirm(int n) { p_->confirm = std::max(1, std::min(5, n)); }
void ModeAcReceiver::setExpiry(double s) { p_->expiry = std::max(10.0, std::min(600.0, s)); p_->adsb.setExpiry(s); }
void ModeAcReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void ModeAcReceiver::setReplyCallback(std::function<void(const ModeAcDemod::Raw&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->replyCb = std::move(cb); }
bool ModeAcReceiver::telemetry(ModeAcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void ModeAcReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.resetState();
        m.publish(m.demod.timeSec());
    }
    if (m.clearReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.codes.clear();
        m.adsb.clearResults();
        m.lastAdsb = AdsbTelemetry();
        m.publish(m.demod.timeSec());
    }
    if (n == 0 || m.rateSet < modeacTuning().minSampleRate - 1) return;
    m.nIn += n;
    m.adsb.feed(x, n);      // first: the messages it decodes mask the detector's candidates
    m.demod.feed(x, n);
    if (m.nIn >= m.nextPub) {
        m.nextPub = m.nIn + (uint64_t)(0.25 * m.rateSet);
        AdsbTelemetry a;
        const bool got = m.adsb.telemetry(a, m.adsbSeq);
        std::lock_guard<std::mutex> lk(m.mu);
        if (got) { m.adsbSeq = a.seq; m.lastAdsb = std::move(a); }
        const double now = m.demod.timeSec();
        m.housekeeping(now);
        m.publish(now);
    }
}

ModeTuning modeacTuning() {
    ModeTuning t;
    t.stdMode = 144; t.id = "modeac"; t.name = "Mode A/C";
    t.minMhz = 1085; t.maxMhz = 1095; t.defMhz = 1090.0;
    t.sampleRate = 4000000.0; t.basebandHz = 3500000.0; t.bandwidthMhz = 2;   // as ADS-B: the same band, the same radio settings
    t.minSampleRate = 2000000.0;
    return t;
}

} // namespace dect2
