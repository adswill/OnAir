// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LTR receiver: FM front end at 9600 Hz, 150 Hz low pass, decimate to 2400 Hz (8 samples per bit), integrate-and-dump slicing at every
// phase, and a search for the 9-bit sync in both polarities. Neighbouring phases that hit the same word are merged (the middle one is used).
#include "dect2/ltr/ltr_rx.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_tel.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

ModeTuning ltrTuning() {
    ModeTuning t;
    t.stdMode = 131;
    t.id = "ltr";
    t.name = "LTR trunking";
    t.minMhz = 29.7;
    t.maxMhz = 960.0;
    t.defMhz = 460.0;
    t.sampleRate = 480000;
    t.basebandHz = 150000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 240000;
    t.tuneOffsetHz = 50000;
    return t;
}

namespace {
constexpr double kWork = 9600.0;
constexpr int kDecim = 4;
constexpr int kSps = 8;                 // samples per bit at 2400 Hz
constexpr double kMinRangeHz = 80.0;    // the data must swing at least this much (peak to peak) to be believed
constexpr double kLockSec = 15.0;
}

struct LtrReceiver::Impl {
    std::mutex mu;
    LtrTelemetry tel;
    std::function<void(const std::string&)> log;
    FmFront fm;
    FmBiquad lp[2];
    EyeEnvelope env;
    std::vector<float> disc;
    double fs = 0, offHz = 0, centreHz = 0;
    bool rateSet = false;
    int dcount = 0;
    uint64_t n = 0;                        // 2400 Hz sample counter
    double ring[512] = {};                  // last samples
    uint64_t reg[kSps] = {};               // last 40 bits per phase (hard decisions)
    struct Hit { uint64_t n; uint64_t word; bool inv; };
    std::vector<Hit> hits;
    uint64_t suppressUntil = 0, firstHit = 0;
    double lastGoodSec = -1e9, lastPub = 0;
    std::map<int, LtrRepeater> rep;

    double timeSec() const { return (double)n / (kWork / kDecim); }

    void clearState() {
        fm.reset(); lp[0].clear(); lp[1].clear(); env.clear();
        n = 0; dcount = 0; for (auto& r : ring) r = 0; for (auto& r : reg) r = 0;
        hits.clear(); suppressUntil = firstHit = 0; lastGoodSec = -1e9; lastPub = 0; rep.clear();
        const uint64_t s = tel.seq;
        tel = LtrTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.centerHz = centreHz; tel.state = rateSet ? 1 : 0;
    }

    void handle(const Hit& h) {
        uint64_t w = h.word & ((1ULL << 40) - 1);
        if (h.inv) w = ~w & ((1ULL << 40) - 1);
        uint8_t bits[ltr::kWordBits];
        for (int i = 0; i < ltr::kWordBits; i++) bits[i] = (uint8_t)((w >> (39 - i)) & 1);
        ltr::Word word;
        if (!ltr::parseWord(bits, word)) return;
        tel.syncs++;
        if (env.range() < kMinRangeHz) return;
        // eye check: every bit of the word, integrated over its 8 samples, must sit clearly on one side of the threshold
        {
            const double thr = env.mid(), half = 0.5 * env.range();
            for (int i = 0; i < ltr::kWordBits; i++) {
                const uint64_t last = h.n - 1 - (uint64_t)(ltr::kWordBits - 1 - i) * kSps;
                double sum = 0;
                for (int k = 0; k < kSps; k++) sum += ring[(last - (uint64_t)k) & 511];
                if (std::fabs(sum / kSps - thr) < 0.15 * half) return;
            }
        }
        if (!ltr::checkOk(word, h.inv) || !ltr::plausible(word)) { tel.blocksBad++; return; }
        LtrMessage m;
        m.wallTime = (int64_t)std::time(nullptr);
        m.timeSec = timeSec();
        m.area = word.area; m.gotoCh = word.gotoCh; m.home = word.home; m.id = word.id; m.freeCh = word.freeCh; m.chk = word.chk;
        m.plausible = true; m.chkOk = true;
        m.talkgroup = ltr::talkgroup(word);
        m.kind = ltr::kindOf(word.id);
        auto touch = [&](int ch, int which) {
            if (ch < 1 || ch > 20) return;
            LtrRepeater& r = rep[ch];
            r.ch = ch; r.lastSec = m.timeSec;
            (which == 0 ? r.asGoto : which == 1 ? r.asHome : r.asFree)++;
        };
        touch(word.gotoCh, 0); touch(word.home, 1); touch(word.freeCh, 2);
        tel.repeaters.clear();
        for (auto& kv : rep) tel.repeaters.push_back(kv.second);
        tel.area = word.area;
        if (tel.messages.size() >= kLtrLogMax) tel.messages.pop_back();
        tel.messages.insert(tel.messages.begin(), m);
        tel.blocksOk++;
        tel.dataValid = true;
        lastGoodSec = m.timeSec;
        if (log) log("LTR " + m.talkgroup + (m.kind.empty() ? "" : " " + m.kind));
    }

    void sample(double s) {
        ring[n & 511] = s;
        env.push(s);
        const double thr = env.mid();
        double sum = 0;
        for (int k = 0; k < kSps; k++) sum += ring[(n - (uint64_t)k) & 511];
        const int ph = (int)(n % kSps);
        reg[ph] = (reg[ph] << 1) | (sum / kSps > thr ? 1ULL : 0ULL);
        n++;
        if (n > 400 && n >= suppressUntil) {
            const uint32_t top = (uint32_t)((reg[ph] >> 31) & 0x1FF);
            if (top == ltr::kSync || top == (~ltr::kSync & 0x1FF)) {
                if (hits.empty()) firstHit = n;
                hits.push_back({n, reg[ph], top != ltr::kSync});
            }
        }
        if (!hits.empty() && n >= firstHit + kSps / 2) {   // the hits of the neighbouring phases are in
            const Hit h = hits[hits.size() / 2];
            hits.clear();
            suppressUntil = n + 38 * kSps;   // the next word is at least 40 bits away
            handle(h);
        }
    }

    void publish() {
        tel.levelDb = (float)fm.levelDb();
        tel.deviationHz = (float)env.range();
        tel.timeSec = timeSec();
        tel.locked = timeSec() - lastGoodSec < kLockSec;
        tel.state = rateSet ? (tel.locked ? 2 : 1) : 0;
        tel.snrDb = 0;
        tel.seq++;
    }
};

LtrReceiver::LtrReceiver() : p_(std::make_unique<Impl>()) {}
LtrReceiver::~LtrReceiver() = default;

void LtrReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->rateSet = inputRateHz >= ltrTuning().minSampleRate - 1 && p_->fm.configure(inputRateHz, kWork, 3500.0);   // 4.0: 3.5 kHz (was 4.5) with the carrier follow-up, see docs/SENSITIVITY-4.0-A3.md
    p_->fm.setOffset(p_->offHz);
    p_->fm.setAfc(true, 4000.0, 9000.0);
    p_->lp[0].lowpass(150.0, kWork, 0.5412);
    p_->lp[1].lowpass(150.0, kWork, 1.3066);
    p_->clearState();
}
void LtrReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->fm.setOffset(hz); }
void LtrReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centreHz = hz; p_->tel.centerHz = hz; }
bool LtrReceiver::ready() const { return p_->rateSet; }
void LtrReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void LtrReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear(); p_->tel.repeaters.clear(); p_->rep.clear(); p_->tel.dataValid = false; p_->tel.seq++;
}
void LtrReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rateSet) { p_->tel.state = 0; return; }
    p_->disc.clear();
    p_->fm.feed(x, n, p_->disc);
    for (float d : p_->disc) {
        const double y = p_->lp[1].run(p_->lp[0].run(d));
        if (++p_->dcount == kDecim) { p_->dcount = 0; p_->sample(y); }
    }
    if (p_->timeSec() - p_->lastPub >= 0.25) { p_->lastPub = p_->timeSec(); p_->publish(); }
}
bool LtrReceiver::telemetry(LtrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void LtrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
