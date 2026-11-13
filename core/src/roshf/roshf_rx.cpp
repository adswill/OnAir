// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) detector receiver, see roshf_rx.h
#include "dect2/roshf/roshf_rx.h"
#include "dect2/roshf/roshf_tel.h"
#include "dect2/ros/ros_detect.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

constexpr double kAudioRate = 8000.0;
constexpr size_t kRing = 8000 * 24;           // 24 s of audio
constexpr size_t kMeasureSec = 16;            // the stretch measured
constexpr double kEverySec = 2.0;
constexpr size_t kFftLen = 640;               // 80 ms: the waterfall rows
constexpr double kHoldSec = 12.0;

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

struct RoshfReceiver::Impl {
    std::mutex mu, telMu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::atomic<int> demodSet{HfDemodUsb};
    std::vector<cf32> base;
    std::vector<float> aud, ring;
    uint64_t na = 0;                 // audio samples seen
    uint64_t nextWater = 4000, nextMeasure = 0;
    double powAcc = 0; uint64_t powN = 0;
    std::vector<float> cosT, sinT, win;
    // results
    std::deque<std::vector<uint8_t>> water;
    std::deque<RoshfEvent> events;
    uint64_t ok = 0, bad = 0;
    ros::Detection cur;
    double curAt = -1e9;             // audio time of the last positive detection
    double seenAt = -1e9;            // audio time of the last measurement that found a signal at all
    bool curValid = false;
    int state = 0;
    double bandDb = 0, floorDb = 0;
    float levelDb = -200;
    uint64_t seq = 0;
    uint64_t nextReport = 0;
    RoshfTelemetry pub;
    double inRate = 0;
    uint64_t nIn = 0;

    void say(const std::string& s) { if (log) log(s); }
    double audioNow() const { return (double)na / kAudioRate; }

    void tables() {
        cosT.assign(kRoshfWaterBins * kFftLen, 0.f); sinT = cosT;
        win.assign(kFftLen, 0.f);
        for (size_t n = 0; n < kFftLen; n++) win[n] = (float)(0.5 - 0.5 * std::cos(2 * math::kPi * (n + 0.5) / kFftLen));
        for (size_t k = 0; k < kRoshfWaterBins; k++) {
            const double f = (k + 0.5) * kRoshfBinHz;
            for (size_t n = 0; n < kFftLen; n++) {
                cosT[k * kFftLen + n] = (float)(win[n] * std::cos(2 * math::kPi * f * n / kAudioRate));
                sinT[k * kFftLen + n] = (float)(win[n] * std::sin(2 * math::kPi * f * n / kAudioRate));
            }
        }
    }

    void resetState() {
        running = rate >= roshfTuning().minSampleRate - 1;
        if (running) { front.configure(rate); front.setOffsetHz(offsetHz); }
        usb.reset();
        ring.assign(kRing, 0.f);
        na = 0; nextWater = 4000; nextMeasure = (uint64_t)(10 * kAudioRate);
        powAcc = 0; powN = 0; curValid = false; curAt = -1e9; state = 0;
        nIn = 0; nextReport = 0;
        if (cosT.empty()) tables();
        if (rate > 0) say(running ? "ROS: looking for the tone grid of ROS HF (4 / 8 / 16 baud) on the sideband; text is not decoded (no public spec)"
                                  : "ROS: the sample rate is too low (250 kHz at least)");
    }

    void waterRow() {
        if (na < kFftLen) return;
        std::vector<float> x(kFftLen);
        for (size_t i = 0; i < kFftLen; i++) x[i] = ring[(na - kFftLen + i) % kRing];
        std::vector<double> db(kRoshfWaterBins);
        for (size_t k = 0; k < kRoshfWaterBins; k++) {
            double re = 0, im = 0;
            const float* c = &cosT[k * kFftLen]; const float* s = &sinT[k * kFftLen];
            for (size_t n = 0; n < kFftLen; n++) { re += x[n] * c[n]; im += x[n] * s[n]; }
            db[k] = 10 * std::log10(re * re + im * im + 1e-12);
        }
        std::vector<double> srt = db;
        std::nth_element(srt.begin(), srt.begin() + (long)srt.size() / 2, srt.end());
        floorDb = srt[srt.size() / 2];
        std::vector<uint8_t> row(kRoshfWaterBins);
        for (size_t k = 0; k < kRoshfWaterBins; k++) row[k] = (uint8_t)std::max(0.0, std::min(255.0, (db[k] - floorDb) / 30.0 * 255.0));
        water.push_front(std::move(row));
        if (water.size() > kRoshfWaterRows) water.pop_back();
        // power in the detected band relative to the floor (a snapshot, for the SNR figure)
        if (state == 2 && cur.hiHz > cur.loHz) {
            double s = 0; int c = 0;
            for (size_t k = 0; k < kRoshfWaterBins; k++) { const double f = (k + 0.5) * kRoshfBinHz; if (f >= cur.loHz && f <= cur.hiHz) { s += db[k]; c++; } }
            bandDb = c ? s / c - floorDb : 0;
        } else {
            bandDb = *std::max_element(db.begin(), db.end()) - floorDb;
        }
    }

    void measure() {
        const size_t want = (size_t)(kMeasureSec * kAudioRate);
        const size_t have = (size_t)std::min<uint64_t>(na, kRing);
        const size_t n = std::min(want, have);
        if (n < (size_t)(8 * kAudioRate)) return;
        std::vector<float> x(n);
        for (size_t i = 0; i < n; i++) x[i] = ring[(na - n + i) % kRing];
        ros::Detection d = ros::detectAudio(x.data(), n, kAudioRate);
        if (!(d.valid && d.ros) && n > (size_t)(10 * kAudioRate)) {
            // a drifting radio smears the tone grid over a long stretch: the last 8 s alone often still fit it
            const size_t m = (size_t)(8 * kAudioRate);
            const ros::Detection d2 = ros::detectAudio(x.data() + (n - m), m, kAudioRate);
            if (d2.valid && d2.ros) d = d2;
        }
        const double t = audioNow();
        if (d.valid && d.ros) {
            ok++;
            cur = d; curAt = t; curValid = true;
            const int64_t utc = wallNow();
            if (!events.empty() && events.front().subId == d.subId && t - events.front().timeSec < 30.0) {
                RoshfEvent& e = events.front();
                e.utc = utc; e.timeSec = t; e.score = d.score; e.weak = d.weak; e.loHz = d.loHz; e.hiHz = d.hiHz; e.spacingHz = d.spacingHz; e.baud = d.baud; e.why = d.why;
            } else {
                RoshfEvent e;
                e.utc = utc; e.timeSec = t; e.subId = d.subId; e.name = d.name; e.why = d.why; e.baud = d.baud; e.spacingHz = d.spacingHz;
                e.loHz = d.loHz; e.hiHz = d.hiHz; e.score = d.score; e.weak = d.weak;
                events.push_front(e);
                if (events.size() > kRoshfEventsMax) events.pop_back();
                say("ROS: " + d.name + (d.weak ? " (perhaps)" : "") + " detected");
            }
        } else if (d.valid) {
            bad++;
            seenAt = t;
            if (!curValid) cur = d;
        }
        // 1: something of about the right width but not identified
        if (!(curValid && t - curAt <= kHoldSec)) { curValid = false; }
    }

    void process(const float* a, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring[na % kRing] = a[i];
            na++;
            powAcc += (double)a[i] * a[i]; powN++;
            if (na >= nextWater) { nextWater += 4000; waterRow(); }
            if (na >= nextMeasure) { nextMeasure += (uint64_t)(kEverySec * kAudioRate); measure(); }
        }
    }

    void publish() {
        const double t = audioNow();
        const bool det = curValid && t - curAt <= kHoldSec;
        state = det ? 2 : (cur.valid && cur.spanHz > 600 && cur.cleanShare > 0.5 && t - seenAt < 30 ? 1 : 0);
        RoshfTelemetry o;
        o.timeSec = t; o.inputRate = rate;
        o.state = state; o.dataValid = det;
        o.levelDb = powN ? (float)(10 * std::log10(powAcc / powN + 1e-12)) : -200.f;
        powAcc = 0; powN = 0;
        o.snrDb = (float)bandDb;
        o.blocksOk = ok; o.blocksBad = bad;
        if (det) {
            o.subId = cur.subId; o.name = cur.name; o.why = cur.why; o.baud = cur.baud; o.spacingHz = cur.spacingHz; o.loHz = cur.loHz; o.hiHz = cur.hiHz;
            o.spanHz = cur.spanHz; o.score = cur.score; o.weak = cur.weak; o.distinctTones = cur.distinctTones;
            o.markLoHz = cur.loHz; o.markHiHz = cur.hiHz;
            o.cfoHz = 0.5 * (cur.loHz + cur.hiHz) - 1550.0;
        }
        o.textStatus = "partial, needs a public spec: ROS is detected and its sub-mode named; its text is not decoded (framing, coding and FEC are not published)";
        o.water.assign(water.begin(), water.end());
        o.events.assign(events.begin(), events.end());
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }
};

RoshfReceiver::RoshfReceiver() : p_(std::make_unique<Impl>()) {}
RoshfReceiver::~RoshfReceiver() = default;

void RoshfReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetState(); }
void RoshfReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; if (p_->running) p_->front.setOffsetHz(hz); }
bool RoshfReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->running; }
void RoshfReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->resetState(); }
void RoshfReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void RoshfReceiver::setDemod(int d) { p_->demodSet = d; }
int RoshfReceiver::demod() const { return p_->demodSet.load(); }

void RoshfReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.events.clear(); m.ok = m.bad = 0; m.curValid = false; m.curAt = m.seenAt = -1e9; m.cur = ros::Detection();
    m.publish();
}

void RoshfReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.running || n == 0) return;
    m.nIn += n;
    m.base.clear();
    m.front.process(x, n, m.base);
    for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
    int d = hfDemodResolve(m.demodSet.load(), 0);
    if (d != HfDemodLsb) d = HfDemodUsb;
    if (d != m.usb.demod()) m.usb.setDemod(d);
    m.aud.clear();
    double pw = 0;
    m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
    m.process(m.aud.data(), m.aud.size());
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.rate;
        m.publish();
    }
}

void RoshfReceiver::flush() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->running) return;
    p_->measure();
    p_->publish();
}

bool RoshfReceiver::telemetry(RoshfTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

const RoshfPreset* roshfPresets(int& count) {
    static const RoshfPreset kP[] = {{"80 m", 3.590}, {"40 m", 7.045}, {"30 m", 10.140}, {"20 m", 14.080}, {"17 m", 18.100}, {"15 m", 21.090}, {"10 m", 28.120}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

ModeTuning roshfTuning() {
    ModeTuning t;
    t.stdMode = 207; t.id = "roshf"; t.name = "ROS (HF)";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.080;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

} // namespace dect2
