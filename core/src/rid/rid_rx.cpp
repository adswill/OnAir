// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID receiver (rid_rx.h): the BLE and Wi-Fi demodulators side by side, their good packets decoded as Open Drone ID.
#include "dect2/rid/rid_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/results/results_io.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/rid/rid_proto.h"
#include "dect2/rid/rid_tel.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dect2 {

RidPlan ridPlan(double centerHz) {
    RidPlan p;
    if (centerHz <= 0) return p;
    const double c = centerHz / 1e6;
    // Wi-Fi: 1, 6 or 11 (where nearly every network and drone sits) when one is within 6 MHz, else the nearest channel
    int best = 0; double bd = 1e9;
    for (int ch : {1, 6, 11}) { const double d = std::fabs(wifi::channelMhz(ch) - c); if (d <= 6 && d < bd) { bd = d; best = ch; } }
    if (!best) for (int ch = 1; ch <= 13; ch++) { const double d = std::fabs(wifi::channelMhz(ch) - c); if (d < bd) { bd = d; best = ch; } }
    p.wifiChannel = best;
    p.wifiOffsetHz = (wifi::channelMhz(best) - c) * 1e6;
    // BLE: the advertising channel within 8 MHz
    bd = 1e9;
    for (int ch = 37; ch <= 39; ch++) {
        const double d = ble::channelMhz(ch) - c;
        if (std::fabs(d) <= 8 && std::fabs(d) < bd) { bd = std::fabs(d); p.bleChannel = ch; p.bleOffsetHz = d * 1e6; }
    }
    return p;
}

namespace {
// The burst gate (rid_rx.h): which windows of the input the Wi-Fi demodulator is handed
class BurstGate {
public:
    static constexpr int kF = 128;          // FFT size
    static constexpr int kPre = 4, kPost = 8;   // windows handed on before and after a window with energy
    static constexpr int kHist = 64;        // windows the noise floor is taken from (every 16th)
    using Out = std::function<void(const cf32*, size_t)>;

    bool on = true;

    // bins of each channel: offsets from the centre of the capture, Hz; bleOn false: BLE is not listened to
    void configure(double rate, bool bleOn, double bleOff, double wifiOff, bool live) {
        rate_ = rate;
        bpw_ = std::max(2, (int)std::lround(rate / 5e6));   // blocks of kF per window: 4 at 20 Msps (25.6 us)
        W_ = bpw_ * kF;
        ring_.assign((size_t)W_ * (kPre + 1), cf32(0, 0));
        buf_.assign(kF, cf32(0, 0));
        wifi_.bins.clear();
        bleOn_ = bleOn;
        for (int k = 0; k < kF; k++) {
            const double f = (k < kF / 2 ? k : k - kF) * rate / kF;
            if (live && std::fabs(f) < 0.25e6) continue;                 // the radio's DC spike
            if (bleOn && std::fabs(f - bleOff) <= 1.2e6) continue;       // a BLE packet should not open the Wi-Fi demodulator
            if (std::fabs(f - wifiOff) <= 9.0e6 && std::fabs(f) <= 0.47 * rate) wifi_.bins.push_back(k);
        }
        wifi_.thr = 1.2f;   // 0.8 dB in about 16 MHz: an 802.11b frame at -6 dB SNR in the channel still opens it
        reset();
    }
    void reset() {
        fill_ = 0; nWin_ = 0;
        Band& b = wifi_;
        b.hn = b.hp = 0; b.q = 0; b.act = 0; b.busy = b.seen = 0; b.bypass = true; b.upd = 0;
    }
    // x: the input; the demodulators get what the gate lets through, kPre windows later
    void push(const cf32* x, size_t n, const Out& toBle, const Out& toWifi) {
        while (n) {
            const size_t k = std::min(n, (size_t)(W_ - fill_));
            std::copy(x, x + k, ring_.begin() + (ptrdiff_t)((size_t)(nWin_ % (kPre + 1)) * (size_t)W_ + (size_t)fill_));   // straight into its slot
            for (size_t i = 0; i < k; i++) power_ += (double)std::norm(x[i]);
            nPower_ += k;
            fill_ += (int)k; x += k; n -= k;
            if (fill_ == W_) { window(toBle, toWifi); fill_ = 0; }
        }
    }
    double levelDb() { const double l = nPower_ ? 10 * std::log10(power_ / (double)nPower_ + 1e-20) : -200.0; power_ = 0; nPower_ = 0; return l; }
    float fedWifi() { const float r = allWifi_ ? (float)fedWifi_ / (float)allWifi_ : 1.f; fedWifi_ = allWifi_ = 0; return r; }

private:
    struct Band {
        std::vector<int> bins;
        float hist[kHist] = {};
        int hn = 0, hp = 0, upd = 0;
        float q = 0, thr = 1.5f;
        uint32_t act = 0;                  // energy in the last windows, the newest in bit 0
        int busy = 0, seen = 0;
        bool bypass = true;                // the first windows, or a busy channel: everything is handed on
    };
    void update(Band& b, float e) {
        const bool a = b.hn >= 16 && e > b.thr * b.q;
        b.act = b.act << 1 | (a ? 1u : 0u);
        b.busy += a ? 1 : 0;
        if (++b.seen == 256) { b.bypass = b.bins.empty() || b.busy > 154; b.busy = b.seen = 0; }   // busy more than 60 % of the time
        // the floor from every 16th window over the last 26 ms (all of the first 16): a packet (an 802.11b beacon lasts a few ms) cannot raise it
        if ((b.upd++ & 15) != 0 && b.hn >= 16) return;
        b.hist[b.hp] = e; b.hp = (b.hp + 1) % kHist;
        if (b.hn < kHist) b.hn++;
        if (b.hn < 16) return;
        float t[kHist];
        std::copy(b.hist, b.hist + b.hn, t);
        std::nth_element(t, t + b.hn / 4, t + b.hn);
        b.q = t[b.hn / 4];
    }
    void window(const Out& toBle, const Out& toWifi) {
        float ew = 0;
        const int R = kPre + 1;
        const cf32* cur = ring_.data() + (size_t)(nWin_ % R) * (size_t)W_;
        for (int blk = 0; blk < bpw_; blk += 2) {   // half of the window is enough to see a burst
            std::copy(cur + blk * kF, cur + (blk + 1) * kF, buf_.begin());
            fft_.forward(buf_.data());
            for (int k : wifi_.bins) ew += std::norm(buf_[(size_t)k]);
        }
        update(wifi_, ew);
        nWin_++;
        if (nWin_ <= kPre) return;
        const cf32* old = ring_.data() + (size_t)(nWin_ % R) * (size_t)W_;   // the window kPre before the newest
        const uint32_t mask = (1u << (kPre + kPost + 1)) - 1;
        if (bleOn_) toBle(old, (size_t)W_);   // BLE gets everything (gating it cost packets near its sensitivity: docs/ADSB-REMOTEID-3.9.md)
        allWifi_ += (uint64_t)W_;
        if (!on || wifi_.bypass || (wifi_.act & mask)) { toWifi(old, (size_t)W_); fedWifi_ += (uint64_t)W_; }
    }

    Band wifi_;
    Fft fft_{kF};
    std::vector<cf32> ring_, buf_;   // ring_: the last kPre + 1 windows
    double rate_ = 0;
    int bpw_ = 4, W_ = 512, fill_ = 0;
    int64_t nWin_ = 0;
    bool bleOn_ = true;
    double power_ = 0; uint64_t nPower_ = 0;
    uint64_t fedWifi_ = 0, allWifi_ = 0;
};
}

struct RidReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};

    BleReceiver ble;
    WifiReceiver wifi;
    BurstGate gate;
    std::atomic<bool> gateOn{true};
    double rate = 0, offset = 0, center = -1;
    RidPlan plan;
    int64_t nIn = 0;
    double nextReport = 0, lastGood = -100;
    RidTelemetry tel, pub;
    std::unordered_map<std::string, size_t> idx;
    std::vector<rid::Message> msgs;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    void applyPlan() {
        center = centerHz.load();
        plan = ridPlan(center);
        ble.setCenterHz(center > 0 ? center : 0);
        ble.setSignalOffset(offset + plan.bleOffsetHz);
        wifi.setCenterHz(center > 0 ? center : 0);
        wifi.setSignalOffset(offset + plan.wifiOffsetHz);
        tel.centerMhz = center > 0 ? center / 1e6 : 0;
        tel.bleChannel = plan.bleChannel; tel.wifiChannel = plan.wifiChannel;
        tel.bleOffsetHz = plan.bleOffsetHz; tel.wifiOffsetHz = plan.wifiOffsetHz;
        if (rate > 0) gate.configure(rate, plan.bleChannel || center <= 0, offset + plan.bleOffsetHz, offset + plan.wifiOffsetHz, center > 0);
    }

    // The drone table and the message log stay through resets: Clear and a loaded file reach the feed thread's copy through these requests (mu), and the
    // published report at once
    bool resClear = false, resLoad = false;
    RidTelemetry loaded;
    double loadedAt = 0;                   // the report's clock the loaded lists are on
    static void moveLists(RidTelemetry& to, RidTelemetry& from) {
        to.drones = std::move(from.drones); to.log = std::move(from.log);
        from.drones.clear(); from.log.clear();
    }
    static void shiftTimes(RidTelemetry& t, double dt) {
        for (auto& d : t.drones) { d.firstSec += dt; d.lastSec += dt; }
        for (auto& l : t.log) l.sec += dt;
    }
    void takeRequests() {   // feed thread
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { RidTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void reindex() {
        idx.clear();
        for (size_t i = 0; i < tel.drones.size(); i++) idx[tel.drones[i].mac] = i;
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        ble.configure(rate); wifi.configure(rate);
        ble.reset(); wifi.reset();
        applyPlan();
        nIn = 0; nextReport = 0.25 * rate; lastGood = -100;
        shiftTimes(tel, -tel.timeSec);    // the lists stay (clearResults): the clock starts again
        tel.inputRate = rate; tel.timeSec = 0;
    }

    void clearAll() {
        const uint64_t seq = tel.seq;
        const RidTelemetry keep = tel;
        tel = RidTelemetry(); tel.seq = seq; tel.inputRate = rate;
        tel.centerMhz = keep.centerMhz; tel.bleChannel = keep.bleChannel; tel.wifiChannel = keep.wifiChannel;
        tel.bleOffsetHz = keep.bleOffsetHz; tel.wifiOffsetHz = keep.wifiOffsetHz;
        idx.clear();
        ble.clearDevices(); wifi.clearNetworks();
    }

    RidDrone& drone(const std::string& mac) {
        auto it = idx.find(mac);
        if (it != idx.end()) return tel.drones[it->second];
        if (tel.drones.size() >= kRidDroneMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.drones.size(); i++) if (tel.drones[i].lastSec < tel.drones[old].lastSec) old = i;
            tel.drones.erase(tel.drones.begin() + (ptrdiff_t)old);
            idx.clear();
            for (size_t i = 0; i < tel.drones.size(); i++) idx[tel.drones[i].mac] = i;
        }
        RidDrone d;
        d.mac = mac;
        d.firstSec = sigSec();
        d.firstWall = (int64_t)time(nullptr);
        tel.drones.push_back(std::move(d));
        idx[mac] = tel.drones.size() - 1;
        if (log && tel.drones.size() <= 30) log("Remote ID: new drone " + mac);
        return tel.drones.back();
    }

    void apply(RidDrone& d, const rid::Message& m) {
        d.messages++;
        if (m.type >= 0 && m.type < 16) { d.typeMask |= 1u << m.type; tel.perType[m.type]++; }
        tel.blocksOk++;
        switch (m.type) {
        case rid::BasicId:
            d.uaType = m.uaType;
            if (m.idType == 1) d.serial = m.id;
            else if (m.idType == 2) d.caaId = m.id;
            else if (m.idType == 3) d.utmId = m.id;
            else if (m.idType == 4) d.sessionId = m.id;
            break;
        case rid::Location:
            d.status = m.status; d.heightType = m.heightType;
            d.altGeo = (float)m.altGeo; d.altBaro = (float)m.altBaro; d.height = (float)m.height;
            d.speed = (float)m.speed; d.direction = (float)m.direction;
            d.vspeed = (float)m.vspeed; d.vspeedValid = m.vspeedValid;
            d.horizAcc = m.horizAcc; d.vertAcc = m.vertAcc; d.speedAcc = m.speedAcc;
            d.timestamp = m.timestamp;
            if (m.posValid) {
                const bool moved = !d.hasPos || std::fabs(m.lat - d.lat) > 1e-6 || std::fabs(m.lon - d.lon) > 1e-6;
                d.hasPos = true; d.lat = m.lat; d.lon = m.lon;
                if (moved) {
                    d.track.push_back({m.lat, m.lon, (float)m.altGeo});
                    if (d.track.size() > kRidTrack) d.track.erase(d.track.begin());
                }
            }
            break;
        case rid::Auth:
            d.authType = m.authType;
            if (m.page == 0) d.authPages = m.lastPage + 1;
            break;
        case rid::SelfId: d.selfId = m.text; d.selfIdType = m.descType; break;
        case rid::System:
            if (m.opValid) { d.hasOperator = true; d.opLat = m.opLat; d.opLon = m.opLon; }
            d.opAlt = (float)m.opAlt; d.opLocType = m.opLocType; d.classType = m.classType;
            d.category = m.category; d.uaClass = m.uaClass; d.areaCount = m.areaCount; d.areaRadius = m.areaRadius; d.sysTime = m.sysTime;
            break;
        case rid::OperatorId: d.operatorId = m.text; break;
        default: break;
        }
    }

    void heard(const std::string& mac, int transport, int channel, int counter, float rssi) {
        const int64_t wall = (int64_t)time(nullptr);
        lastGood = sigSec();
        RidDrone& d = drone(mac);
        if (transport == 0) d.viaBle = true; else d.viaWifi = true;
        d.rssi = rssi;
        d.rssiHist.push_back(rssi);
        if (d.rssiHist.size() > kRidRssiHist) d.rssiHist.erase(d.rssiHist.begin());
        d.lastSec = sigSec(); d.lastWall = wall;
        d.counter = counter;
        for (const auto& m : msgs) {
            apply(d, m);
            RidMsgLog l;
            l.wallTime = wall; l.sec = sigSec(); l.transport = transport; l.channel = channel; l.mac = mac; l.type = m.type; l.rssi = rssi;
            l.info = rid::summary(m);
            tel.log.insert(tel.log.begin(), std::move(l));
        }
        if (tel.log.size() > kRidLogMax) tel.log.resize(kRidLogMax);
        if (d.hasPos && automation::wants(automation::FRid) && sigSec() - d.autoEmitSec >= 5.0) {   // the automations: at most every 5 s per drone
            d.autoEmitSec = sigSec();
            automation::Event ev("rid.drone");
            ev.at(automation::wallNow(), "Remote ID", 0);
            ev.str("id", !d.serial.empty() ? d.serial : !d.sessionId.empty() ? d.sessionId : !d.caaId.empty() ? d.caaId : d.mac).str("mac", d.mac).num("lat", d.lat).num("lon", d.lon);
            if (d.altGeo > -1000) ev.num("alt_m", d.altGeo);
            if (d.speed >= 0) ev.num("speed_ms", d.speed);
            if (d.hasOperator) ev.num("op_lat", d.opLat).num("op_lon", d.opLon);
            automation::emit(std::move(ev));
        }
    }

    void onBle(const ble::Adv& a, int ch, float rssi) {
        tel.blePackets++;
        if (!a.hasAdv || a.data.empty()) return;
        msgs.clear();
        int counter = -1;
        if (!rid::fromBleAdvData(a.data.data(), a.data.size(), counter, msgs)) return;
        tel.bleRid++;
        heard(ble::addrText(a.adv), 0, ch, counter, rssi);
    }

    void onWifi(const uint8_t* f, size_t n, float rssi) {
        tel.wifiFrames++;
        msgs.clear();
        int counter = -1;
        if (!rid::fromBeacon(f, n, counter, msgs)) return;
        tel.wifiRid++;
        heard(wifi::macText(f + 10), 1, plan.wifiChannel, counter, rssi);
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        BleTelemetry bt; WifiTelemetry wt;
        const bool hb = ble.telemetry(bt, 0), hw = wifi.telemetry(wt, 0);
        tel.levelDb = hw ? wt.levelDb : hb ? bt.levelDb : -200.f;
        {   // the demodulators only see the bursts: the level of the whole input comes from the gate
            const double l = gate.levelDb();
            if (gate.on && l > -199) tel.levelDb = (float)l;
            tel.wifiFed = gate.fedWifi();
        }
        tel.blocksBad = (hb ? bt.blocksBad : 0) + (hw ? wt.blocksBad : 0);
        // the carrier offset and SNR of whichever carrier was last heard with Remote ID
        if (!tel.log.empty()) {
            if (tel.log.front().transport == 0 && hb) { tel.cfoHz = bt.cfoHz; tel.snrDb = bt.snrDb; }
            else if (hw) { tel.cfoHz = wt.cfoHz; tel.snrDb = wt.snrDb; }
        }
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        RidTelemetry out = tel;
        std::stable_sort(out.drones.begin(), out.drones.end(), [](const RidDrone& x, const RidDrone& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

RidReceiver::RidReceiver() : p_(std::make_unique<Impl>()) {
    Impl* m = p_.get();
    m->ble.setPacketCallback([m](const ble::Adv& a, int ch, float rssi) { m->onBle(a, ch, rssi); });
    m->wifi.setFrameCallback([m](const uint8_t* f, size_t n, float rssi) { m->onWifi(f, n, rssi); });
}
RidReceiver::~RidReceiver() = default;

void RidReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void RidReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void RidReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool RidReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= ridTuning().minSampleRate - 1; }
void RidReceiver::reset() { p_->resetReq = true; }
void RidReceiver::clearDrones() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);   // the report at once; the feed thread's lists (and counters) on its next block
        RidTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}
void RidReceiver::clearResults() { clearDrones(); }

void RidReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.centerHz.load() != m.center) m.applyPlan();
    if (m.rate <= 0 || n == 0) return;
    // in pieces of at most a quarter second, so the reports keep their pace
    size_t i = 0;
    while (i < n) {
        const size_t left = (size_t)std::max<int64_t>(1, (int64_t)m.nextReport - m.nIn);
        const size_t k = std::min(n - i, left);
        if (m.rate >= ridTuning().minSampleRate - 1) {
            m.gate.on = m.gateOn.load();
            m.gate.push(x + i, k, [&m](const cf32* p, size_t c) { m.ble.feed(p, c); }, [&m](const cf32* p, size_t c) { m.wifi.feed(p, c); });
        }
        m.nIn += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

std::string RidReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.drones, p_->pub.log);
}
bool RidReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    RidTelemetry l;
    if (!results::unpack(blob, t0, l.drones, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    RidTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool RidReceiver::telemetry(RidTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void RidReceiver::setGate(bool on) { p_->gateOn = on; }
void RidReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning ridTuning() {
    ModeTuning t;
    t.stdMode = 58; t.id = "rid"; t.name = "Drone Remote ID";
    t.minMhz = 2400; t.maxMhz = 2500; t.defMhz = 2432;   // BLE channel 38 and Wi-Fi channel 6 in one 20 MHz band
    t.sampleRate = 20000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 20000000;              // the Wi-Fi demodulator works at 20 Msps
    t.tuneOffsetHz = 0;                      // neither carrier sits on the centre
    return t;
}

} // namespace dect2
