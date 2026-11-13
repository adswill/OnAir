// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus receiver (horus_rx.h): the channels, the wide-span search, the payload table and the results file.
#include "dect2/horus/horus_rx.h"
#include "horus_modem.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>

namespace dect2 {

ModeTuning horusTuning() {
    ModeTuning t;
    t.stdMode = horus::kStandard;
    t.id = "horus"; t.name = "Horus balloons";
    t.minMhz = 400; t.maxMhz = 450; t.defMhz = 434.660;
    t.sampleRate = 960000; t.bandwidthMhz = 0.9; t.minSampleRate = 16000; t.tuneOffsetHz = 30000;
    return t;
}

namespace {
constexpr int kMaxPackets = 200, kMaxTrack = 3000;
constexpr int kMaxPayloads = 64;

// the packet's time of day on the receive date (UTC); a time just before midnight heard just after it is yesterday's
double packetUnix(const horus::Packet& p) {
    const time_t now = (time_t)p.wallTime;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    const double day = (double)now - (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec);
    double t = day + p.hour * 3600 + p.minute * 60 + p.second;
    if (t > (double)now + 12 * 3600) t -= 86400;
    else if (t < (double)now - 12 * 3600) t += 86400;
    return t;
}
} // namespace

struct HorusReceiver::Impl {
    mutable std::mutex mu;
    HorusTelemetry tel;
    horus::Tables tables;
    std::vector<std::unique_ptr<horus::Channel>> ch;
    horus::WideDetector wide;
    struct Cand { double hz; int seen; };
    std::vector<Cand> cands;
    double rate = 0, chRate = 0, offset = 0, center = 0, t = 0, lastPublish = -1, nextSearch = 2, lastPacket = -1e9;
    horus::Cic2 pre;            // above 2.4 MS/s the input is first brought to 2 - 4 MS/s once for every channel
    std::vector<cf32> preOut;
    int maxChannels = 6;
    bool ready = false, wideOn = true;
    std::function<void(const std::string&)> log;

    Impl() {
        horus::parseIdList(horus::defaultIdList(), tables.ids);
        horus::parseCustomFields(horus::defaultCustomFields(), tables.formats);
    }
    void build() {
        ch.clear(); cands.clear();
        if (!ready) return;
        pre.init(rate > 2.4e6 ? (int)std::floor(rate / 2e6) : 1);
        chRate = rate / pre.decimation();
        auto c = std::make_unique<horus::Channel>();
        c->init(chRate, offset, false);
        ch.push_back(std::move(c));
        wide.init(chRate);
    }
    bool wideUsable() const { return wideOn && maxChannels > 1 && rate >= 40000; }
    void addPacket(horus::Packet&& k, int channel) {
        k.channel = channel;
        if (k.wallTime <= 0) k.wallTime = (int64_t)std::time(nullptr);
        // the same packet from two channels or two framers
        for (const auto& q : tel.packets) {
            if (k.wallTime - q.wallTime > 30) break;
            if (q.callsign == k.callsign && q.seq == k.seq && q.kind == k.kind) return;
        }
        tel.packets.insert(tel.packets.begin(), k);
        if ((int)tel.packets.size() > kMaxPackets) tel.packets.pop_back();
        tel.blocksOk++;
        auto it = std::find_if(tel.payloads.begin(), tel.payloads.end(), [&](const HorusPayload& p) { return p.callsign == k.callsign; });
        if (it == tel.payloads.end()) {
            HorusPayload p; p.callsign = k.callsign; p.firstHeard = (double)k.wallTime;
            tel.payloads.push_back(std::move(p));
            it = tel.payloads.end() - 1;
            if ((int)tel.payloads.size() > kMaxPayloads) { tel.payloads.erase(std::min_element(tel.payloads.begin(), tel.payloads.end(), [](const HorusPayload& a, const HorusPayload& b) { return a.lastHeard < b.lastHeard; })); it = std::find_if(tel.payloads.begin(), tel.payloads.end(), [&](const HorusPayload& p) { return p.callsign == k.callsign; }); }
        }
        HorusPayload& p = *it;
        p.payloadId = k.payloadId; p.packets++; p.lastHeard = (double)k.wallTime;
        const bool fix = !(k.lat == 0 && k.lon == 0);
        if (fix) {
            HorusTrackPoint tp; tp.unixT = packetUnix(k); tp.lat = k.lat; tp.lon = k.lon; tp.altM = (float)k.altM;
            if (p.track.empty() || tp.unixT >= p.track.back().unixT) p.track.push_back(tp);
            if ((int)p.track.size() > kMaxTrack) p.track.erase(p.track.begin());
            p.maxAltM = std::max(p.maxAltM, k.altM);
            // climb rate: the oldest point at least 30 s back within the last 3 minutes
            p.hasAscent = false;
            for (auto r = p.track.rbegin(); r != p.track.rend(); ++r) {
                const double dt = tp.unixT - r->unixT;
                if (dt > 180) break;
                if (dt >= 30) { p.ascentMs = (tp.altM - r->altM) / dt; p.hasAscent = true; }
            }
        }
        p.last = std::move(k);
        // most recently heard first
        std::stable_sort(tel.payloads.begin(), tel.payloads.end(), [](const HorusPayload& a, const HorusPayload& b) { return a.lastHeard > b.lastHeard; });
        tel.dataValid = true;
        lastPacket = t;
        if (log) log(horusSummary(tel));
    }
    void search() {
        if (!wideUsable() || !wide.ready()) return;
        // the radio's centre (DC) is not a balloon; after the shared decimator its band edges are aliased
        const auto hits = wide.hits(0, offset != 0 ? 1500 : 0, pre.decimation() > 1 ? 0.3 * chRate : 0.45 * chRate);
        std::vector<Cand> next;
        for (const auto& h : hits) {
            bool covered = false;
            for (const auto& c : ch) if (std::fabs(c->mixer() - h.hz) < 3500) covered = true;
            if (covered) continue;
            int seen = 1;
            for (const auto& c : cands) if (std::fabs(c.hz - h.hz) < 400) seen = c.seen + 1;
            next.push_back({h.hz, seen});
        }
        cands = next;
        for (auto& c : cands) {
            if (c.seen < 2 || (int)ch.size() >= maxChannels) continue;
            bool covered = false;
            for (const auto& q : ch) if (std::fabs(q->mixer() - c.hz) < 3500) covered = true;
            if (covered) continue;
            auto nc = std::make_unique<horus::Channel>();
            nc->init(chRate, c.hz, true);
            nc->createdSec = t; nc->lastSeenSec = t;
            ch.push_back(std::move(nc));
            if (log) { char b[96]; snprintf(b, sizeof b, "Horus: a signal %+.1f kHz from the dial, channel %zu opened", (c.hz - offset) / 1e3, ch.size() - 1); log(b); }
        }
        // channels that never decoded or went quiet, and whose signal is gone
        for (size_t i = 1; i < ch.size();) {
            bool still = false;
            for (const auto& h : hits) if (std::fabs(h.hz - ch[i]->mixer()) < 3500) still = true;
            if (still) ch[i]->lastSeenSec = t;
            const double quiet = t - std::max(ch[i]->lastDecodeSec, ch[i]->createdSec);
            if (quiet > 120 && t - ch[i]->lastSeenSec > 30) ch.erase(ch.begin() + (ptrdiff_t)i); else i++;
        }
    }
    void publish() {
        tel.timeSec = t;
        tel.state = !ready ? 0 : t - lastPacket < 30 ? 2 : 1;
        tel.channels.clear();
        uint64_t syncs = 0, fails = 0;
        float best = -99;
        for (const auto& c : ch) {
            HorusChannelInfo ci;
            ci.offsetHz = c->mixer() - offset;
            ci.dynamic = c->dynamic();
            if (c->fsk4().hasTones()) { ci.ntones = 4; for (int k = 0; k < 4; k++) ci.tones[k] = c->fsk4().tones()[k]; }
            ci.rtty[0] = c->rttyTones[0]; ci.rtty[1] = c->rttyTones[1];
            ci.spacingHz = c->toneSpacing();
            ci.snrDb = c->snrDb; ci.mode = c->mode; ci.packets = c->packets; ci.syncs = c->syncs(); ci.failures = c->failures();
            syncs += ci.syncs; fails += ci.failures;
            if (t - c->lastDecodeSec < 60) best = std::max(best, c->snrDb);
            tel.channels.push_back(ci);
        }
        tel.syncs = syncs; tel.blocksBad = fails; tel.snrDb = best > -99 ? best : 0;
        const double nowWall = (double)std::time(nullptr);
        for (auto& p : tel.payloads) { p.lastHeardS = std::max(0.0, nowWall - p.lastHeard); p.active = p.lastHeardS < 120; }
        tel.spectrumDb.clear();
        if (!ch.empty() && ch[0]->estimator().primed()) {   // the dial channel's spectrum, about 256 points over +-4 kHz
            const auto& e = ch[0]->estimator();
            const auto& pw = e.power();
            const float fl = e.floorDb();
            const int n = e.size(), lo = n / 2 - (int)(0.42 * ch[0]->outRate() / e.binHz()), hi = n - lo;
            const int step = std::max(1, (hi - lo) / 256);
            for (int i = lo; i + step <= hi; i += step) {
                float m = 0;
                for (int k = 0; k < step; k++) m = std::max(m, pw[(size_t)(i + k)]);
                tel.spectrumDb.push_back(10.f * std::log10(m + 1e-30f) - fl);
            }
            tel.specLoHz = (lo - n / 2) * e.binHz(); tel.specStepHz = step * e.binHz();
        }
        tel.seq++;
    }
};

HorusReceiver::HorusReceiver() : p_(std::make_unique<Impl>()) {}
HorusReceiver::~HorusReceiver() = default;

void HorusReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->ready = std::isfinite(rate) && rate >= horusTuning().minSampleRate;
    p_->rate = rate; p_->tel.inputRate = rate; p_->t = 0; p_->lastPublish = -1; p_->nextSearch = 2;
    p_->build();
    p_->tel.state = p_->ready ? 1 : 0; p_->tel.seq++;
}
void HorusReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->offset = hz;
    if (!p_->ch.empty()) p_->ch[0]->setMixer(hz);
}
void HorusReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lock(p_->mu); p_->center = hz; p_->tel.centerHz = hz; }
bool HorusReceiver::ready() const { std::lock_guard<std::mutex> lock(p_->mu); return p_->ready; }
void HorusReceiver::reset() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->t = 0; p_->lastPublish = -1; p_->nextSearch = 2; p_->lastPacket = -1e9;
    p_->build();
    p_->tel.state = p_->ready ? 1 : 0; p_->tel.seq++;
}
void HorusReceiver::clearResults() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->tel.payloads.clear(); p_->tel.packets.clear(); p_->tel.blocksOk = 0; p_->tel.dataValid = false; p_->tel.seq++;
}
void HorusReceiver::setMaxChannels(int n) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->maxChannels = std::clamp(n, 1, 8); p_->tel.maxChannels = p_->maxChannels;
    while ((int)p_->ch.size() > p_->maxChannels) p_->ch.pop_back();
}
void HorusReceiver::setWideSearch(bool on) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->wideOn = on; p_->tel.wide = on;
    if (!on) while (p_->ch.size() > 1) p_->ch.pop_back();
}
std::string HorusReceiver::setTables(const std::string& ids, const std::string& fields) {
    horus::Tables t;
    std::string err;
    const int bad = horus::parseIdList(ids.empty() ? horus::defaultIdList() : ids, t.ids);
    if (bad > 0) err = std::to_string(bad) + " line(s) of the ID list could not be read";
    std::string jerr;
    if (!horus::parseCustomFields(fields.empty() ? horus::defaultCustomFields() : fields, t.formats, &jerr)) {
        if (!err.empty()) err += "; ";
        err += "custom fields: " + jerr;
        horus::parseCustomFields(horus::defaultCustomFields(), t.formats);
    }
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->tables = std::move(t);
    return err;
}

void HorusReceiver::feed(const cf32* iq, size_t n) {
    if (!iq || !n) return;
    std::lock_guard<std::mutex> lock(p_->mu);
    if (!p_->ready || p_->ch.empty()) return;
    Impl& s = *p_;
    const cf32* x = iq;
    size_t m = n;
    if (s.pre.decimation() > 1) {
        s.preOut.clear();
        s.pre.process(iq, n, s.preOut);
        x = s.preOut.data(); m = s.preOut.size();
    }
    std::vector<horus::Packet> found;
    if (m > 0) for (size_t i = 0; i < s.ch.size(); i++) {
        found.clear();
        s.ch[i]->feed(x, m, s.tables, s.offset, found);
        for (auto& k : found) s.addPacket(std::move(k), (int)i);
    }
    if (s.wideUsable() && m > 0) s.wide.push(x, m);
    s.t += (double)n / s.rate;
    if (s.t >= s.nextSearch) { s.nextSearch = s.t + 1.0; s.search(); }
    if (s.t - s.lastPublish >= 0.25) { s.lastPublish = s.t; s.publish(); }
}

bool HorusReceiver::telemetry(HorusTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lock(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

// results: one line per packet, oldest first: wall time, kind, the payload in hex (binary) or the sentence (RTTY)
std::string HorusReceiver::saveResults() const {
    std::lock_guard<std::mutex> lock(p_->mu);
    std::string s = "horus1\n";
    const char* h = "0123456789abcdef";
    for (auto it = p_->tel.packets.rbegin(); it != p_->tel.packets.rend(); ++it) {
        s += std::to_string(it->wallTime) + '\t' + std::to_string(it->kind) + '\t';
        if (it->kind == 0) s += it->sentence;
        else for (auto b : it->raw) { s += h[b >> 4]; s += h[b & 15]; }
        s += '\n';
    }
    return s;
}
bool HorusReceiver::loadResults(const std::string& blob) {
    if (blob.compare(0, 7, "horus1\n") != 0) return false;
    std::istringstream in(blob.substr(7));
    std::string line;
    std::vector<horus::Packet> list;
    horus::Tables tables;
    { std::lock_guard<std::mutex> lock(p_->mu); tables = p_->tables; }
    while (std::getline(in, line) && list.size() < (size_t)kMaxPackets) {
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) return false;
        const int64_t wall = std::strtoll(line.c_str(), nullptr, 10);
        const int kind = std::atoi(line.c_str() + a + 1);
        const std::string body = line.substr(b + 1);
        horus::Packet k;
        if (kind == 0) { if (!horus::parseSentence(body, tables, k)) return false; }
        else {
            if (body.size() != 44 && body.size() != 64) return false;
            std::vector<uint8_t> raw;
            for (size_t i = 0; i < body.size(); i += 2) raw.push_back((uint8_t)std::strtoul(body.substr(i, 2).c_str(), nullptr, 16));
            if (!horus::parseBinary(raw.data(), (int)raw.size(), tables, k)) return false;
        }
        k.wallTime = wall;
        list.push_back(std::move(k));
    }
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->tel.packets.clear(); p_->tel.payloads.clear(); p_->tel.blocksOk = 0;
    const double t = p_->t;
    auto log = std::move(p_->log);   // a loaded file is not news
    p_->log = nullptr;
    for (auto& k : list) p_->addPacket(std::move(k), 0);
    p_->log = std::move(log);
    p_->lastPacket = t - 1e9;   // loaded, not heard now
    p_->tel.seq++;
    return true;
}
void HorusReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lock(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
