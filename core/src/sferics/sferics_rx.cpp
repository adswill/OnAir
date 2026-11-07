// Sferics receiver: smoothed power |x|^2 (25 us), a noise floor from the median of 1 ms block means over the last 4 s, a threshold over it, and a
// small state machine that turns each crossing into an impulse (start, peak, width). Pulses closer than the merge time are one strike. An impulse
// that repeats at a multiple of the 100 / 120 Hz mains half period is mains interference: the receiver waits 0.5 s before it counts an impulse so a
// regular train can be recognised and left out. Impulses longer than 5 ms are not lightning.
#include "dect2/sferics/sferics_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kSmoothUs = 25.0;
constexpr double kHoldUs = 100.0;
constexpr double kMaxWidthS = 0.005;
constexpr double kCommitDelayS = 0.5;
constexpr double kFlashGapS = 0.5;
constexpr size_t kFloorBlocks = 4000;      // 4 s of 1 ms blocks
constexpr size_t kFloorWarm = 500;         // 0.5 s before the first detection
constexpr size_t kMainsWindow = 32;
constexpr int kMainsOn = 24, kMainsOff = 12;
}

struct SfericsReceiver::Impl {
    mutable std::mutex mu;
    double rate = 0, centerHz = 0, offsetHz = 0;
    double thresholdDb = 10, mergeS = 0.0005;
    bool mainsFilter = true;
    int64_t startUtc = 0;
    std::function<void(const std::string&)> log;

    // front end
    size_t W = 1, B = 1, hold = 1;
    std::vector<float> win;
    size_t wpos = 0;
    double wsum = 0;
    double blockSum = 0;
    size_t blockN = 0;
    std::deque<float> blocks;
    size_t blocksSinceMedian = 0;
    double floorLin = 0;
    bool floorValid = false;
    uint64_t n = 0;               // samples seen

    // detector
    bool active = false;
    uint64_t startN = 0;
    double peak = 0;
    size_t below = 0;

    struct Ev { double t = 0, peakDb = 0, widthS = 0; bool hit = false; double endT = 0; };
    std::deque<Ev> pending;       // not yet counted (0.5 s)
    std::deque<double> lastTimes; // the start times of the last 4 impulses (counted or not)
    std::deque<bool> hits;
    bool mainsOn = false;
    double lastEndT = -1;
    double gridP = 0, gridT0 = 0;   // the mains grid: period and the last impulse on it

    // results
    uint64_t strikes = 0, flashes = 0, mains = 0, tooLong = 0;
    double lastStrikeT = -1;
    int flashNo = 0;
    std::map<int64_t, int> perMinuteMap;
    std::deque<double> strikeTimes;      // last 10 minutes
    std::deque<SfericsStrike> recent;    // newest at the back
    float lastPeakDb = 0;
    size_t sinceSnap = 0;
    uint64_t seq = 0;
    SfericsTelemetry snap;

    void clearAll() {
        win.assign(W, 0.f);
        wpos = 0; wsum = 0; blockSum = 0; blockN = 0;
        blocks.clear(); blocksSinceMedian = 0; floorLin = 0; floorValid = false; n = 0;
        active = false; below = 0; peak = 0;
        pending.clear(); lastTimes.clear(); hits.clear(); mainsOn = false; lastEndT = -1; gridP = gridT0 = 0;
        clearCounts();
        startUtc = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        snap = SfericsTelemetry();
        snap.seq = seq;
        snap.inputRate = rate;
        snap.centerHz = centerHz;
        snap.thresholdDb = (float)thresholdDb;
    }
    void clearCounts() {
        strikes = flashes = mains = tooLong = 0;
        lastStrikeT = -1; flashNo = 0; lastPeakDb = 0;
        perMinuteMap.clear(); strikeTimes.clear(); recent.clear(); pending.clear();
    }

    static bool matchesMains(double dt) {
        if (dt <= 0 || dt > 0.05) return false;
        for (double T : {0.010, 1.0 / 120.0})
            for (int k = 1; k <= 5; k++) {
                const double e = k * T;
                if (std::fabs(dt - e) <= std::max(0.0003, 0.04 * e)) return true;
            }
        return false;
    }

    void addEvent(double tStart, double tEnd, double peakDb) {
        // pulses closer than the merge time are one strike
        if (!pending.empty() && tStart - pending.back().endT < mergeS) {
            Ev& e = pending.back();
            e.peakDb = std::max(e.peakDb, peakDb);
            e.endT = tEnd;
            e.widthS = tEnd - e.t;
            return;
        }
        Ev e;
        e.t = tStart; e.endT = tEnd; e.peakDb = peakDb; e.widthS = tEnd - tStart;
        double nominal = 0;   // the mains half period this event follows from the one before (0 when none)
        for (double lt : lastTimes) if (matchesMains(tStart - lt)) { e.hit = true; break; }
        if (!lastTimes.empty()) {
            const double dt = tStart - lastTimes.back();
            for (double T : {0.010, 1.0 / 120.0}) if (std::fabs(dt - T) <= std::max(0.0003, 0.04 * T)) nominal = T;
        }
        if (mainsOn && gridP > 0) {   // a train is being ignored: only an impulse on its grid is part of it (a strike falls anywhere)
            const double k = std::round((tStart - gridT0) / gridP);
            e.hit = k >= 1 && std::fabs(tStart - gridT0 - k * gridP) <= 0.0005;
        }
        if (e.hit) gridT0 = tStart;
        if (nominal > 0 && e.hit) gridP = nominal;
        lastTimes.push_back(tStart);
        if (lastTimes.size() > 4) lastTimes.pop_front();
        hits.push_back(e.hit);
        if (hits.size() > kMainsWindow) hits.pop_front();
        if (hits.size() >= kMainsWindow) {
            int h = 0;
            for (bool b : hits) h += b ? 1 : 0;
            if (!mainsOn && h >= kMainsOn) mainsOn = true;
            else if (mainsOn && h < kMainsOff) { mainsOn = false; gridP = 0; }
        }
        pending.push_back(e);
    }

    void commit(double nowT) {
        while (!pending.empty() && pending.front().t < nowT - kCommitDelayS) {
            const Ev e = pending.front();
            pending.pop_front();
            if (mainsFilter && mainsOn && e.hit) { mains++; continue; }
            strikes++;
            if (lastStrikeT < 0 || e.t - lastStrikeT > kFlashGapS) { flashes++; flashNo++; }
            lastStrikeT = e.t;
            lastPeakDb = (float)e.peakDb;
            strikeTimes.push_back(e.t);
            perMinuteMap[(int64_t)std::floor(e.t / 60.0)]++;
            SfericsStrike s;
            s.timeSec = e.t;
            s.utc = startUtc + (int64_t)std::llround(e.t);
            s.peakDb = (float)e.peakDb;
            s.widthUs = (float)(e.widthS * 1e6);
            s.flash = flashNo;
            recent.push_back(s);
            if (recent.size() > kSfericsRecentMax) recent.pop_front();
        }
    }

    void updateFloor() {
        std::vector<float> v(blocks.begin(), blocks.end());
        std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
        floorLin = std::max(1e-14, (double)v[v.size() / 2]);
        floorValid = blocks.size() >= kFloorWarm;
    }

    void snapshot() {
        const double nowT = (double)n / rate;
        while (!strikeTimes.empty() && strikeTimes.front() < nowT - 600.0) strikeTimes.pop_front();
        SfericsTelemetry t;
        t.seq = ++seq;
        t.inputRate = rate;
        t.centerHz = centerHz;
        t.timeSec = nowT;
        t.strikes = strikes; t.flashes = flashes; t.mains = mains; t.tooLong = tooLong;
        t.blocksOk = strikes; t.blocksBad = mains + tooLong;
        t.mainsActive = mainsOn && mainsFilter;
        t.dataValid = strikes > 0;
        t.floorDb = floorValid ? (float)(10 * std::log10(floorLin)) : -200.f;
        t.thresholdDb = (float)thresholdDb;
        t.snrDb = lastPeakDb;
        int last60 = 0;
        for (auto it = strikeTimes.rbegin(); it != strikeTimes.rend() && *it >= nowT - 60.0; ++it) last60++;
        t.perMin1 = last60;
        const double span10 = std::min(600.0, std::max(60.0, nowT));
        t.perMin10 = (double)strikeTimes.size() / (span10 / 60.0);
        t.lastAgoSec = lastStrikeT < 0 ? -1 : nowT - lastStrikeT;
        t.state = n == 0 ? 0 : !floorValid ? 1 : (!strikeTimes.empty() ? 3 : 2);
        t.perMinute.assign(kSfericsMinutes, 0);
        const int64_t curMin = (int64_t)std::floor(nowT / 60.0);
        for (const auto& kv : perMinuteMap) {
            const int64_t age = curMin - kv.first;
            if (age >= 0 && age < kSfericsMinutes) t.perMinute[(size_t)(kSfericsMinutes - 1 - age)] += kv.second;
        }
        while (!perMinuteMap.empty() && curMin - perMinuteMap.begin()->first >= kSfericsMinutes) perMinuteMap.erase(perMinuteMap.begin());
        t.recent.assign(recent.rbegin(), recent.rend());
        snap = t;
    }

    void finish(uint64_t endN) {
        const double w = (double)(endN - startN) / rate;
        const double tStart = (double)startN / rate;
        active = false;
        if (w > kMaxWidthS) { tooLong++; return; }
        addEvent(tStart, (double)endN / rate, 10 * std::log10(std::max(1e-30, peak / floorLin)));
        lastEndT = (double)endN / rate;
    }
};

SfericsReceiver::SfericsReceiver() : p_(new Impl) {}
SfericsReceiver::~SfericsReceiver() = default;

void SfericsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& s = *p_;
    s.rate = inputRateHz;
    s.W = std::max<size_t>(16, (size_t)std::llround(inputRateHz * kSmoothUs * 1e-6));   // at least 16 samples: a low sample rate must not make noise spikes
    s.B = std::max<size_t>(1, (size_t)std::llround(inputRateHz * 1e-3));
    s.hold = std::max<size_t>(1, (size_t)std::llround(inputRateHz * kHoldUs * 1e-6));
    s.clearAll();
}

void SfericsReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->offsetHz = hz; }
void SfericsReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> l(p_->mu); p_->centerHz = hz; p_->snap.centerHz = hz; }
bool SfericsReceiver::ready() const { std::lock_guard<std::mutex> l(p_->mu); return p_->rate >= sfericsTuning().minSampleRate; }
void SfericsReceiver::reset() { std::lock_guard<std::mutex> l(p_->mu); if (p_->rate > 0) p_->clearAll(); }
void SfericsReceiver::setThresholdDb(double db) { std::lock_guard<std::mutex> l(p_->mu); p_->thresholdDb = std::min(30.0, std::max(6.0, db)); }
void SfericsReceiver::setMainsFilter(bool on) { std::lock_guard<std::mutex> l(p_->mu); p_->mainsFilter = on; }
void SfericsReceiver::setMergeMs(double ms) { std::lock_guard<std::mutex> l(p_->mu); p_->mergeS = std::min(5.0, std::max(0.1, ms)) * 1e-3; }
void SfericsReceiver::clearCounts() { std::lock_guard<std::mutex> l(p_->mu); p_->clearCounts(); if (p_->rate > 0) p_->snapshot(); }
void SfericsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->log = std::move(cb); }

void SfericsReceiver::feed(const std::complex<float>* x, size_t cnt) {
    std::lock_guard<std::mutex> l(p_->mu);
    Impl& s = *p_;
    if (s.rate < sfericsTuning().minSampleRate) return;
    const double thrLin = s.floorLin * std::pow(10.0, s.thresholdDb / 10.0);
    double thr = thrLin, endThr = s.floorLin * std::pow(10.0, (s.thresholdDb - 3.0) / 10.0);
    for (size_t i = 0; i < cnt; i++) {
        const float pw = std::norm(x[i]);
        s.wsum += (double)pw - (double)s.win[s.wpos];
        s.win[s.wpos] = pw;
        s.wpos = (s.wpos + 1) % s.W;
        s.n++;
        const double sp = std::max(0.0, s.wsum) / (double)s.W;
        s.blockSum += sp;
        if (++s.blockN >= s.B) {
            s.blocks.push_back((float)(s.blockSum / (double)s.blockN));
            if (s.blocks.size() > kFloorBlocks) s.blocks.pop_front();
            s.blockSum = 0; s.blockN = 0;
            if (++s.blocksSinceMedian >= 100 || !s.floorValid) {
                if (s.blocks.size() >= 20 || s.floorValid) { s.updateFloor(); s.blocksSinceMedian = 0; }
                thr = s.floorLin * std::pow(10.0, s.thresholdDb / 10.0);
                endThr = s.floorLin * std::pow(10.0, (s.thresholdDb - 3.0) / 10.0);
            }
        }
        if (!s.floorValid) continue;
        if (!s.active) {
            if (sp > thr) { s.active = true; s.startN = s.n; s.peak = sp; s.below = 0; }
        } else {
            if (sp > s.peak) s.peak = sp;
            if (sp < endThr) {
                if (++s.below >= s.hold) s.finish(s.n - s.below + 1);
            } else s.below = 0;
        }
    }
    s.commit((double)s.n / s.rate);
    s.sinceSnap += cnt;
    if (s.sinceSnap >= (size_t)(s.rate * 0.1) || s.snap.seq == 0) { s.sinceSnap = 0; s.snapshot(); }
}

bool SfericsReceiver::telemetry(SfericsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    out.centerHz = p_->centerHz;
    return true;
}

ModeTuning sfericsTuning() {
    ModeTuning t;
    t.stdMode = 245;
    t.id = "sferics";
    t.name = "Sferics (lightning)";
    t.minMhz = 0.003;
    t.maxMhz = 30.0;
    t.defMhz = 3.0;
    t.sampleRate = 2e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.0;
    t.minSampleRate = 20000;      // a sound card at 48 kHz works too (timing to 20 us)
    t.tuneOffsetHz = 0;
    t.notCircular = true;         // impulses, not a circular signal: the blind IQ correction would take them for an imbalance
    return t;
}

std::string sfericsSummary(const SfericsTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Sferics: waiting for samples";
    if (t.state == 1) return "Sferics: measuring the noise floor";
    if (t.strikes == 0) snprintf(b, sizeof b, "Sferics: quiet%s", t.mainsActive ? ", mains interference ignored" : "");
    else snprintf(b, sizeof b, "Sferics: %llu strikes, %llu flashes, %.0f / min (%s)%s", (unsigned long long)t.strikes, (unsigned long long)t.flashes,
                  t.perMin1, sfericsActivity(t.perMin1), t.mainsActive ? ", mains ignored" : "");
    return b;
}

}  // namespace dect2
