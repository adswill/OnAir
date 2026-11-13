// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DGPS beacons in the engine: mixes the beacon to 0 Hz, decimates to about 4.8 kHz, feeds dgps::Receiver one second at a time.
#include "dect2/dgps/dgps_engine.h"
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTargetFs = 4800.0;     // the decoder's working rate (12 samples per bit at 200 bit/s is 2400; 4800 keeps some slack)
}

struct DgpsReceiver::Impl {
    mutable std::mutex mu;
    std::vector<dgps::Receiver> rxs{dgps::Receiver(200), dgps::Receiver(100), dgps::Receiver(50)};   // the beacons' bit rates (25 bit/s is not used any more)
    size_t best = 0;
    double rate = 0;          // input sample rate
    int decim = 1;            // input samples averaged into one working sample
    double fsEff = 0;         // working sample rate after decimation
    double offsetHz = 0;      // beacon position in the input (0 Hz is the dial)
    double centerHz = 0;
    double phase = 0;         // mixer phase, radians
    std::complex<float> acc{0, 0};
    int accN = 0;
    std::vector<std::complex<float>> block;   // one second of working samples
    uint64_t seq = 0;
    uint64_t framesSeen = 0;
    DgpsTelemetry snap;
    std::function<void(const std::string&)> log;

    void emit(const std::string& s) { if (log) log(s); }

    static std::string describe(const dgps::Message& m) {
        char b[200];
        std::string t;
        switch (m.h.type) {
        case 1: case 9: {
            snprintf(b, sizeof b, "%s: %zu satellites", m.h.type == 1 ? "corrections, all satellites" : "corrections, partial set", m.sats.size());
            t = b;
            for (size_t i = 0; i < m.sats.size() && i < 4; i++) { snprintf(b, sizeof b, "%s G%02d %+.2f m %+.3f m/s", i ? ";" : ":", m.sats[i].sat, m.sats[i].prc(), m.sats[i].rrc()); t += b; }
            if (m.sats.size() > 4) t += " ...";
            break;
        }
        case 3:
            if (m.hasPosition) { snprintf(b, sizeof b, "reference station %.5f %.5f, %.1f m (ECEF %.2f %.2f %.2f)", m.lat, m.lon, m.height, m.x, m.y, m.z); t = b; }
            else t = "reference station position";
            break;
        case 5: snprintf(b, sizeof b, "constellation health, %zu satellites", m.satHealth.size()); t = b; break;
        case 6: t = "null frame (fill)"; break;
        case 7:
            snprintf(b, sizeof b, "beacon almanac, %zu beacons", m.almanac.size()); t = b;
            for (size_t i = 0; i < m.almanac.size() && i < 3; i++) { snprintf(b, sizeof b, "%s %.1f kHz id %d %d bit/s %d km", i ? ";" : ":", m.almanac[i].frequencyKhz, m.almanac[i].stationId, m.almanac[i].bitRate, m.almanac[i].rangeKm); t += b; }
            break;
        case 16: t = "special message: " + m.text; break;
        default: snprintf(b, sizeof b, "message type %d (%d words)", m.h.type, m.h.length); t = b;
        }
        if (!m.complete) t += " [word error]";
        return t;
    }

    void clearState() {
        for (auto& r : rxs) r.reset();
        best = 0;
        block.clear();
        acc = {0, 0};
        accN = 0;
        phase = 0;
        framesSeen = 0;
        snap = DgpsTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
    }

    void processBlock() {
        for (auto& r : rxs) r.process(block.data(), block.size(), fsEff);
        block.clear();
        for (size_t i = 0; i < rxs.size(); i++) {   // the receiver that finds the most messages wins (ties keep the faster rate)
            const dgps::Stats& a = rxs[i].stats(); const dgps::Stats& b = rxs[best].stats();
            if (a.messagesComplete * 4 + a.framesOk > b.messagesComplete * 4 + b.framesOk) best = i;
        }
        const dgps::Stats& st = rxs[best].stats();
        snap.cfoHz = st.carrierOffsetHz;
        snap.levelDb = (float)st.levelDb;
        snap.blocksOk = (uint64_t)st.framesOk;
        snap.dataValid = st.framesOk > 0;
        snap.state = st.framesOk > 0 ? 2 : 1;
        snap.typesSeen.assign(st.typesSeen.begin(), st.typesSeen.end());
        snap.bitRate = st.bitRate;
        snap.inverted = st.inverted;
        snap.messagesComplete = (uint64_t)st.messagesComplete;
        snap.wordsBad = (uint64_t)st.wordsBad;
        if ((uint64_t)st.framesOk != framesSeen) {
            framesSeen = (uint64_t)st.framesOk;
            snap.stationId = st.last.stationId;
            snap.lastType = st.last.type;
            snap.zcount = st.last.zcount;
            snap.health = st.last.health;
            snap.rows.clear();
            for (size_t i = st.messages.size(); i-- > 0;) {
                const dgps::Message& m = st.messages[i];
                DgpsMessageRow r;
                r.type = m.h.type; r.station = m.h.stationId; r.zcount = m.h.zcount; r.seq = m.h.seq; r.length = m.h.length; r.health = m.h.health; r.complete = m.complete;
                r.text = describe(m);
                snap.rows.push_back(r);
                if (!m.complete) continue;
                if (m.hasPosition && !snap.hasPosition) { snap.hasPosition = true; snap.lat = m.lat; snap.lon = m.lon; snap.height = m.height; }
                if (m.h.type == 7 && snap.almanac.empty())
                    for (const auto& a : m.almanac) { DgpsBeacon bc; bc.latitude = a.latitude; bc.longitude = a.longitude; bc.frequencyKhz = a.frequencyKhz; bc.rangeKm = a.rangeKm; bc.stationId = a.stationId; bc.bitRate = a.bitRate; bc.health = a.health; snap.almanac.push_back(bc); }
                if (m.h.type == 16 && snap.specialText.empty()) snap.specialText = m.text;
                if ((m.h.type == 1 || m.h.type == 9) && !snap.satellites) snap.satellites = (int)m.sats.size();
            }
            char b[120];
            snprintf(b, sizeof b, "DGPS: frame %llu, station %d, type %d, %.0f bit/s", (unsigned long long)framesSeen, st.last.stationId, st.last.type, st.bitRate);
            emit(b);
        }
        snap.seq = ++seq;
    }
};

DgpsReceiver::DgpsReceiver() : p_(new Impl) {}
DgpsReceiver::~DgpsReceiver() = default;

void DgpsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = inputRateHz;
    p_->decim = inputRateHz > kTargetFs ? (int)std::lround(inputRateHz / kTargetFs) : 1;
    if (p_->decim < 1) p_->decim = 1;
    p_->fsEff = inputRateHz / p_->decim;
    p_->clearState();
}

void DgpsReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->offsetHz = hz;
}

void DgpsReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->centerHz = hz;
    p_->snap.centerHz = hz;
}

bool DgpsReceiver::ready() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->rate >= dgpsTuning().minSampleRate && p_->fsEff > 0;
}

void DgpsReceiver::reset() {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->clearState();
}

void DgpsReceiver::feed(const std::complex<float>* x, size_t n) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->fsEff <= 0) return;
    const double step = -2.0 * kPi * p_->offsetHz / p_->rate;   // mixes the beacon at offsetHz down to 0 Hz
    const size_t want = (size_t)std::llround(p_->fsEff);        // one second per block
    for (size_t i = 0; i < n; i++) {
        const std::complex<float> m = x[i] * std::complex<float>((float)std::cos(p_->phase), (float)std::sin(p_->phase));
        p_->phase += step;
        if (p_->phase > kPi) p_->phase -= 2 * kPi;
        if (p_->phase < -kPi) p_->phase += 2 * kPi;
        p_->acc += m;
        if (++p_->accN == p_->decim) {
            p_->block.push_back(p_->acc / (float)p_->decim);
            p_->acc = {0, 0};
            p_->accN = 0;
            if (p_->block.size() >= want) p_->processBlock();
        }
    }
}

bool DgpsReceiver::telemetry(DgpsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.inputRate = p_->rate;
    out.centerHz = p_->centerHz;
    return true;
}

void DgpsReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->log = std::move(cb);
}

ModeTuning dgpsTuning() {
    ModeTuning t;
    t.stdMode = 122;
    t.id = "dgps";
    t.name = "DGPS beacon";
    t.minMhz = 0.2835;
    t.maxMhz = 0.325;
    t.defMhz = 0.300;
    t.sampleRate = 960000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.02;
    t.minSampleRate = 48000;
    t.tuneOffsetHz = 50000;     // the radio sits 50 kHz above the beacon, the receiver mixes it out
    return t;
}

std::string dgpsSummary(const DgpsTelemetry& t) {
    char b[160];
    if (t.state == 0) return "DGPS: waiting for samples";
    if (!t.dataValid) {
        snprintf(b, sizeof b, "DGPS: listening (%.0f dBFS)", t.levelDb);
        return b;
    }
    snprintf(b, sizeof b, "DGPS: station %d, type %d, %llu frames", t.stationId, t.lastType, (unsigned long long)t.blocksOk);
    return b;
}

}  // namespace dect2
