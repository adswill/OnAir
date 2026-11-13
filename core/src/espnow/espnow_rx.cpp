// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW receiver (espnow_rx.h): the Wi-Fi demodulator's frames, the ESP-NOW ones kept as devices and messages.
#include "dect2/espnow/espnow_rx.h"
#include "dect2/common/diag.h"
#include "dect2/espnow/espnow_proto.h"
#include "dect2/espnow/espnow_tel.h"
#include "dect2/results/results_io.h"
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dect2 {

EspnowPlan espnowPlan(double centerHz) {
    EspnowPlan p;
    if (centerHz <= 0) return p;
    const double c = centerHz / 1e6;
    double bd = 1e9;
    for (int ch = 1; ch <= 14; ch++) {
        const double d = wifi::channelMhz(ch) - c;
        if (std::fabs(d) <= 12 && std::fabs(d) < bd) { bd = std::fabs(d); p.channel = ch; p.offsetHz = d * 1e6; }
    }
    return p;
}

namespace {
int rateIndex(const WifiFrameMeta& m) {
    if (m.ofdm) return m.rateKbps >= 12000 ? 4 : 3;
    if (m.rateKbps >= 2000) return m.shortPreamble ? 2 : 1;
    return 0;
}
} // namespace

struct EspnowReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false}, showBad{false};
    std::atomic<double> centerHz{0};
    double reportSec = 0.25;

    WifiReceiver wifi;
    double rate = 0, offset = 0, center = -1;
    EspnowPlan plan;
    int64_t nIn = 0;
    double nextReport = 0, lastGood = -100;
    bool slowNoted = false;
    EspnowTelemetry tel, pub;
    std::unordered_map<std::string, size_t> idx;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    void applyPlan() {
        center = centerHz.load();
        plan = espnowPlan(center);
        wifi.setCenterHz(center > 0 ? center : 0);
        wifi.setSignalOffset(offset + plan.offsetHz);
        tel.centerMhz = center > 0 ? center / 1e6 : 0;
        tel.channel = plan.channel; tel.offsetHz = plan.offsetHz;
    }

    // The device table and the message log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu)
    bool resClear = false, resLoad = false;
    EspnowTelemetry loaded;
    double loadedAt = 0;
    static void moveLists(EspnowTelemetry& to, EspnowTelemetry& from) {
        to.devices = std::move(from.devices); to.log = std::move(from.log);
        from.devices.clear(); from.log.clear();
    }
    static void shiftTimes(EspnowTelemetry& t, double dt) {
        for (auto& d : t.devices) { d.firstSec += dt; d.lastSec += dt; d.winStart += dt; }
        for (auto& l : t.log) l.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { EspnowTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void reindex() {
        idx.clear();
        for (size_t i = 0; i < tel.devices.size(); i++) idx[tel.devices[i].mac] = i;
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        wifi.configure(rate);
        wifi.reset();
        applyPlan();
        nIn = 0; nextReport = reportSec * rate; lastGood = -100;
        shiftTimes(tel, -tel.timeSec);
        tel.inputRate = rate; tel.timeSec = 0;
        if (rate > 0 && rate < espnowTuning().minSampleRate - 1 && !slowNoted) {
            slowNoted = true;
            DIAG(161612, "The sample rate of %.3f Msps is below the 20 Msps that ESP-NOW needs, so no frame is decoded.", rate / 1e6);
        }
    }
    void clearAll() {
        const uint64_t seq = tel.seq;
        const EspnowTelemetry keep = tel;
        tel = EspnowTelemetry(); tel.seq = seq; tel.inputRate = rate;
        tel.centerMhz = keep.centerMhz; tel.channel = keep.channel; tel.offsetHz = keep.offsetHz;
        idx.clear();
        wifi.clearNetworks();
    }

    EspnowDevice& device(const uint8_t mac[6]) {
        const std::string key = wifi::macText(mac);
        auto it = idx.find(key);
        if (it != idx.end()) return tel.devices[it->second];
        if (tel.devices.size() >= kEspnowDevMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.devices.size(); i++) if (tel.devices[i].lastSec < tel.devices[old].lastSec) old = i;
            tel.devices.erase(tel.devices.begin() + (ptrdiff_t)old);
            reindex();
        }
        EspnowDevice d;
        d.mac = key;
        d.espressif = espnow::espressifOui(mac);
        const char* v = wifi::vendorOf(mac);
        d.vendor = d.espressif ? "Espressif" : v ? v : "";
        d.firstSec = sigSec(); d.winStart = sigSec();
        d.firstWall = (int64_t)time(nullptr);
        tel.devices.push_back(std::move(d));
        idx[key] = tel.devices.size() - 1;
        if (log && tel.devices.size() <= 30) log("ESP-NOW: new device " + key);
        return tel.devices.back();
    }

    std::vector<std::vector<uint8_t>> keys() {
        std::vector<std::vector<uint8_t>> out;
        voicekeys::CallKeyInfo ki; ki.protocol = voicekeys::Protocol::EspNow;
        for (const auto& k : voicekeys::KeyStore::instance().candidates(ki)) if (k.key.size() == 16) out.push_back(k.key);
        return out;
    }

    void onFrame(const uint8_t* p, size_t n, const WifiFrameMeta& m) {
        espnow::Frame f;
        if (!m.fcsOk) {   // counted by the demodulator; listed only when asked for, never a device
            if (!showBad.load() || !espnow::parse(p, n, f) || (!f.espnow && !f.isProtected)) return;
            addLog(f, m);
            return;
        }
        tel.wifiFrames++;
        if (n < 24 || (p[0] >> 2 & 3) != 0 || ((p[0] >> 4) != 13 && (p[0] >> 4) != 14)) return;
        const bool prot = (p[1] & 0x40) != 0;
        if (!espnow::parse(p, n, f, prot ? keys() : std::vector<std::vector<uint8_t>>{})) return;
        tel.actionFrames++;
        // an encrypted action frame is taken for ESP-NOW when it comes from an Espressif address or has the broadcast address as address 3 (an
        // access point's protected action frames carry its BSSID there)
        const bool a3b = std::all_of(f.a3, f.a3 + 6, [](uint8_t v) { return v == 0xFF; });
        const bool encOnly = f.isProtected && !f.decrypted;
        if (!f.espnow && !(encOnly && (a3b || espnow::espressifOui(f.sa)))) return;
        tel.espnowFrames++; tel.blocksOk++;
        if (f.isProtected) tel.encryptedFrames++;
        if (f.decrypted) tel.decryptedFrames++;
        if (f.broadcast) tel.broadcastFrames++;
        tel.perRate[rateIndex(m)]++;
        if (plan.channel >= 1 && plan.channel <= 14) tel.perChannel[plan.channel]++;
        tel.cfoHz = m.cfoHz; tel.snrDb = m.snrDb;
        lastGood = sigSec();
        const int64_t wall = (int64_t)time(nullptr);
        EspnowDevice& d = device(f.sa);
        d.packets++;
        if (encOnly) d.encrypted++;
        if (f.broadcast) d.broadcasts++;
        if (f.version >= 0) d.version = f.version;
        d.rssi = m.rssiDbfs;
        d.rssiHist.push_back(m.rssiDbfs);
        if (d.rssiHist.size() > kEspnowRssiHist) d.rssiHist.erase(d.rssiHist.begin());
        d.channel = plan.channel; d.rateKbps = m.rateKbps;
        d.lastTo = f.broadcast ? "broadcast" : wifi::macText(f.da);
        d.lastEncrypted = encOnly;
        if (!encOnly) d.lastPayload.assign(f.body.begin(), f.body.begin() + (ptrdiff_t)std::min(f.body.size(), kEspnowKeepBytes));
        d.lastSec = sigSec(); d.lastWall = wall;
        d.winCount++;
        addLog(f, m);
    }

    void addLog(const espnow::Frame& f, const WifiFrameMeta& m) {
        EspnowMsg l;
        l.wallTime = (int64_t)time(nullptr); l.sec = sigSec(); l.channel = plan.channel;
        l.rateKbps = m.rateKbps; l.shortPreamble = m.shortPreamble;
        l.from = wifi::macText(f.sa); l.to = f.broadcast ? "broadcast" : wifi::macText(f.da);
        l.seq = f.seq; l.version = f.version;
        l.encrypted = f.isProtected; l.decrypted = f.decrypted; l.fcsOk = m.fcsOk;
        l.rssi = m.rssiDbfs; l.snrDb = m.snrDb; l.cfoHz = m.cfoHz;
        if (!f.isProtected || f.decrypted) l.payload.assign(f.body.begin(), f.body.begin() + (ptrdiff_t)std::min(f.body.size(), kEspnowKeepBytes));
        l.info = espnow::summary(f);
        if (!m.fcsOk) l.info = "FCS bad: " + l.info;
        tel.log.insert(tel.log.begin(), std::move(l));
        if (tel.log.size() > kEspnowLogMax) tel.log.pop_back();
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        WifiTelemetry wt;
        if (wifi.telemetry(wt, 0)) {
            tel.levelDb = wt.levelDb;
            tel.blocksBad = wt.blocksBad + wt.ofdmBadFcs;
        }
        for (auto& d : tel.devices) {   // packets per second over a window of 1 - 10 s; 0 once not heard for 10 s
            const double span = tel.timeSec - d.winStart;
            if (span >= 1) d.rate = (float)(d.winCount / span);
            if (span >= 10) { d.winStart = tel.timeSec; d.winCount = 0; }
            if (tel.timeSec - d.lastSec > 10) d.rate = 0;
        }
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        EspnowTelemetry out = tel;
        std::stable_sort(out.devices.begin(), out.devices.end(), [](const EspnowDevice& x, const EspnowDevice& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

EspnowReceiver::EspnowReceiver() : p_(std::make_unique<Impl>()) {
    Impl* m = p_.get();
    m->wifi.setOfdmQpsk(true);
    m->wifi.setReportInterval(0.05);
    m->wifi.setFrameMetaCallback([m](const uint8_t* f, size_t n, const WifiFrameMeta& meta) { m->onFrame(f, n, meta); });
}
EspnowReceiver::~EspnowReceiver() = default;

void EspnowReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void EspnowReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void EspnowReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool EspnowReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= espnowTuning().minSampleRate - 1; }
void EspnowReceiver::reset() { p_->resetReq = true; }
void EspnowReceiver::setShowBadFcs(bool on) { p_->showBad = on; }
void EspnowReceiver::setReportInterval(double sec) { std::lock_guard<std::mutex> lk(p_->mu); p_->reportSec = std::max(0.005, sec); p_->resetReq = true; }
void EspnowReceiver::clearDevices() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        EspnowTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}
void EspnowReceiver::clearResults() { clearDevices(); }

void EspnowReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.centerHz.load() != m.center) m.applyPlan();
    if (m.rate <= 0 || n == 0) return;
    size_t i = 0;
    while (i < n) {   // in pieces cut at the reports, so they keep their pace
        const size_t left = (size_t)std::max<int64_t>(1, (int64_t)std::ceil(m.nextReport) - m.nIn);
        const size_t k = std::min(n - i, left);
        m.wifi.feed(x + i, k);
        m.nIn += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += m.reportSec * m.rate; m.report(); }
    }
}

std::string EspnowReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.devices, p_->pub.log);
}
bool EspnowReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    EspnowTelemetry l;
    if (!results::unpack(blob, t0, l.devices, l.log)) {
        DIAG_DEBUG(284039, "The saved ESP-NOW results of %zu bytes are not in this build's format, so they are not loaded.", blob.size());
        return false;
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    EspnowTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool EspnowReceiver::telemetry(EspnowTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void EspnowReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning espnowTuning() {
    ModeTuning t;
    t.stdMode = 900; t.id = "espnow"; t.name = "ESP-NOW";
    t.minMhz = 2400; t.maxMhz = 2500; t.defMhz = 2412;   // channel 1: where an ESP32 not joined to an access point sends by default
    t.sampleRate = 20000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 20000000;              // the Wi-Fi demodulator works at 20 Msps
    t.tuneOffsetHz = 0;                      // the channel fills the band
    return t;
}

} // namespace dect2
