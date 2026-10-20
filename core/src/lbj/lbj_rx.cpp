// LBJ receiver: the dedicated 1200 bit/s FSK modem (lbj_modem.h), POCSAG codewords (lbj_pocsag.h) and the message layer (lbj_proto.h).
#include "dect2/lbj/lbj_rx.h"
#include "dect2/lbj/lbj_modem.h"
#include "dect2/lbj/lbj_pocsag.h"
#include "dect2/lbj/lbj_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

namespace dect2 {

ModeTuning lbjTuning() {
    ModeTuning t;
    t.stdMode = 83;
    t.id = "lbj";
    t.name = "LBJ";
    t.minMhz = 821.0;
    t.maxMhz = 821.5;
    t.defMhz = lbj::kDefaultMhz;
    t.sampleRate = 960000;
    t.basebandHz = 150000;                   // the radio's analogue filter must pass the carrier 50 kHz off its centre (+-4.5 kHz deviation, 6 kHz channel)
    t.bandwidthMhz = 0.035;
    t.minSampleRate = 256000;
    t.tuneOffsetHz = 50000;
    return t;
}

struct LbjReceiver::Impl {
    std::mutex mu;
    LbjTelemetry tel;
    std::function<void(const std::string&)> log;
    lbj::Modem modem;
    lbj::LbjSessionState sessions;
    std::vector<lbj::Burst> bursts;
    double fs = 0, offHz = 0, centreHz = 0, lastPublish = 0, lastMsgSec = -1e9;
    bool rateSet = false;

    void pushMessage(LbjMessage m, const lbj::Burst& b) {
        m.wallTime = (int64_t)std::time(nullptr);
        m.rssiDb = b.levelDb;
        m.afcHz = b.cfoHz;
        if (log) {
            char line[320];
            snprintf(line, sizeof line, "LBJ %s km %s %s km/h", m.train.c_str(), m.kmMarker.empty() ? "-" : m.kmMarker.c_str(), m.speedKmh.empty() ? "-" : m.speedKmh.c_str());
            log(line);
        }
        if (tel.messages.size() >= kLbjLogMax) tel.messages.pop_back();
        tel.messages.insert(tel.messages.begin(), std::move(m));
        tel.dataValid = true;
        tel.blocksOk++;
        lastMsgSec = modem.timeSec();
    }

    void ingest(const lbj::Burst& b) {
        tel.bursts++;
        for (const lbj::Page& p : lbj::pagesFromBurst(b)) {
            if (!lbj::isLbjAddress(p.ric)) {
                sessions.basicSec = sessions.detailSec = -1e9;
                continue;
            }
            LbjMessage m;
            const uint64_t rej = sessions.rejected;
            if (lbj::parsePage(p, b.timeSec, sessions, m)) pushMessage(std::move(m), b);
            else if (sessions.rejected != rej) tel.blocksBad++;
        }
        tel.emptyFrames = sessions.emptyFrames;
    }

    void publish() {
        const double now = modem.timeSec();
        tel.levelDb = (float)modem.levelDb();
        tel.snrDb = (float)modem.snrDb();
        tel.timeSec = now;
        tel.blocks = modem.syncs();
        tel.state = rateSet ? ((now - lastMsgSec) < 30.0 ? 2 : 1) : 0;
        if (!tel.messages.empty()) tel.cfoHz = tel.messages.front().afcHz;
        tel.seq++;
    }
};

LbjReceiver::LbjReceiver() : p_(std::make_unique<Impl>()) {}
LbjReceiver::~LbjReceiver() = default;

void LbjReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= lbjTuning().minSampleRate - 1;
    if (p_->rateSet) {
        p_->modem.configure(inputRateHz);
        p_->modem.setOffset(p_->offHz);
    }
    p_->tel.inputRate = inputRateHz;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void LbjReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offHz = hz;
    p_->modem.setOffset(hz);
}
void LbjReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->centreHz = hz;
    p_->tel.centerHz = hz;
}
bool LbjReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet;
}
void LbjReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rateSet) p_->modem.reset();
    p_->sessions = lbj::LbjSessionState();
    p_->lastPublish = 0;
    p_->lastMsgSec = -1e9;
    const uint64_t seq = p_->tel.seq;
    p_->tel = LbjTelemetry();
    p_->tel.seq = seq + 1;
    p_->tel.inputRate = p_->fs;
    p_->tel.centerHz = p_->centreHz;
    p_->tel.state = p_->rateSet ? 1 : 0;
}
void LbjReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear();
    p_->tel.dataValid = false;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void LbjReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) {
        p_->tel.state = 0;
        return;
    }
    p_->bursts.clear();
    p_->modem.feed(x, n, p_->bursts);
    for (const lbj::Burst& b : p_->bursts) p_->ingest(b);
    const double now = p_->modem.timeSec();
    if (!p_->bursts.empty() || now - p_->lastPublish >= 0.25) {
        p_->lastPublish = now;
        p_->publish();
    }
}
bool LbjReceiver::telemetry(LbjTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void LbjReceiver::clearResults() { clearMessages(); }

// Results file: one line per message, tab separated: train, km post, loco, speed, direction, wall time.
std::string LbjReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string s = "lbj3\n";
    for (const auto& m : p_->tel.messages) {
        char position[96]; snprintf(position, sizeof position, "%.10f\t%.10f", m.latitude, m.longitude);
        s += m.train + '\t' + m.kmMarker + '\t' + m.loco + '\t' + m.speedKmh + '\t' + m.direction + '\t' + std::to_string((long long)m.wallTime)
            + '\t' + m.route + '\t' + m.locoCode + '\t' + m.locoSerial + '\t' + m.prefix + '\t' + m.trainBase + '\t' + m.cabEnd + '\t' + m.unknownTail + '\t' + m.networkTime
            + '\t' + position + '\t' + std::to_string(m.positionValid) + '\t' + std::to_string(m.paired) + '\t' + std::to_string(m.timeSync) + '\t' + std::to_string(m.detailed)
            + '\t' + std::to_string(m.address) + '\t' + std::to_string(m.correctedBits) + '\t' + std::to_string(m.crcOk) + '\n';
    }
    return s;
}
bool LbjReceiver::loadResults(const std::string& blob) {
    const bool v2 = blob.compare(0, 5, "lbj2\n") == 0;
    const bool v3 = blob.compare(0, 5, "lbj3\n") == 0;
    if (!v2 && !v3 && blob.compare(0, 5, "lbj1\n") != 0) return false;
    std::vector<LbjMessage> msgs;
    size_t i = 5;
    while (i < blob.size() && msgs.size() < kLbjLogMax) {
        size_t e = blob.find('\n', i);
        if (e == std::string::npos) e = blob.size();
        std::vector<std::string> f;
        size_t a = i;
        for (;;) {
            const size_t t = blob.find('\t', a);
            if (t == std::string::npos || t >= e) { f.push_back(blob.substr(a, e - a)); break; }
            f.push_back(blob.substr(a, t - a));
            a = t + 1;
        }
        if (f.size() >= 3) {
            LbjMessage m;
            m.train = f[0];
            m.kmMarker = f[1];
            m.loco = f[2];
            if ((v2 || v3) && f.size() >= 6) { m.speedKmh = f[3]; m.direction = f[4]; m.wallTime = std::atoll(f[5].c_str()); }
            if (v3) {
                if (f.size() != 23) return false;
                m.route = f[6]; m.locoCode = f[7]; m.locoSerial = f[8]; m.prefix = f[9]; m.trainBase = f[10]; m.cabEnd = f[11]; m.unknownTail = f[12]; m.networkTime = f[13];
                m.latitude = std::atof(f[14].c_str()); m.longitude = std::atof(f[15].c_str());
                m.positionValid = f[16] == "1" && std::isfinite(m.latitude) && std::isfinite(m.longitude) && m.latitude > 0 && m.latitude <= 60 && m.longitude >= 70 && m.longitude <= 140;
                m.paired = f[17] == "1"; m.timeSync = f[18] == "1"; m.detailed = f[19] == "1"; m.address = (uint32_t)std::strtoul(f[20].c_str(), nullptr, 10); m.correctedBits = std::atoi(f[21].c_str()); m.crcOk = f[22] == "1";
            }
            msgs.push_back(std::move(m));
        }
        i = e + 1;
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages = std::move(msgs);
    p_->tel.dataValid = !p_->tel.messages.empty();
    p_->tel.seq++;
    return true;
}
void LbjReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
bool LbjReceiver::busy() const { return false; }

} // namespace dect2
