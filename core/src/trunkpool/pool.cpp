// The trunk pool controller: device roles, allocation of voice radios to grants, the call state machine, preemption, device loss, the call log
// (see trunkpool.h). No threads; time is what the host passes in, so every path is testable with a simulated clock.
#include "dect2/trunkpool/trunkpool.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace tpool {

const char* roleName(Role r) {
    switch (r) {
    case Role::Control: return "control";
    case Role::Voice: return "voice";
    case Role::Idle: return "idle";
    case Role::Lost: return "lost";
    default: return "off";
    }
}
const char* callStateName(CallState s) {
    switch (s) {
    case CallState::Tuning: return "tuning";
    case CallState::Active: return "active";
    case CallState::Hold: return "hold";
    default: return "ended";
    }
}

namespace {
std::string mhz(double hz) { char b[32]; snprintf(b, sizeof b, "%.4f MHz", hz / 1e6); return b; }
}

void TrunkPool::note(const std::string& s) {
    messages.push_back(s);
    if (messages.size() > 200) messages.erase(messages.begin(), messages.begin() + (long)(messages.size() - 200));
}

// ------------------------------------------------------------------------------------------------ devices

int TrunkPool::addDeviceRef(VoiceReceiver* rx) {
    Dev d;
    d.rx = rx;
    d.sliceCalls.assign((size_t)std::max(0, rx->maxSlices()), 0);
    d.wasPresent = rx->present();
    dev_.push_back(std::move(d));
    const int i = (int)dev_.size() - 1;
    if (ctl_ < 0 && rx->canControl() && rx->present()) { ctl_ = i; dev_[(size_t)i].role = Role::Control; }
    return i;
}
int TrunkPool::addDevice(std::unique_ptr<VoiceReceiver> rx) {
    VoiceReceiver* p = rx.get();
    const int i = addDeviceRef(p);
    dev_[(size_t)i].own = std::move(rx);
    return i;
}

void TrunkPool::removeDevice(int i) {
    if (i < 0 || i >= (int)dev_.size() || !dev_[(size_t)i].rx) return;
    deviceLost(i, lastNow_);
    dev_[(size_t)i].rx = nullptr;
    dev_[(size_t)i].own.reset();
    dev_[(size_t)i].role = Role::Off;
    dev_[(size_t)i].enabled = false;
    if (ctl_ == i) { ctl_ = -1; promote(lastNow_); }
}

bool TrunkPool::setControl(int i) {
    if (i < 0 || i >= (int)dev_.size() || !dev_[(size_t)i].rx || !dev_[(size_t)i].rx->canControl() || !dev_[(size_t)i].rx->present()) return false;
    if (i == ctl_) return true;
    Dev& n = dev_[(size_t)i];
    for (const Live& l : live_) if (l.dev == i) return false;   // busy with a call: not now
    if (ctl_ >= 0) dev_[(size_t)ctl_].role = Role::Idle;
    ctl_ = i;
    n.role = Role::Control;
    n.enabled = true;
    return true;
}

void TrunkPool::setEnabled(int i, bool on) {
    if (i < 0 || i >= (int)dev_.size() || !dev_[(size_t)i].rx) return;
    Dev& d = dev_[(size_t)i];
    d.enabled = on;
    if (!on && i != ctl_) {
        std::vector<uint64_t> ids;
        for (const Live& l : live_) if (l.dev == i) ids.push_back(l.id);
        for (uint64_t id : ids) if (Live* l = liveById(id)) endCall(*l, "radio switched off", lastNow_, false);
        d.role = Role::Off;
    } else if (on && d.role == Role::Off) d.role = Role::Idle;
}

DeviceInfo TrunkPool::device(int i) const {
    DeviceInfo o;
    o.index = i;
    if (i < 0 || i >= (int)dev_.size()) return o;
    const Dev& d = dev_[(size_t)i];
    o.name = d.rx ? d.rx->name() : "(removed)";
    o.role = d.role;
    o.present = d.rx && d.rx->present();
    o.enabled = d.enabled;
    o.rx = d.rx ? d.rx->state() : RxState::Idle;
    o.calls = d.calls;
    o.call = d.call;
    if (i == ctl_) o.freqHz = ccHz_;
    for (const Live& l : live_) {
        if (l.dev != i) continue;
        if (l.slice >= 0) o.slices++;
        else { o.call = l.id; }
    }
    uint64_t show = d.call;
    if (!show) for (uint64_t c : d.sliceCalls) if (c) { show = c; break; }
    for (const CallRecord& r : log_) if (r.id == show && show) { o.tg = r.tg; o.freqHz = r.freqHz; o.call = r.id; }
    return o;
}
std::vector<DeviceInfo> TrunkPool::devices() const {
    std::vector<DeviceInfo> v;
    for (int i = 0; i < (int)dev_.size(); i++) v.push_back(device(i));
    return v;
}

void TrunkPool::autoAssign(int hint) {
    for (const Live& l : live_) (void)l;
    int c = -1;
    if (hint >= 0 && hint < (int)dev_.size() && dev_[(size_t)hint].rx && dev_[(size_t)hint].rx->canControl() && dev_[(size_t)hint].rx->present()) c = hint;
    if (c < 0 && ctl_ >= 0 && dev_[(size_t)ctl_].rx && dev_[(size_t)ctl_].rx->present()) c = ctl_;
    if (c < 0) for (int i = 0; i < (int)dev_.size(); i++) if (dev_[(size_t)i].rx && dev_[(size_t)i].rx->canControl() && dev_[(size_t)i].rx->present()) { c = i; break; }
    ctl_ = c;
    int voice = 0;
    for (int i = 0; i < (int)dev_.size(); i++) {
        Dev& d = dev_[(size_t)i];
        if (!d.rx) continue;
        if (i == c) { d.role = Role::Control; d.enabled = true; continue; }
        d.enabled = true;
        if (!d.rx->present()) { d.role = Role::Lost; continue; }
        if (!d.call) d.role = Role::Idle;
        voice++;
    }
    note(c < 0 ? "Auto-assign: no radio can take the control channel" : "Auto-assign: " + dev_[(size_t)c].rx->name() + " on the control channel, " + std::to_string(voice) + " voice radio" + (voice == 1 ? "" : "s"));
}

// ------------------------------------------------------------------------------------------------ helpers

bool TrunkPool::covers(const VoiceReceiver& r, double hz) const {
    const double span = r.captureSpanHz();
    if (!(span > 0)) return false;
    return std::fabs(hz - r.captureCentreHz()) <= span / 2 - cfg_.edgeGuardHz;
}

int TrunkPool::pickCover(double hz) const {
    int best = -1, bestFree = -1;
    for (int i = 0; i < (int)dev_.size(); i++) {
        const Dev& d = dev_[(size_t)i];
        if (!d.rx || !d.enabled || d.role == Role::Lost || d.role == Role::Off || !d.rx->present() || !covers(*d.rx, hz)) continue;
        int free = 0;
        for (uint64_t c : d.sliceCalls) free += c == 0;
        if (!free) continue;
        if (free > bestFree || (free == bestFree && i == ctl_)) { best = i; bestFree = free; }
    }
    return best;
}

int TrunkPool::weakest(int priority, bool emergency) const {
    const int mine = rank(priority, emergency);
    int worst = -1, worstRank = mine;
    double worstStart = 0;
    for (size_t k = 0; k < live_.size(); k++) {
        const CallRecord* r = nullptr;
        for (const CallRecord& x : log_) if (x.id == live_[k].id) { r = &x; break; }
        if (!r || !r->followed) continue;
        const int rr = rank(r->priority, r->emergency);
        if (rr > worstRank || (rr == worstRank && rr > mine && r->startSec < worstStart)) { worst = (int)k; worstRank = rr; worstStart = r->startSec; }
    }
    return worst;
}

TrunkPool::Live* TrunkPool::liveById(uint64_t id) {
    for (Live& l : live_) if (l.id == id) return &l;
    return nullptr;
}

CallRecord& TrunkPool::addRecord(const GrantEvent& g, const AccessDecision& d, double now) {
    CallRecord r;
    r.id = next_++;
    r.wallStart = wallBase + (int64_t)now;
    r.startSec = r.lastVoiceSec = r.lastGrantSec = now;
    r.proto = g.proto; r.tg = g.tg; r.src = g.src; r.slot = g.slot; r.channel = g.channel; r.freqHz = g.freqHz;
    r.priority = d.priority;
    r.emergency = g.emergency; r.group = g.group;
    log_.push_back(r);
    if (log_.size() > 2000) {
        size_t drop = 0;
        while (drop < 200 && drop < log_.size() && !log_[drop].active()) drop++;
        if (drop) log_.erase(log_.begin(), log_.begin() + (long)drop);
    }
    st_.calls++;
    return log_.back();
}

void TrunkPool::miss(CallRecord& r, const char* why, double now) {
    r.missed = true; r.followed = false;
    r.note = why;
    r.state = CallState::Ended;
    r.endSec = now;
    r.endReason = "not followed";
}

// ------------------------------------------------------------------------------------------------ grants

void TrunkPool::onGrant(const GrantEvent& g, double now) {
    lastNow_ = std::max(lastNow_, now);
    st_.grants++;
    onControlSeen(now);
    if (!(g.freqHz > 0)) {
        bool seen = false;
        for (auto& n : noted_) if (n.first == -1 - g.channel) { seen = now - n.second < cfg_.notFollowedLogSec; if (!seen) n.second = now; }
        if (!seen) {
            noted_.push_back({-1.0 - g.channel, now});
            note("Grant for channel " + std::to_string(g.channel) + (g.unplacedType >= 0 ? " has a channel type this build cannot place" : " has no frequency: add it to the channel plan"));
        }
        return;
    }
    // a late grant for a call that just ended (control channel latency) does not open it again
    for (const Ended& e : ended_) if (e.tg == g.tg && std::fabs(e.freq - g.freqHz) < 1 && now - e.sec < cfg_.endedCooldownSec) return;
    for (const auto& b : block_) if (b.first == g.tg && now < b.second) return;

    // a call that is running (followed or not)
    for (CallRecord& r : log_) {
        if (r.tg != g.tg || r.group != g.group || r.state == CallState::Ended) continue;
        if (std::fabs(r.freqHz - g.freqHz) < 1 && r.slot == g.slot) { r.lastGrantSec = now; if (g.src >= 0) r.src = g.src; return; }
        if (Live* l = liveById(r.id)) { endCall(*l, "moved to another channel", now, false); break; }   // the call moved: follow it again on the new channel
    }
    const AccessDecision d = access.decide(g.tg, g.src);
    // a call that was not followed: the repeat of its grant is a new chance, in the same log row
    CallRecord* revive = nullptr;
    for (auto it = log_.rbegin(); it != log_.rend() && now - it->endSec < 3600; ++it) {
        if (it->missed && it->tg == g.tg && it->group == g.group && std::fabs(it->freqHz - g.freqHz) < 1 && it->slot == g.slot && now - it->lastGrantSec <= std::max(2.5, cfg_.hangSec * 2)) { revive = &*it; break; }
    }
    CallRecord* rp = revive;
    if (!rp) {
        rp = &addRecord(g, d, now);
        rp->state = CallState::Ended;   // until it is followed
        rp->endSec = now;
    } else { rp->lastGrantSec = now; rp->endSec = now; rp->priority = d.priority; if (g.src >= 0) rp->src = g.src; }
    const uint64_t id = rp->id;
    if (g.encrypted) {
        if (!revive) { rp->encrypted = true; st_.encrypted++; miss(*rp, "encrypted: never followed", now); }
        return;
    }
    if (!d.allow) {
        if (!revive) { st_.missedDenied++; miss(*rp, d.reason.empty() ? "denied" : d.reason.c_str(), now); }
        return;
    }
    start(g, d, now);
    (void)id;
}

void TrunkPool::assign(Live& l, CallRecord& r, int dev, int slice, const TuneRequest& tr, double now) {
    (void)tr;
    l.dev = dev; l.slice = slice; l.tuneAt = now;
    r.device = dev; r.slice = slice;
    r.deviceName = dev_[(size_t)dev].rx->name();
    r.followed = true; r.missed = false; r.note.clear(); r.endReason.clear();
    r.state = CallState::Tuning;
    r.startSec = now; r.lastVoiceSec = now; r.lastGrantSec = now; r.endSec = 0;
    r.wallStart = wallBase + (int64_t)now;
    dev_[(size_t)dev].calls++;
    if (slice >= 0) dev_[(size_t)dev].sliceCalls[(size_t)slice] = r.id;
    else { dev_[(size_t)dev].call = r.id; if (dev != ctl_) dev_[(size_t)dev].role = Role::Voice; }
    st_.followed++;
}

void TrunkPool::start(const GrantEvent& g, const AccessDecision& d, double now) {
    // find the log row (the newest for this grant)
    CallRecord* rp = nullptr;
    for (auto it = log_.rbegin(); it != log_.rend(); ++it) if (it->tg == g.tg && it->group == g.group && std::fabs(it->freqHz - g.freqHz) < 1 && it->slot == g.slot) { rp = &*it; break; }
    if (!rp) return;
    const uint64_t id = rp->id;
    auto row = [&]() -> CallRecord& { for (CallRecord& x : log_) if (x.id == id) return x; return log_.back(); };
    TuneRequest tr;
    tr.freqHz = g.freqHz; tr.proto = g.proto; tr.slot = g.slot; tr.tg = g.tg; tr.src = g.src;
    int active = 0;
    for (const Live& l : live_) (void)l, active++;
    if (active >= cfg_.maxCalls) {
        if (!rp->missed) st_.missedBusy++;
        miss(*rp, "busy: call limit reached", now);
        return;
    }
    const int myRank = rank(d.priority, g.emergency);
    for (int attempt = 0; attempt < 2; attempt++) {
        // 1. a radio whose capture already holds the channel: a slice, no retune
        if (cfg_.allowSlices) {
            const int c = pickCover(g.freqHz);
            if (c >= 0) {
                int slice = -1;
                if (dev_[(size_t)c].rx->openSlice(tr, &slice) && slice >= 0 && slice < (int)dev_[(size_t)c].sliceCalls.size() && dev_[(size_t)c].sliceCalls[(size_t)slice] == 0) {
                    Live l; l.id = id;
                    CallRecord& r = row();
                    l.record = d.record;
                    live_.push_back(std::move(l));
                    assign(live_.back(), r, c, slice, tr, now);
                    r.priority = d.priority;
                    if (onCallStart) onCallStart(r);
                    return;
                }
            }
        }
        // 2. an idle voice radio, the one whose capture is nearest the channel first
        std::vector<int> cand;
        for (int i = 0; i < (int)dev_.size(); i++) {
            const Dev& dv = dev_[(size_t)i];
            if (i == ctl_ || !dv.rx || !dv.enabled || dv.role == Role::Lost || dv.role == Role::Off || !dv.rx->present() || dv.call) continue;
            bool sl = false;
            for (uint64_t c : dv.sliceCalls) sl = sl || c;
            if (sl) continue;
            cand.push_back(i);
        }
        std::sort(cand.begin(), cand.end(), [&](int a, int b) {
            const double da = std::fabs(dev_[(size_t)a].rx->captureCentreHz() - g.freqHz), db = std::fabs(dev_[(size_t)b].rx->captureCentreHz() - g.freqHz);
            return da != db ? da < db : dev_[(size_t)a].calls < dev_[(size_t)b].calls;
        });
        for (int i : cand) {
            if (!dev_[(size_t)i].rx->tune(tr)) continue;
            Live l; l.id = id;
            CallRecord& r = row();
            l.record = d.record;
            live_.push_back(std::move(l));
            assign(live_.back(), r, i, -1, tr, now);
            r.priority = d.priority;
            if (onCallStart) onCallStart(r);
            return;
        }
        // 3. a call of lower priority gives its radio up
        if (attempt == 0 && cfg_.preempt) {
            const int w = weakest(d.priority, g.emergency);
            if (w >= 0) {
                const CallRecord* wr = nullptr;
                for (const CallRecord& x : log_) if (x.id == live_[(size_t)w].id) wr = &x;
                if (wr && rank(wr->priority, wr->emergency) > myRank) {
                    note("TG " + std::to_string(g.tg) + " takes the radio from TG " + std::to_string(wr->tg));
                    st_.preempted++;
                    endCall(live_[(size_t)w], "preempted", now, false);
                    continue;
                }
            }
        }
        break;
    }
    // nothing could take it
    CallRecord& r = row();
    int voiceRadios = 0;
    for (int i = 0; i < (int)dev_.size(); i++) if (i != ctl_ && dev_[(size_t)i].rx && dev_[(size_t)i].enabled && dev_[(size_t)i].role != Role::Lost) voiceRadios++;
    if (voiceRadios == 0) {
        const bool first = !r.missed;
        char b[220];
        const double span = ctl_ >= 0 && dev_[(size_t)ctl_].rx ? dev_[(size_t)ctl_].rx->captureSpanHz() : 0;
        snprintf(b, sizeof b, "outside the capture (%s, %.1f MHz wide): add a second radio or widen the capture", mhz(g.freqHz).c_str(), span / 1e6);
        why_ = std::string("Control channel only: ") + b;
        bool seen = false;
        for (auto& n : noted_) if (std::fabs(n.first - g.freqHz) < 1) { seen = now - n.second < cfg_.notFollowedLogSec; if (!seen) n.second = now; }
        if (!seen) { noted_.push_back({g.freqHz, now}); note(std::string("Call on ") + b); }
        if (first) st_.missedOutside++;
        miss(r, b, now);
    } else {
        if (!r.missed) st_.missedBusy++;
        miss(r, "busy: every voice radio is on a call", now);
    }
}

// ------------------------------------------------------------------------------------------------ call end, device loss

void TrunkPool::freeDevice(Live& l) {
    if (l.dev < 0 || l.dev >= (int)dev_.size()) return;
    Dev& d = dev_[(size_t)l.dev];
    if (!d.rx) return;
    if (l.slice >= 0) {
        if ((size_t)l.slice < d.sliceCalls.size()) d.sliceCalls[(size_t)l.slice] = 0;
        d.rx->closeSlice(l.slice);
    } else {
        d.call = 0;
        d.rx->release();
        if (l.dev != ctl_ && d.role == Role::Voice) d.role = Role::Idle;
    }
}

void TrunkPool::endCall(Live& l, const char* why, double now, bool cooldown) {
    CallRecord* rp = nullptr;
    for (CallRecord& x : log_) if (x.id == l.id) { rp = &x; break; }
    const uint64_t id = l.id;
    if (rp) {
        pushAudio(l, now);
        rp->state = CallState::Ended;
        rp->endSec = now;
        rp->endReason = why;
        if (cooldown) { ended_.push_back({rp->tg, rp->freqHz, now}); if (ended_.size() > 64) ended_.erase(ended_.begin()); }
        CallMeta m;
        m.id = rp->id; m.proto = rp->proto; m.system = cfg_.systemName; m.device = rp->deviceName; m.endReason = why;
        m.tg = rp->tg; m.src = rp->src; m.slot = rp->slot; m.freqHz = rp->freqHz; m.wallStart = rp->wallStart;
        m.durationSec = std::max(0.0, rp->lastVoiceSec - rp->startSec); m.setupSec = rp->setupSec; m.emergency = rp->emergency; m.group = rp->group;
        if (l.recorder && l.recorder->open()) {
            const std::string p = l.recorder->finish(&m);
            if (!p.empty()) { rp->audioPath = p; st_.recordings++; }
            rp->audioSec = m.audioSec;
        } else if (cfg_.recordMetaOnly && !cfg_.recordDir.empty() && cfg_.record) {   // no sound (no vocoder): the call's data is still kept
            CallRecorder::finishMetaOnly(cfg_.recordDir, m);
        }
    }
    freeDevice(l);
    for (size_t k = 0; k < live_.size(); k++) if (live_[k].id == id) { live_.erase(live_.begin() + (long)k); break; }
    if (rp && onCallEnd) onCallEnd(*rp);
}

void TrunkPool::deviceLost(int di, double now) {
    Dev& d = dev_[(size_t)di];
    for (size_t k = 0; k < live_.size();) {
        if (live_[k].dev == di) {
            st_.deviceLost++;
            note("Radio " + (d.rx ? d.rx->name() : std::string("?")) + " lost: call ended");
            Live& l = live_[k];
            // the radio is gone: nothing to release
            const int sl = l.slice;
            (void)sl;
            l.dev = -1;
            endCall(l, "device lost", now, false);
        } else k++;
    }
    d.call = 0;
    std::fill(d.sliceCalls.begin(), d.sliceCalls.end(), 0);
    if (di == ctl_) { d.role = Role::Lost; }
    else d.role = Role::Lost;
}

void TrunkPool::promote(double now) {
    if (!cfg_.promoteControl || ctl_ >= 0) { if (ctl_ < 0) why_ = "No control radio"; return; }
    for (int i = 0; i < (int)dev_.size(); i++) {
        Dev& d = dev_[(size_t)i];
        if (!d.rx || !d.enabled || d.role == Role::Lost || d.role == Role::Off || !d.rx->present() || !d.rx->canControl() || d.call) continue;
        bool sl = false;
        for (uint64_t c : d.sliceCalls) sl = sl || c;
        if (sl) continue;
        TuneRequest tr; tr.freqHz = ccHz_; tr.control = true;
        if (ccHz_ > 0 && !d.rx->tune(tr)) continue;
        ctl_ = i; d.role = Role::Control;
        ccSeen_ = true; lastCc_ = now;
        why_.clear();
        note("Control radio lost: " + d.rx->name() + " now on the control channel");
        return;
    }
    why_ = "No control radio: every other radio is busy or gone";
}

// ------------------------------------------------------------------------------------------------ time

void TrunkPool::pushAudio(Live& l, double now) {
    (void)now;
    if (l.dev < 0 || l.dev >= (int)dev_.size() || !dev_[(size_t)l.dev].rx) return;
    VoiceReceiver& rx = *dev_[(size_t)l.dev].rx;
    float buf[4096];
    for (int it = 0; it < 8; it++) {
        int rate = 0;
        const int n = rx.pullAudio(l.slice, buf, 4096, &rate);
        if (n <= 0 || rate <= 0) break;
        if (!l.record || !cfg_.record || cfg_.recordDir.empty()) continue;
        if (!l.recorder) {
            l.recorder.reset(new CallRecorder());
            CallMeta m;
            for (const CallRecord& x : log_) if (x.id == l.id) { m.id = x.id; m.proto = x.proto; m.tg = x.tg; m.src = x.src; m.slot = x.slot; m.freqHz = x.freqHz; m.wallStart = x.wallStart; m.device = x.deviceName; m.emergency = x.emergency; m.group = x.group; }
            m.system = cfg_.systemName;
            std::string err;
            if (!l.recorder->begin(cfg_.recordDir, m, &err)) { note("Recording failed: " + err); l.record = false; l.recorder.reset(); continue; }
        }
        l.recorder->feed(buf, n, rate);
    }
}

void TrunkPool::tick(double now) {
    lastNow_ = std::max(lastNow_, now);
    // devices appearing and disappearing
    for (int i = 0; i < (int)dev_.size(); i++) {
        Dev& d = dev_[(size_t)i];
        if (!d.rx) continue;
        const bool p = d.rx->present();
        if (d.wasPresent && !p) { d.wasPresent = false; deviceLost(i, now); if (i == ctl_) { ctl_ = -1; promote(now); } }
        else if (!d.wasPresent && p) {
            d.wasPresent = true;
            if (d.enabled) { d.role = Role::Idle; note("Radio " + d.rx->name() + " is back"); }
            if (ctl_ < 0) promote(now);
        }
    }
    if (ctl_ < 0 && ccSeen_) promote(now);
    std::vector<uint64_t> ids;
    for (const Live& l : live_) ids.push_back(l.id);
    for (uint64_t id : ids) {
        Live* lp = liveById(id);
        if (!lp) continue;
        Live& l = *lp;
        CallRecord* rp = nullptr;
        for (CallRecord& x : log_) if (x.id == id) { rp = &x; break; }
        if (!rp || l.dev < 0 || !dev_[(size_t)l.dev].rx) { endCall(l, "device lost", now, false); continue; }
        VoiceReceiver& rx = *dev_[(size_t)l.dev].rx;
        CallRecord& r = *rp;
        const bool hearing = l.slice >= 0 ? rx.sliceHearing(l.slice) : rx.hearingVoice();
        if (rx.encryptedHeard()) { r.encrypted = true; st_.encrypted++; endCall(l, "encrypted (heard on the voice channel): left", now, true); continue; }
        if (r.state == CallState::Tuning) {
            const RxState s = l.slice >= 0 ? RxState::Locked : rx.state();
            if (hearing || s == RxState::Locked) {
                r.state = hearing ? CallState::Active : CallState::Hold;
                r.setupSec = now - l.tuneAt;
                if (l.slice < 0) { st_.retunes++; retuneSum_ += r.setupSec; st_.retuneAvgSec = retuneSum_ / st_.retunes; st_.retuneMaxSec = std::max(st_.retuneMaxSec, r.setupSec); }
                if (hearing) r.lastVoiceSec = now;
            } else if (s == RxState::Failed || now - l.tuneAt > cfg_.maxSetupSec) {
                st_.missedNoSignal++;
                block_.push_back({r.tg, now + cfg_.setupBackoffSec});
                r.missed = true;
                const int tg = r.tg; (void)tg;
                endCall(l, "no signal", now, false);
                r.missed = true; r.followed = false; r.note = "no signal on the voice channel: the radio never heard it";
                continue;
            }
        } else {
            if (hearing) { r.lastVoiceSec = now; r.state = CallState::Active; }
            else if (r.state == CallState::Active) r.state = CallState::Hold;
        }
        if (l.slice < 0 && rx.terminated()) { endCall(l, "terminator", now, true); continue; }
        if (r.state != CallState::Tuning && now - std::max(r.lastVoiceSec, r.lastGrantSec) > cfg_.hangSec) { endCall(l, "hang time over", now, true); continue; }
        if (now - r.startSec > cfg_.maxCallSec) { endCall(l, "too long", now, true); continue; }
        if (l.slice < 0 || r.state != CallState::Tuning) pushAudio(l, now);
    }
    // calls nobody follows end by silence on the control channel
    for (CallRecord& r : log_) if (r.missed && r.state == CallState::Ended && now - r.lastGrantSec > cfg_.hangSec * 2 && r.endReason == "not followed") r.endReason = "ended";
    for (size_t i = 0; i < ended_.size();) { if (now - ended_[i].sec > cfg_.endedCooldownSec * 4) ended_.erase(ended_.begin() + (long)i); else i++; }
    for (size_t i = 0; i < block_.size();) { if (now > block_[i].second) block_.erase(block_.begin() + (long)i); else i++; }
    // the single-radio message goes away when a voice radio exists
    int voiceRadios = 0;
    for (int i = 0; i < (int)dev_.size(); i++) if (i != ctl_ && dev_[(size_t)i].rx && dev_[(size_t)i].enabled && dev_[(size_t)i].role != Role::Lost) voiceRadios++;
    if (voiceRadios > 0 && ctl_ >= 0) why_.clear();
}

// ------------------------------------------------------------------------------------------------ reporting

std::vector<const CallRecord*> TrunkPool::nowTalking() const {
    std::vector<const CallRecord*> v;
    for (const CallRecord& r : log_) if (r.active()) v.push_back(&r);
    return v;
}

std::string TrunkPool::statusLine(double now) const {
    if (ctl_ < 0) return why_.empty() ? "No control radio" : why_;
    if (controlLost(now)) return "Control channel lost: no messages for " + std::to_string((int)(now - lastCc_)) + " s";
    int calls = 0, voice = 0;
    const CallRecord* first = nullptr;
    for (const CallRecord& r : log_) if (r.active()) { calls++; if (!first) first = &r; }
    for (int i = 0; i < (int)dev_.size(); i++) if (i != ctl_ && dev_[(size_t)i].rx && dev_[(size_t)i].enabled && dev_[(size_t)i].role != Role::Lost) voice++;
    if (calls) {
        char b[200];
        snprintf(b, sizeof b, "Following %d call%s: TG %d on %s%s", calls, calls == 1 ? "" : "s", first->tg, mhz(first->freqHz).c_str(), calls > 1 ? " and more" : "");
        return b;
    }
    if (!why_.empty()) return why_;
    return "Listening on the control channel, " + std::to_string(voice) + " voice radio" + (voice == 1 ? "" : "s") + " ready";
}

std::string TrunkPool::callLogCsv() const {
    std::string o = "id,start_unix,protocol,talkgroup,source,frequency_mhz,slot,duration_s,state,followed,device,setup_s,note,audio\n";
    char b[400];
    for (const CallRecord& r : log_) {
        std::string note = r.note.empty() ? r.endReason : r.note;
        std::string q;
        for (char c : note) { if (c == '"') q += "\"\""; else q += c; }
        snprintf(b, sizeof b, "%llu,%lld,%s,%d,%d,%.5f,%d,%.1f,%s,%d,", (unsigned long long)r.id, (long long)r.wallStart, protoName(r.proto), r.tg, r.src, r.freqHz / 1e6, r.slot, r.durationSec(), callStateName(r.state), r.followed ? 1 : 0);
        o += b;
        o += "\"" + r.deviceName + "\"";
        snprintf(b, sizeof b, ",%.3f,\"", r.setupSec);
        o += b;
        o += q + "\",\"" + r.audioPath + "\"\n";
    }
    return o;
}

void TrunkPool::clear() {
    while (!live_.empty()) endCall(live_.back(), "cleared", lastNow_, false);
    log_.clear(); ended_.clear(); block_.clear(); noted_.clear(); messages.clear(); st_ = PoolStats(); retuneSum_ = 0; why_.clear();
}

// ------------------------------------------------------------------------------------------------ adapters

GrantEvent grantMpt1327(int channel, int controlChannel, double controlHz, double spacingHz, int called, int caller, bool group) {
    GrantEvent g;
    g.proto = Proto::Mpt1327; g.tg = called; g.src = caller; g.channel = channel; g.group = group;
    if (channel >= 0 && controlChannel >= 0 && spacingHz > 0 && controlHz > 0) {
        const double f = controlHz + (double)(channel - controlChannel) * spacingHz;
        g.freqHz = f > 0 ? f : 0;
    }
    return g;
}

GrantEvent grantFromLcn(Proto p, const ChannelPlan& plan, int lcn, int tg, int src, bool group, bool emergency, bool encrypted, int slot) {
    GrantEvent g;
    g.proto = p; g.tg = tg; g.src = src; g.channel = lcn; g.group = group; g.emergency = emergency; g.encrypted = encrypted; g.slot = slot;
    g.freqHz = plan.channelHz(lcn);
    return g;
}

GrantEvent grantTetra(double baseHz, int carrier, int offsetCode, int slot, int ssi, int gssi, bool encrypted) {
    static const double off[4] = {0, 6250, -6250, 12500};
    GrantEvent g;
    g.proto = Proto::Tetra; g.tg = gssi; g.src = ssi; g.channel = carrier; g.slot = slot; g.encrypted = encrypted;
    if (baseHz > 0 && carrier >= 0 && offsetCode >= 0 && offsetCode < 4) g.freqHz = baseHz + carrier * 25000.0 + off[offsetCode];
    return g;
}

} // namespace tpool
} // namespace dect2
