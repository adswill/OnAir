// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP for the engine: see kapp_live.h.
#include "dect2/kapp/kapp_live.h"
#include "dect2/kapp/kapp_arq.h"
#include "dect2/kapp/kapp_rx.h"
#include "dect2/kapp/kapp_tel.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

constexpr size_t kMaxPackets = 32;
constexpr double kBurstTimeoutSec = 5.0;       // no burst for this long: back to "listening"
constexpr double kPartialTimeoutSec = 30.0;    // an incomplete message nobody continues is dropped
constexpr double kRepeatWindowSec = 120.0;     // a complete message identical to one delivered this recently is a repeat

uint64_t fnv(const std::string& call, const std::vector<uint8_t>& m) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    for (char c : call) mix((uint8_t)c);
    mix(0);
    for (uint8_t b : m) mix(b);
    return h;
}

uint64_t fnvBytes(const std::vector<uint8_t>& m) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : m) { h ^= b; h *= 1099511628211ull; }
    return h;
}

void makePreview(const std::vector<uint8_t>& m, std::string& out, bool& binary) {
    const size_t n = std::min<size_t>(m.size(), 160);
    size_t odd = 0;
    out.clear();
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = m[i];
        if (c >= 0x20 && c < 0x7F) out += (char)c;
        else if (c == '\n' || c == '\r' || c == '\t') out += ' ';
        else { out += '.'; odd++; }
    }
    binary = n > 0 && odd * 10 > n;
}

} // namespace

struct KappLiveReceiver::Impl {
    std::mutex feedMu;                       // the processing state below
    std::mutex pubMu;                        // the published telemetry and the callback
    std::unique_ptr<kapp::BurstReceiver> rx;
    double rate = 0;
    bool ready = false;
    uint64_t samples = 0, nextReport = 0, seq = 0;

    struct Partial {
        kapp::ArqReceiver arq;
        double lastSec = 0;
        explicit Partial(const std::string& c) : arq(c) {}
    };
    std::map<std::string, Partial> partial;
    struct Done { uint64_t hash; double sec; };
    std::deque<Done> done;
    std::deque<std::pair<double, uint32_t>> goodBytes;   // (stream time, payload bytes of the blocks that passed)

    KappTelemetry cur;                       // the counts and lists (feed thread)
    double lastBurstSec = -1e9;
    bool wasReceiving = false;
    KappTelemetry pub;
    std::function<void(const std::string&)> log;

    double now() const { return rate > 0 ? (double)samples / rate : 0.0; }

    void startStream() {
        kapp::BurstReceiver::Config c;
        c.sampleRate = rate;
        rx = std::make_unique<kapp::BurstReceiver>(c);
        samples = 0; nextReport = (uint64_t)(0.25 * rate);
        partial.clear(); done.clear(); goodBytes.clear();
        cur = KappTelemetry();
        cur.profile = -1;
        lastBurstSec = -1e9;
        wasReceiving = false;
    }

    void onBurst(const kapp::RxBurst& b) {
        const double t = now();
        lastBurstSec = t;
        cur.callsign = b.callsign;
        cur.profile = b.profile;
        cur.snrDb = b.snrDb;
        cur.cfoHz = b.cfoHz;
        if (b.isAck()) { cur.acks++; return; }
        cur.bursts++;
        if (b.profile >= 0 && b.profile < 9) cur.profileBursts[b.profile]++;
        const kapp::Profile* P = kapp::profileById(b.profile);
        if (P) {
            cur.constellationBits = P->bitsPerSym;
            cur.constellation.clear();
            for (const kapp::cf& s : b.symbols) cur.constellation.emplace_back(s.real(), s.imag());
        }
        const int ok = b.blocksOk();
        cur.blocksOk += (uint64_t)ok;
        cur.blocksBad += (uint64_t)(b.nBlocks - ok);
        if (P && ok > 0) goodBytes.emplace_back(t, (uint32_t)((size_t)ok * (size_t)P->payloadBytes));
        auto it = partial.find(b.callsign);
        if (it == partial.end()) it = partial.emplace(b.callsign, Partial(b.callsign)).first;
        it->second.lastSec = t;
        it->second.arq.onBurst(b);   // the ACK content is not sent: this receiver only listens
        if (!it->second.arq.complete()) return;
        const std::vector<uint8_t> msg = it->second.arq.message();
        partial.erase(it);
        const uint64_t h = fnv(b.callsign, msg);
        while (!done.empty() && t - done.front().sec > kRepeatWindowSec) done.pop_front();
        for (const Done& d : done) {
            if (d.hash == h) { cur.repeats++; return; }
        }
        done.push_back(Done{h, t});
        KappPacket p;
        p.callsign = b.callsign;
        p.profile = b.profile;
        p.bytes = (uint32_t)msg.size();
        p.contentHash = fnvBytes(msg);
        p.atSec = t;
        p.snrDb = b.snrDb;
        makePreview(msg, p.preview, p.binary);
        cur.messages++;
        cur.bytesDelivered += msg.size();
        cur.packets.insert(cur.packets.begin(), p);
        if (cur.packets.size() > kMaxPackets) cur.packets.resize(kMaxPackets);
        std::function<void(const std::string&)> lg;
        { std::lock_guard<std::mutex> pk(pubMu); lg = log; }
        if (lg) lg("KAPP message from " + p.callsign + ", " + std::to_string(p.bytes) + " bytes");
    }

    void report() {
        const double t = now();
        for (auto it = partial.begin(); it != partial.end();) {
            if (t - it->second.lastSec > kPartialTimeoutSec) it = partial.erase(it); else ++it;
        }
        while (!goodBytes.empty() && t - goodBytes.front().first > 10.0) goodBytes.pop_front();
        double bytes = 0;
        for (const auto& g : goodBytes) bytes += g.second;
        const bool receiving = t - lastBurstSec <= kBurstTimeoutSec;
        KappTelemetry o = cur;
        o.seq = ++seq;
        o.state = receiving ? 2 : samples ? 1 : 0;
        o.inputRate = rate;
        o.dataValid = receiving && o.blocksOk > 0;
        o.timeSec = t;
        o.lastBurstAgeSec = lastBurstSec > -1e8 ? std::max(0.0, t - lastBurstSec) : 0.0;
        const kapp::RxStats& st = rx->stats();
        o.detections = st.detections; o.headerOk = st.headerOk; o.headerFail = st.headerFail;
        o.throughputBps = bytes * 8.0 / std::max(1.0, std::min(10.0, t));
        o.partialMessages = (uint32_t)partial.size();
        o.partialBytes = 0;
        for (const auto& kv : partial) o.partialBytes += (uint32_t)kv.second.arq.bytesReceived();
        std::function<void(const std::string&)> lg;
        {
            std::lock_guard<std::mutex> lk(pubMu);
            pub = std::move(o);
            lg = log;
        }
        if (lg && receiving != wasReceiving) lg(receiving ? "KAPP bursts found" : "KAPP bursts stopped");
        wasReceiving = receiving;
    }
};

KappLiveReceiver::KappLiveReceiver() : p_(std::make_unique<Impl>()) {}
KappLiveReceiver::~KappLiveReceiver() = default;

void KappLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.ready = r >= kappTuning().minSampleRate - 1;
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KappTelemetry();
    m.pub.profile = -1;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = r;
}

bool KappLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KappLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KappTelemetry();
    m.pub.profile = -1;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = m.rate;
}

void KappLiveReceiver::clearPackets() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.cur.packets.clear();
    m.cur.messages = 0; m.cur.repeats = 0; m.cur.bytesDelivered = 0;
    m.cur.bursts = 0; m.cur.acks = 0; m.cur.blocksOk = 0; m.cur.blocksBad = 0;
    for (auto& v : m.cur.profileBursts) v = 0;
    m.partial.clear();
    m.done.clear();
}

void KappLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (!m.ready) return;
    m.rx->feed(x, n);
    m.samples += n;
    for (const kapp::RxBurst& b : m.rx->takeBursts()) m.onBurst(b);
    if (m.samples >= m.nextReport) {
        m.nextReport = m.samples + (uint64_t)(0.25 * m.rate);
        m.report();
    }
}

bool KappLiveReceiver::telemetry(KappTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->pubMu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void KappLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->pubMu); p_->log = std::move(cb); }

ModeTuning kappTuning() {
    ModeTuning t;
    t.stdMode = 126; t.id = "kapp"; t.name = "KAPP";
    t.minMhz = 30; t.maxMhz = 6000; t.defMhz = 433;
    t.sampleRate = 250000; t.bandwidthMhz = 0.1; t.minSampleRate = 125000;
    return t;
}

} // namespace dect2
