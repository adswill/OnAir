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
    for (const Slot& s : slots_) if (s.used && !s.hopped) n++;
    return n;
}

std::vector<double> Follower::sliceFreqs(bool tdma) const {
    std::vector<double> v;
    for (const Slot& s : slots_) if (s.used && !s.hopped && s.tdma == tdma) v.push_back(s.freqHz);
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
    a.kind = s.hopped ? Action::Notice : Action::CloseSlice;
    a.tdma = s.tdma;
    if (!s.hopped) out_.push_back(a);
    if (CallRecord* r = log.byId(s.callId)) {
        if (!s.leave) {
            r->active = false;
            r->durSec = std::max(0.0, std::max(s.lastSec, r->lastSec) - r->startSec);
        }
        if (why && *why && r->note.empty()) r->note = why;
    }
    (void)now;
    s.used = false; s.leave = false;
}

void Follower::leave(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_)
        if (s.used && s.callId == callId) { s.leave = true; s.lastSec = now - cfg_.hangSec - 0.001; }   // closed at the next tick, the call stays in the log
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
        for (Slot& s : slots_) if (s.used && s.callId == r.id && !s.leave) s.lastSec = now;
        return;
    }
    CallRecord rec;
    rec.wallTime = (int64_t)time(nullptr);
    rec.startSec = rec.lastSec = now;
    rec.active = true; rec.dmr = g.dmr; rec.group = g.group; rec.tg = g.tg; rec.src = g.src;
    rec.channel = g.channel; rec.freqHz = g.freqHz; rec.slot = g.slot; rec.emergency = g.emergency; rec.encrypted = g.encrypted;
    const uint64_t id = log.add(rec).id;
    log.trim(2000);
    CallRecord* rp = log.byId(id);   // trim moves records: look it up again
    if (!rp) return;
    auto skip = [&](const std::string& why, bool missed) {
        rp->note = why;
        if (missed) { rp->missed = true; missed_++; }
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
        rp->followed = true;
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
    rp->followed = true;
    setState(FollowState::Hopped, now);
}

void Follower::onVoice(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId && !s.leave) s.lastSec = now;
}

void Follower::onTerminator(uint64_t callId, double now) {
    lastNow_ = now;
    for (Slot& s : slots_) if (s.used && s.callId == callId) s.lastSec = now - cfg_.hangSec - 0.001;   // ends at the next tick
}

void Follower::tick(double now, CallLog& log) {
    lastNow_ = now;
    bool hoppedEnded = false;
    for (Slot& s : slots_) {
        if (!s.used) continue;
        if (now - s.lastSec > cfg_.hangSec) {
            if (s.hopped) hoppedEnded = true;
            close(s, log, now, "");
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
        if (cfg_.allowRetune && !cands_.empty() && now - lastCcSec_ > cfg_.ccLossSec) {
            candIdx_ = 0;
            Action a;
            a.kind = Action::Retune; a.freqHz = cands_[0];
            out_.push_back(a);
            radioHz_ = cands_[0];
            dwellStart_ = now;
            setState(FollowState::Searching, now);
            note("Control channel lost: looking for it");
        }
        break;
    case FollowState::Returning:
        if (now - stateSince_ > cfg_.returnTimeoutSec) {
            if (cfg_.allowRetune && !cands_.empty()) {
                candIdx_ = 0;
                Action a;
                a.kind = Action::Retune; a.freqHz = cands_[0];
                out_.push_back(a);
                radioHz_ = cands_[0];
                dwellStart_ = now;
                setState(FollowState::Searching, now);
            } else setState(FollowState::NoControl, now);
        }
        break;
    case FollowState::Searching:
        if (!cands_.empty() && now - dwellStart_ > cfg_.ccDwellSec) {
            candIdx_ = (candIdx_ + 1) % cands_.size();
            Action a;
            a.kind = Action::Retune; a.freqHz = cands_[candIdx_];
            out_.push_back(a);
            radioHz_ = cands_[candIdx_];
            dwellStart_ = now;
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
