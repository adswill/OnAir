// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/hfbeacon/hfbeacon_rx.h"
#include "dect2/hfbeacon/hfbeacon_dsp.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include "../audio/ham_listen.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {

struct HfbReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> cb;
    double rate = 0;
    std::atomic<double> centerReq{0}, originReq{0}, ppmReq{0}, clockReq{0};
    std::atomic<bool> clockValidReq{false}, resetReq{false}, obsHave{false};
    std::atomic<double> obsLat{0}, obsLon{0};
    std::atomic<int> selReq{-1};
    hfb::Analyzer an;
    bool started = false;
    uint64_t inSamples = 0, nextPub = 0, nextSnap = 0;
    bool listReq = false, clearReq = false, loadReq = false;
    std::vector<hfb::Beacon> listIn;
    std::string loadBlob, snap;
    HfbTelemetry pub;
    std::vector<bool> loggedHeard;
    // Listen: a channel of the wide capture to the sound card, only while switched on
    marine::Front front;
    hfdig::UsbAudio usb;
    hfdig::Listen listen;
    std::atomic<double> listenHz{0};
    std::vector<cf32> base;
    std::vector<float> aud;
    double frontRate = 0, frontOff = 1e18;

    void publish() {
        HfbTelemetry t;
        an.fill(t);
        if (cb) {
            if (loggedHeard.size() != t.beacons.size()) loggedHeard.assign(t.beacons.size(), false);
            for (size_t i = 0; i < t.beacons.size(); i++) {
                const bool h = t.beacons[i].heard && t.beacons[i].conf == 2;
                if (h && !loggedHeard[i]) {
                    char b[200];
                    const auto& bc = an.beacons()[i];
                    snprintf(b, sizeof b, "HF beacons: %s %.1f kHz heard, %.1f dB in 100 Hz", bc.name.c_str(), bc.khz, (double)t.beacons[i].snr);
                    cb(b);
                }
                loggedHeard[i] = h;
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        t.seq = pub.seq + 1;
        pub = std::move(t);
    }
};

HfbReceiver::HfbReceiver() : p_(new Impl) { p_->an.setBeacons(hfb::defaultBeacons()); }
HfbReceiver::~HfbReceiver() = default;

void HfbReceiver::configure(double r) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->rate = r;
    }
    p_->resetReq = true;
    if (p_->cb && r > 0) {
        char b[128];
        snprintf(b, sizeof b, "HF beacons: %.3f Msps, watching the beacon list within 80 %% of the span", r / 1e6);
        p_->cb(b);
    }
}
void HfbReceiver::setCenterHz(double hz) { if (hz != p_->centerReq.load()) { p_->centerReq = hz; p_->resetReq = true; } }
void HfbReceiver::setTimeOrigin(double sec, bool isClock) { p_->originReq = sec; p_->clockValidReq = isClock; p_->resetReq = true; }
bool HfbReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0; }
void HfbReceiver::reset() { p_->resetReq = true; }
void HfbReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->cb = std::move(cb); }
void HfbReceiver::setSelected(int i) { p_->selReq = i; }
void HfbReceiver::setObserver(bool have, double lat, double lon) { p_->obsLat = lat; p_->obsLon = lon; p_->obsHave = have; }
void HfbReceiver::setPpm(double ppm) { p_->ppmReq = ppm; }
void HfbReceiver::setClockOffset(double s) { p_->clockReq = s; }
void HfbReceiver::setListen(bool on) { p_->listen.on = on; }
void HfbReceiver::setListenFreqHz(double hz) { p_->listenHz = hz; }
bool HfbReceiver::listening() const { return p_->listen.on.load(); }
void HfbReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void HfbReceiver::setMuted(bool m) { p_->listen.muted = m; }
void HfbReceiver::setSilent(bool s) { p_->listen.silent = s; }
void HfbReceiver::setBeacons(const std::vector<hfb::Beacon>& b) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->listIn = b; p_->listReq = true;
}

void HfbReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.listReq) { m.an.setBeacons(std::move(m.listIn)); m.listReq = false; m.loggedHeard.clear(); }
        if (m.clearReq) { m.an.clear(); m.clearReq = false; m.snap = m.an.saveResults(); }
        if (m.loadReq) { m.an.loadResults(m.loadBlob); m.loadBlob.clear(); m.loadReq = false; }
    }
    m.an.setPpm(m.ppmReq.load());
    m.an.setClockOffset(m.clockReq.load());
    m.an.setObserver(m.obsHave.load(), m.obsLat.load(), m.obsLon.load());
    m.an.setSelected(m.selReq.load());
    if (m.resetReq.exchange(false) || !m.started) {
        // with a radio the time origin is the computer clock now; a file or the test signal start from the given origin
        double origin = m.originReq.load();
        const bool clk = m.clockValidReq.load();
        if (clk) origin = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        m.an.configure(m.rate, m.centerReq.load(), origin, clk);
        m.started = true; m.inSamples = 0; m.nextPub = 0;
    }
    m.an.feed(x, n);
    if (m.listen.on.load() && m.listenHz.load() > 0 && m.rate >= 250e3) {
        const double off = m.listenHz.load() - 700.0 - m.centerReq.load();   // the dial 700 Hz below the carrier: USB puts the carrier at 700 Hz
        if (std::fabs(off) < 0.45 * m.rate) {
            if (m.frontRate != m.rate) { m.front.configure(m.rate); m.frontRate = m.rate; m.frontOff = 1e18; m.usb.reset(); }
            if (off != m.frontOff) { m.front.setOffsetHz(off); m.frontOff = off; }
            m.base.clear(); m.aud.clear();
            m.front.process(x, n, m.base);
            for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            double pw = 0;
            m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
            m.listen.play(m.aud.data(), m.aud.size());
        }
    } else if (m.frontRate != 0) { m.frontRate = 0; m.listen.reset(); }
    m.inSamples += n;
    if (m.inSamples >= m.nextSnap) {
        m.nextSnap = m.inSamples + (uint64_t)(m.rate * 30);
        std::string b = m.an.saveResults();
        std::lock_guard<std::mutex> lk(m.mu);
        m.snap = std::move(b);
    }
    if (m.inSamples >= m.nextPub) { m.nextPub = m.inSamples + (uint64_t)(m.rate / 4); m.publish(); }
}

void HfbReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearReq = true; p_->loadReq = false; p_->loadBlob.clear();
    p_->snap.clear();
    for (auto& b : p_->pub.beacons) { b.heard = false; b.conf = 0; b.lastHeard = 0; b.snr = NAN; b.best1h = b.best24h = NAN; b.spark.clear(); }
    p_->pub.history.clear();
    p_->pub.seq++;
}
std::string HfbReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->snap.empty()) { hfb::Analyzer empty; return empty.saveResults(); }
    return p_->snap;
}
bool HfbReceiver::loadResults(const std::string& blob) {
    hfb::Analyzer probe;
    if (!probe.loadResults(blob)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->loadBlob = blob; p_->loadReq = true; p_->clearReq = false;
    p_->snap = blob;
    p_->pub.seq++;
    return true;
}

bool HfbReceiver::telemetry(HfbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning hfbeaconTuning() {
    ModeTuning t;
    t.stdMode = 69; t.id = "hfbeacon"; t.name = "HF beacons";
    t.minMhz = 0.5; t.maxMhz = 30; t.defMhz = 14.5;
    t.sampleRate = 10e6;          // a HackRF at 10 Msps sees 8 MHz of HF; 20 Msps sees 16 MHz
    t.basebandHz = 0;
    t.bandwidthMhz = 8;
    t.minSampleRate = 250e3;
    t.carrierAtCentre = true;     // the detector skips the bins at the centre itself
    return t;
}

std::string hfbeaconSummary(const HfbTelemetry& t) {
    if (t.state == 0) return "HF beacons: waiting for samples";
    int inSpan = 0;
    for (const auto& b : t.beacons) if (b.inSpan) inSpan++;
    char b[200];
    snprintf(b, sizeof b, "%.2f - %.2f MHz, %d beacon%s in the span, %d heard now, %.1f dBFS", t.spanLoHz / 1e6, t.spanHiHz / 1e6, inSpan, inSpan == 1 ? "" : "s", t.heardNow, t.powerDb);
    return b;
}

} // namespace dect2
