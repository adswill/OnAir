// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector for the engine (see bletrk_live.h).
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/automation/engine.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/bletrk/bletrk_book.h"
#include "dect2/bletrk/bletrk_proto.h"
#include "dect2/bletrk/bletrk_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <mutex>

namespace dect2 {

struct BletrkLiveReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};

    BleReceiver ble;
    BletrkBook book;
    double rate = 0, base = 0;           // base: signal time of the earlier runs (the book's clock never restarts)
    int64_t nIn = 0;
    double nextReport = 0, lastGood = -100;
    BletrkTelemetry tel, pub;

    double sigSec() const { return base + (rate > 0 ? nIn / rate : 0); }

    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); if (rate > 0) base += nIn / rate; rate = rateSet; }
        nIn = 0; nextReport = 0.25 * rate; lastGood = -100 + 0;
        ble.configure(rate);
        ble.reset();
        tel.inputRate = rate;
    }

    void onBle(const ble::Adv& a, int ch, float rssi) {
        (void)ch;
        if (!a.hasAdv || a.data.empty()) return;
        if (a.type != ble::AdvInd && a.type != ble::AdvNonconnInd && a.type != ble::AdvScanInd && a.type != ble::ScanRsp) return;
        bletrk::Detection d;
        if (!bletrk::detect(a.data.data(), a.data.size(), d)) return;
        tel.trackerPackets++;
        book.hear(ble::addrText(a.adv), d, rssi, sigSec(), (int64_t)time(nullptr));
    }

    void report() {
        const double t = sigSec();
        const std::vector<BletrkAlert> fresh = book.tick(t, (int64_t)time(nullptr));
        for (const BletrkAlert& a : fresh) {
            if (log) log(a.text);
            if (automation::wants(automation::FBle)) {   // the automations and the watchlists (a Words list with "tracker" or "unwanted tracking")
                automation::Event ev("ble.tracker");
                ev.at(automation::wallNow(), "BLE tracker", 0);
                ev.str("kind", bletrk::kindName(a.kind)).str("summary", a.text).str("name", "Unwanted tracking alert").num("minutes", a.minutes).num("rssi_db", a.rssiAvg);
                automation::emit(std::move(ev));
            }
        }
        BleTelemetry bt;
        const bool hb = ble.telemetry(bt, 0);
        if (hb) {
            tel.cfoHz = bt.cfoHz; tel.snrDb = bt.snrDb; tel.levelDb = bt.levelDb; tel.blocksOk = bt.blocksOk; tel.blocksBad = bt.blocksBad; tel.channel = bt.channel;
            if (bt.blocksOk) lastGood = t;
        }
        tel.seq++;
        tel.timeSec = t;
        tel.state = hb && bt.state >= 1 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        const BletrkConfig c = book.config();
        tel.alertSec = c.alertSec; tel.nearDbfs = c.nearDbfs;
        book.fill(tel, t);
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

BletrkLiveReceiver::BletrkLiveReceiver() : p_(std::make_unique<Impl>()) {
    Impl* m = p_.get();
    m->ble.setPacketCallback([m](const ble::Adv& a, int ch, float rssi) { m->onBle(a, ch, rssi); });
}
BletrkLiveReceiver::~BletrkLiveReceiver() = default;

void BletrkLiveReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void BletrkLiveReceiver::setSignalOffset(double hz) { p_->ble.setSignalOffset(hz); }
void BletrkLiveReceiver::setCenterHz(double hz) { p_->ble.setCenterHz(hz); }
bool BletrkLiveReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= bletrkTuning().minSampleRate - 1; }
void BletrkLiveReceiver::reset() { p_->resetReq = true; }
void BletrkLiveReceiver::clearResults() {
    p_->book.clear();
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.trackers.clear(); p_->pub.alerts.clear(); p_->pub.followingNow = 0; p_->pub.seq++; }
    p_->clearReq = true;
}
void BletrkLiveReceiver::setAlertSeconds(double sec) { BletrkConfig c = p_->book.config(); c.alertSec = std::max(1.0, sec); p_->book.setConfig(c); }
void BletrkLiveReceiver::setNearDbfs(float dbfs) { BletrkConfig c = p_->book.config(); c.nearDbfs = dbfs; p_->book.setConfig(c); }
void BletrkLiveReceiver::ignoreTracker(uint32_t id, bool on) { p_->book.ignore(id, on); }

void BletrkLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) { m.tel.trackerPackets = 0; m.tel.alerts.clear(); }
    if (m.rate <= 0 || n == 0) return;
    // in pieces of at most a quarter second, so the reports keep their pace
    size_t i = 0;
    while (i < n) {
        const size_t left = (size_t)std::max<int64_t>(1, (int64_t)m.nextReport - m.nIn);
        const size_t k = std::min(n - i, left);
        if (m.rate >= bletrkTuning().minSampleRate - 1) m.ble.feed(x + i, k);
        m.nIn += (int64_t)k;
        i += k;
        if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); }
    }
}

bool BletrkLiveReceiver::telemetry(BletrkTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void BletrkLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning bletrkTuning() {
    ModeTuning t = bleTuning();
    t.stdMode = 145; t.id = "bletracker"; t.name = "BLE tracker detector";
    return t;
}

} // namespace dect2
