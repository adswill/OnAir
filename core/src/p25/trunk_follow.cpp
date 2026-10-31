// Trunking follow logic: grant -> follow -> end -> back to the control channel. No DSP: the host executes the Actions and reports back what it hears.
#include "dect2/p25/trunk.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace dect2 {
namespace trunk {

const char* followStateName(FollowState s) {
    switch (s) {
    case FollowState::NoControl: return "no control channel";
    case FollowState::ControlHold: return "on the control channel";
    case FollowState::Following: return "following calls";
    case FollowState::Hopped: return "left the control channel for a call";
    case FollowState::Returning: return "returning to the control channel";
    case FollowState::Searching: return "looking for the control channel";
    }
    return "?";
}

namespace {
int effPrio(int p) { return p <= 0 ? 11 : p; }   // 1 is the highest; none counts as the lowest
}

void Follower::reset() {
    slots_.clear();
    out_.clear();
    state_ = FollowState::NoControl;
    lastCcSec_ = -1e9;
    missed_ = 0;
    noticed_.clear();
    ccHz_ = 0; radioHz_ = 0; candIdx_ = 0;
    cands_.clear();
    good_.clear();
}

void Follower::setControlChannel(double hz, const std::vector<double>& cands) {
    if (hz > 0) ccHz_ = hz;
    cands_ = cands;
    if (ccHz_ > 0 && std::none_of(cands_.begin(), cands_.end(), [&](double c) { return std::fabs(c - ccHz_) < 1; })) cands_.insert(cands_.begin(), ccHz_);
}

bool Follower::inSpan(double f) const {
    if (spanHz_ <= 0 || f <= 0) return false;
    return std::fabs(f - centerHz_) <= spanHz_ / 2 - cfg_.edgeGuardHz;
}

int Follower::activeSlices() const {
    int n = 0;
    for (const Slot& s : slots_) if (s.used && !s.hopped) n++;   // a call on the voice radio counts: the control channel radio is still following
    return n;
}

std::vector<double> Follower::sliceFreqs(bool tdma) const {
    std::vector<double> v;
    for (const Slot& s : slots_) if (s.used && !s.hopped && !s.remote && s.tdma == tdma) v.push_back(s.freqHz);   // not the voice radio's: the wide bank is for the capture
    return v;
}

uint64_t Follower::audibleCall() const {
    const Slot* best = nullptr;
    for (const Slot& s : slots_) {
        if (!s.used) continue;
        if (!best || effPrio(s.prio) < effPrio(best->prio) || (effPrio(s.prio) == effPrio(best->prio) && s.startSec < best->startSec)) best = &s;
    }
    return best ? best->callId : 0;
}

Follower::Slot* Follower::freeSlot() {
    for (Slot& s : slots_) if (!s.used) return &s;
    if ((int)slots_.size() < std::max(1, cfg_.maxSlices)) { slots_.emplace_back(); return &slots_.back(); }
    return nullptr;
}

void Follower::close(Slot& s, CallLog& log, double now, const char* why) {
    if (!s.used) return;
    const int idx = (int)(&s - slots_.data());
    Action a;
    a.kind = Action::RecordStop; a.callId = s.callId; a.slice = idx;
    if (cfg_.record) out_.push_back(a);
    a.kind = s.remote ? Action::CloseRemote : s.hopped ? Action::Notice : Action::CloseSlice;
    a.tdma = s.tdma;
    if (!s.hopped) out_.push_back(a);
    if (s.remote && !s.voiced && !s.leave) {   // the voice radio never heard it: logged as not followed, and not hammered with the next repeat
        remoteBlockUntil_ = now + cfg_.remoteBackoffSec;
        if (CallRecord* r = log.byId(s.callId)) { r->followed = false; r->missed = true; missed_++; }
    }
    if (CallRecord* r = log.byId(s.callId)) {
        if (!s.leave) {
            r->active = false;
            r->durSec = std::max(0.0, std::max(s.lastSec, r->lastSec) - r->startSec);
        }
        if (why && *why && r->note.empty()) r->note = why;
    }
    s.used = false; s.leave = false; s.remote = false; s.voiced = false;
}

bool Follower::remoteActive() const { return remoteCall() != 0; }
uint64_t Follower::remoteCall() const {
    for (const Slot& s : slots_) if (s.used && s.remote) return s.callId;
    return 0;
}

void Follower::remoteFailed(uint64_t callId, const std::string& why, double now, CallLog& log) {
    lastNow_ = now;
    for (Slot& s : slots_) {
        if (!s.used || !s.remote || s.callId != callId) continue;
        if (CallRecord* r = log.byId(callId)) {
            r->followed = false; r->missed = true; r->note = "voice radio: " + why;
            missed_++;
        }
        s.voiced = true;   // a failure the host named: no extra back-off for "heard nothing"
        remoteBlockUntil_ = now + cfg_.remoteBackoffSec;
        close(s, log, now, "");
        note("Voice radio could not take the call: " + why);
        return;
    }
}

void Follower::leave(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_)
        if (s.used && s.callId == callId) { s.leave = true; s.lastSec = now - cfg_.hangSec - 0.001; }   // closed at the next tick, the call stays in the log
}

// Where the radio is now, as far as known: the last retune, else the capture centre of a single-channel capture
double Follower::tunedHz() const { return radioHz_ > 0 ? radioHz_ : spanHz_ <= 0 ? centerHz_ : 0; }

// The order of a control channel hunt: the frequencies a control message was really heard on, the most recent first (a site that moved its control
// channel, or a neighbour list gone stale, does not send the hunt on a tour of guesses before it returns to what worked), then the system's own
// candidates. The heard list is kept for the session (reset() clears it) and holds the last few only.
std::vector<double> Follower::searchList() const {
    std::vector<std::pair<double, double>> g = good_;
    std::stable_sort(g.begin(), g.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<double> v;
    auto add = [&](double hz) {
        if (hz <= 0) return;
        for (double o : v) if (std::fabs(o - hz) < 1) return;
        v.push_back(hz);
    };
    for (const auto& x : g) add(x.first);
    for (double c : cands_) add(c);
    return v;
}

// Start (or step) the control channel search at a frequency. Already tuned there: no retune (it would only restart the receiver's acquisition for nothing)
void Follower::searchAt(double hz, double now) {
    const double cur = tunedHz();
    if (!(cur > 0 && std::fabs(cur - hz) < 1)) {
        Action a;
        a.kind = Action::Retune; a.freqHz = hz;
        out_.push_back(a);
    }
    radioHz_ = hz;
    dwellStart_ = now;
}

void Follower::goHome(double now) {
    const double home = ccHz_ > 0 ? ccHz_ : homeHz_;
    if (home <= 0) { setState(FollowState::NoControl, now); return; }
    Action a;
    a.kind = Action::Retune; a.freqHz = home;
    out_.push_back(a);
    radioHz_ = home;
    setState(FollowState::Returning, now);
}

void Follower::onControlSeen(double now) {
    lastCcSec_ = now;
    if (state_ != FollowState::Hopped && state_ != FollowState::Following) {
        const double f = radioHz_ > 0 ? radioHz_ : ccHz_ > 0 ? ccHz_ : centerHz_;
        if (f > 0) {
            bool found = false;
            for (auto& x : good_) if (std::fabs(x.first - f) < 1) { x.second = now; found = true; }
            if (!found) {
                good_.push_back({f, now});
                if (good_.size() > 4) {
                    size_t old = 0;
                    for (size_t i = 1; i < good_.size(); i++) if (good_[i].second < good_[old].second) old = i;
                    good_.erase(good_.begin() + (long)old);
                }
            }
        }
    }
    switch (state_) {
    case FollowState::NoControl: setState(FollowState::ControlHold, now); break;
    case FollowState::Returning: setState(FollowState::ControlHold, now); break;
    case FollowState::Searching:
        if (radioHz_ > 0) ccHz_ = radioHz_;
        setState(FollowState::ControlHold, now);
        break;
    default: break;
    }
}

void Follower::onGrant(const Grant& g, const TalkgroupList::Decision& d, double now, CallLog& log) {
    lastNow_ = now;
    // a repeat of a call that is running?
    for (size_t i = log.records().size(); i-- > 0;) {
        CallRecord& r = const_cast<CallRecord&>(log.records()[i]);
        if (!r.active || r.tg != g.tg || r.group != g.group || r.dmr != g.dmr) continue;
        if (g.freqHz > 0 && r.freqHz > 0 && std::fabs(g.freqHz - r.freqHz) > 1 && g.slot == r.slot) {   // the call moved: the old one ends
            r.active = false;
            r.durSec = r.lastSec - r.startSec;
            for (Slot& s : slots_) if (s.used && s.callId == r.id) close(s, log, now, "moved");
            break;
        }
        r.lastSec = now;
        if (g.src >= 0 && r.src < 0) r.src = g.src;
        if (g.emergency) r.emergency = true;
        bool owned = false;
        for (Slot& s : slots_) if (s.used && s.callId == r.id) { owned = true; if (!s.leave && !s.remote) s.lastSec = now; }   // the voice radio's call lives on what it hears, not on grant repeats
        // a call that found every radio busy when it was granted and is still on the air: its repeat tries again (the call followed then may have
        // ended, and the radio should go to the call that is still running rather than wait for a new grant)
        if (!owned && r.missed && !r.followed && !r.encrypted) follow(g, d, now, log, r.id);
        return;
    }
    CallRecord rec;
    rec.wallTime = (int64_t)time(nullptr);
    rec.startSec = rec.lastSec = now;
    rec.active = true; rec.dmr = g.dmr; rec.group = g.group; rec.tg = g.tg; rec.src = g.src;
    rec.channel = g.channel; rec.freqHz = g.freqHz; rec.slot = g.slot; rec.emergency = g.emergency; rec.encrypted = g.encrypted;
    const uint64_t id = log.add(rec).id;
    log.trim(2000);
    follow(g, d, now, log, id);
}

// Give a logged call a slice, the voice radio or the hopped radio, or log why not. Also a repeat of a call that was busy when it was granted.
void Follower::follow(const Grant& g, const TalkgroupList::Decision& d, double now, CallLog& log, uint64_t id) {
    CallRecord* rp = log.byId(id);   // trim moves records: look it up again
    if (!rp) return;
    auto skip = [&](const std::string& why, bool missed) {
        rp->note = why;
        if (missed && !rp->missed) { rp->missed = true; missed_++; }   // a retry that is still busy is not missed twice
    };
    auto followed = [&]() {
        rp->followed = true;
        if (rp->missed) { rp->missed = false; if (missed_) missed_--; rp->note = "joined late: busy when it was granted"; }
    };
    if (g.encrypted) { skip("encrypted: not followed", false); return; }
    if (!d.follow) { skip(d.reason, false); return; }
    if (g.freqHz <= 0) { skip("channel frequency not known yet (no identifier update heard)", false); return; }
    const bool p2 = g.tdma && !g.dmr;
    const int p2Slot = p2 ? g.slot : -1;   // the Phase 2 time slot of the call (-1: Phase 1)
    if (p2 && !cfg_.followPhase2) { skip("P25 Phase 2 call: Phase 2 following is off", false); return; }
    const int prio = d.priority;
    auto lowest = [&](bool hoppedOnly, bool anyOnly) -> Slot* {
        Slot* v = nullptr;
        for (Slot& s : slots_) {
            if (!s.used) continue;
            if (hoppedOnly && !s.hopped) continue;
            if (!anyOnly && s.hopped) continue;
            if (!v || effPrio(s.prio) > effPrio(v->prio)) v = &s;
        }
        return v;
    };
    auto announce = [&](const std::string& text) {
        for (const auto& n : noticed_) if (std::fabs(n.first - g.freqHz) < 1 && now - n.second < 60) return;
        noticed_.push_back({g.freqHz, now});
        note(text);
    };
    // one Phase 2 call at a time inside the capture (the engine has one Phase 2 receiver to run beside the control channel): the running one
    // gives way to a higher priority call, else this call does not get in
    auto phase2Room = [&]() {
        for (Slot& x : slots_) {
            if (!x.used || x.hopped || !x.tdma) continue;
            if (effPrio(prio) >= effPrio(x.prio)) return false;
            close(x, log, now, "preempted by a higher priority call");
        }
        return true;
    };
    const bool captured = inSpan(g.freqHz);
    if (captured && (p2 || slices_)) {
        if (p2 && !phase2Room()) { skip("busy: one Phase 2 call at a time inside the capture", true); return; }
        Slot* s = freeSlot();
        if (!s) {
            Slot* v = lowest(false, false);
            if (v && effPrio(prio) < effPrio(v->prio)) { close(*v, log, now, "preempted by a higher priority call"); s = v; }
        }
        if (!s) { skip("busy: every slice is in use", true); return; }
        s->used = true; s->hopped = false; s->tdma = p2; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
        Action a;
        a.kind = Action::OpenSlice; a.callId = id; a.slice = (int)(s - slots_.data()); a.freqHz = g.freqHz;
        a.tdma = s->tdma; a.slot = p2Slot;
        out_.push_back(a);
        if (cfg_.record) {
            Action rs;
            rs.kind = Action::RecordStart; rs.callId = id; rs.slice = a.slice;
            char b[64];
            snprintf(b, sizeof b, "tg%d", g.tg);
            rs.text = b;
            out_.push_back(rs);
        }
        followed();
        if (state_ == FollowState::ControlHold) setState(FollowState::Following, now);
        return;
    }
    // outside the capture, a second radio takes the call and the control channel radio stays where it is
    if (!captured && cfg_.voiceRadio) {
        if (now < remoteBlockUntil_) { skip("voice radio: waiting after a call it did not hear", true); return; }
        Slot* busy = nullptr;
        for (Slot& x : slots_) if (x.used && x.remote) busy = &x;
        if (busy) {
            if (effPrio(prio) >= effPrio(busy->prio)) {
                skip("voice radio is on another call", true);
                announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is outside the capture while the voice radio has another call: needs a third radio or a wider capture");
                return;
            }
            close(*busy, log, now, "preempted by a higher priority call");
        }
        Slot* s = freeSlot();
        if (!s) {   // every slot is taken by calls inside the capture: take the lowest one's
            Slot* v = lowest(false, false);
            if (!v) { skip("busy: every slice is in use", true); return; }
            close(*v, log, now, "preempted by a higher priority call");
            s = v;
        }
        s->used = true; s->hopped = false; s->remote = true; s->voiced = false; s->tdma = p2; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
        Action a;
        a.kind = Action::OpenRemote; a.callId = id; a.slice = (int)(s - slots_.data()); a.freqHz = g.freqHz;
        a.tdma = p2; a.slot = p2Slot;
        out_.push_back(a);
        if (cfg_.record) {
            Action rs;
            rs.kind = Action::RecordStart; rs.callId = id; rs.slice = a.slice;
            char b[64];
            snprintf(b, sizeof b, "tg%d", g.tg);
            rs.text = b;
            out_.push_back(rs);
        }
        followed();
        if (state_ == FollowState::ControlHold) setState(FollowState::Following, now);
        return;
    }
    // outside the capture: the radio has to leave the control channel, which blinds it to every other grant
    if (!cfg_.allowRetune && captured) {   // a Phase 1 call inside the capture, but nothing decodes it there
        skip("inside the capture: turn on whole-span decoding (or let the radio retune)", true);
        announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is inside the capture: turn on whole-span decoding to follow it there");
        return;
    }
    if (!cfg_.allowRetune) {
        skip("needs a wider capture or a second radio", true);
        announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is outside the capture: needs a wider capture or a second radio");
        return;
    }
    if (state_ == FollowState::NoControl || state_ == FollowState::Searching) { skip("not on the control channel", true); return; }
    bool block = false;
    for (const Slot& s : slots_) if (s.used && effPrio(s.prio) <= effPrio(prio)) block = true;   // a call of equal or higher priority is running
    if (block) {
        skip("needs a wider capture or a second radio (a call is running)", true);
        announce("Call on " + std::to_string(g.freqHz / 1e6).substr(0, 9) + " MHz is outside the capture while another call is followed: needs a wider capture or a second radio");
        return;
    }
    for (Slot& s : slots_) if (s.used) close(s, log, now, "preempted by a higher priority call");
    if (homeHz_ <= 0 && ccHz_ <= 0) homeHz_ = centerHz_;
    Slot* s = freeSlot();
    if (!s) return;
    s->used = true; s->hopped = true; s->tdma = p2; s->callId = id; s->prio = prio; s->freqHz = g.freqHz; s->startSec = s->lastSec = now;
    Action a;
    a.kind = Action::Retune; a.callId = id; a.freqHz = g.freqHz;
    a.tdma = s->tdma; a.slot = p2Slot;
    out_.push_back(a);
    radioHz_ = g.freqHz;
    if (cfg_.record) {
        Action rs;
        rs.kind = Action::RecordStart; rs.callId = id; rs.slice = (int)(s - slots_.data());
        char b[64];
        snprintf(b, sizeof b, "tg%d", g.tg);
        rs.text = b;
        out_.push_back(rs);
    }
    followed();
    setState(FollowState::Hopped, now);
}

void Follower::onVoice(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId && !s.leave) { s.lastSec = now; s.voiced = true; }
}

void Follower::onTerminator(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId) { s.lastSec = now - cfg_.hangSec - 0.001; s.voiced = true; }   // ends at the next tick (a terminator is heard: the radio works)
}

void Follower::tick(double now, CallLog& log) {
    lastNow_ = now;
    bool hoppedEnded = false;
    for (Slot& s : slots_) {
        if (!s.used) continue;
        // a voice radio call not heard yet is given time to retune and sync; after that the hang time counts from the last voice
        const double limit = s.remote && !s.voiced && !s.leave ? std::max(cfg_.hangSec, cfg_.remoteStartSec) : cfg_.hangSec;
        if (now - s.lastSec > limit) {
            if (s.hopped) hoppedEnded = true;
            close(s, log, now, s.remote && !s.voiced ? "voice radio heard nothing on the channel" : "");
        }
    }
    // calls nobody follows end by silence on the control channel too
    for (const CallRecord& cr : log.records()) {
        if (!cr.active) continue;
        bool owned = false;
        for (const Slot& s : slots_) if (s.used && s.callId == cr.id) owned = true;
        if (!owned && now - cr.lastSec > cfg_.hangSec) {
            CallRecord* r = log.byId(cr.id);
            if (r) { r->active = false; r->durSec = r->lastSec - r->startSec; }
        }
    }
    if (hoppedEnded) { goHome(now); return; }
    switch (state_) {
    case FollowState::Following:
        if (activeSlices() == 0) setState(FollowState::ControlHold, now);
        break;
    case FollowState::ControlHold:
        if (activeSlices() > 0) { setState(FollowState::Following, now); break; }
        if (cfg_.allowRetune && now - lastCcSec_ > cfg_.ccLossSec) {
            const std::vector<double> l = searchList();
            if (l.empty()) break;
            candIdx_ = 0;
            const double cur = tunedHz();
            const bool here = cur > 0 && std::fabs(cur - l[0]) < 1;
            searchAt(l[0], now);
            // already on it and silent for ccLossSec: it has had its wait, the next step comes after one more loss time, not a whole dwell
            if (here && l.size() > 1) dwellStart_ = now - std::max(0.0, cfg_.ccDwellSec - cfg_.ccLossSec);
            setState(FollowState::Searching, now);
            note("Control channel lost: looking for it");
        }
        break;
    case FollowState::Returning:
        if (now - stateSince_ > cfg_.returnTimeoutSec) {
            const std::vector<double> l = searchList();
            if (cfg_.allowRetune && !l.empty()) {
                candIdx_ = 0;
                searchAt(l[0], now);
                setState(FollowState::Searching, now);
            } else setState(FollowState::NoControl, now);
        }
        break;
    case FollowState::Searching:
        if (now - dwellStart_ > cfg_.ccDwellSec) {
            const std::vector<double> l = searchList();
            if (l.empty()) break;
            candIdx_ = (candIdx_ + 1) % l.size();
            searchAt(l[candIdx_], now);
        }
        break;
    default: break;
    }
}

std::vector<Action> Follower::takeActions() {
    std::vector<Action> o;
    o.swap(out_);
    return o;
}

} // namespace trunk
} // namespace dect2
