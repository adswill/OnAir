// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Trunking controller: control channel messages in, calls and follow actions out. Also the DMR Tier III grant model.
#include "dect2/p25/trunk.h"
#include "dect2/dmr/dmr_vendor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace trunk {

void DmrModel::setChannel(int lcn, double hz) { if (hz > 0) lcn_[lcn] = hz; else lcn_.erase(lcn); }
double DmrModel::lcnFreq(int lcn) const { auto it = lcn_.find(lcn); return it == lcn_.end() ? 0 : it->second; }

bool DmrModel::onCsbk(int opcode, int fid, const uint8_t d[8], double, Grant& g) {
    if (fid != 0) return false;   // vendor CSBKs go through onCsbkVendor
    if (opcode != ops_.tvGrant && opcode != ops_.pvGrant && opcode != ops_.btvGrant) return false;
    g = Grant();
    g.dmr = true; g.tdma = true;
    g.channel = ((d[0] & 0x0F) << 8) | d[1];
    g.slot = (d[0] >> 4) & 1;
    g.emergency = (d[0] >> 5) & 1;
    g.encrypted = (d[0] >> 6) & 1;   // the privacy indicator
    g.tg = (d[2] << 16) | (d[3] << 8) | d[4];
    g.src = (d[5] << 16) | (d[6] << 8) | d[7];
    g.group = opcode != ops_.pvGrant;
    g.freqHz = lcnFreq(g.channel);
    n_++;
    return true;
}

// Capacity Plus (FID 0x10) and Connect Plus (FID 0x06). LSNs and LCNs are looked up in the user's channel table as for Tier III.
bool DmrModel::onCsbkVendor(int opcode, int fid, const uint8_t d[8], std::vector<Grant>& out) {
    out.clear();
    if (fid == 0x10 && opcode == 0x3E) {
        std::vector<dmr::CapPlusGrant> gs; int rest = 0;
        if (!capPlus_.feed(d, gs, rest)) return false;
        for (const auto& c : gs) {
            Grant g; g.dmr = true; g.tdma = true; g.channel = c.lsn; g.slot = (c.lsn - 1) & 1; g.group = true; g.tg = c.tg; g.freqHz = lcnFreq(c.lsn); g.explicitUpdate = true;
            n_++; out.push_back(g);
        }
        return !out.empty();
    }
    if (fid == 0x06 && opcode == 0x03) {
        dmr::ConPlus c;
        if (!dmr::conPlusParse(opcode, d, c)) return false;
        Grant g; g.dmr = true; g.tdma = true; g.channel = c.lcn; g.slot = c.slot; g.group = true; g.tg = (int)c.dst; g.src = (int)c.src; g.freqHz = lcnFreq(c.lcn);
        n_++; out.push_back(g);
        return true;
    }
    return false;
}

Controller::Controller() {}

void Controller::pushMsg(const std::string& s) {
    if (s.empty()) return;
    messages.push_back(s);
    if (messages.size() > 200) messages.erase(messages.begin(), messages.end() - 200);
}

void Controller::dispatch(const std::vector<Grant>& gs, double now) {
    for (const Grant& g : gs) {
        TalkgroupList::Decision d = g.group ? tgs.decide(g.tg) : TalkgroupList::Decision();
        Grant gg = g;
        // a talkgroup the user marked as encrypted in the list is treated as encrypted even if the grant does not say so
        if (const TgEntry* e = g.group ? tgs.find(g.tg) : nullptr) if (e->encryptedMode()) gg.encrypted = true;
        // Phase 2 is scrambled with the site's WACN / system ID / NAC: without the system broadcast nothing would be heard, so it is not followed yet
        if (gg.tdma && !gg.dmr && d.follow && (sys.site().wacn < 0 || sys.site().sysId < 0)) { d.follow = false; d.retry = true; d.reason = "Phase 2 call: waiting for the system broadcast (WACN / system ID) to descramble it"; }
        if (ccNow() > 0) health.onGrant(ccNow(), now);
        if (grantTap) grantTap(gg, now);
        follower.onGrant(gg, d, now, log);
    }
}

void Controller::onTsbk(const p25::Tsbk& t, int nac, double now) {
    good_++;
    if (ccNow() > 0) health.onGood(ccNow(), now);
    lastGood_ = now;
    follower.onControlSeen(now, nac);
    std::vector<Grant> gs;
    pushMsg(sys.onTsbk(t, nac, now, &gs));
    if (sys.site().valid && sys.site().ccHz > 0) follower.setControlChannel(sys.site().ccHz, sys.ownControlChannels());
    if (sys.site().valid) {   // the site book: this site and the neighbours it announces, learned over a whole drive
        health.learnSite(sys.site().sysId, sys.site().rfss, sys.site().site, ccNow() > 0 ? ccNow() : sys.site().ccHz, now);
        for (const SiteInfo& a : sys.adjacent()) health.learnSite(a.sysId, a.rfss, a.site, a.ccHz, now);
    }
    dispatch(gs, now);
}

void Controller::onTsbkBad(double now) { bad_++; if (ccNow() > 0) health.onBad(ccNow(), now); }

void Controller::onDmrCsbk(int opcode, int fid, const uint8_t data[8], double now) {
    Grant g;
    good_++;
    lastGood_ = now;
    follower.onControlSeen(now);
    if (dmr.onCsbk(opcode, fid, data, now, g)) dispatch({g}, now);
    else { std::vector<Grant> vg; if (dmr.onCsbkVendor(opcode, fid, data, vg)) dispatch(vg, now); }
}

void Controller::tick(double now) {
    follower.tick(now, log);
    if (autoPark && follower.state() == FollowState::ControlHold && follower.activeSlices() == 0 && !follower.remoteActive() && ccNow() > 0) {
        const double hz = health.advise(ccNow(), now);
        if (hz > 0 && follower.parkAt(hz, now)) {
            health.moved(now);
            char b[120];
            snprintf(b, sizeof b, "Control channel moved to %.4f MHz (better decode quality)", hz / 1e6);
            pushMsg(b);
        }
    }
}

ControllerStatus Controller::status(double now) const {
    ControllerStatus s;
    s.state = follower.state();
    s.tsbkGood = good_; s.tsbkBad = bad_;
    s.crcOkPct = good_ + bad_ ? 100.0 * (double)good_ / (double)(good_ + bad_) : 0;
    s.controlLocked = now - lastGood_ < follower.config().ccLossSec;
    for (const CallRecord& r : log.records()) if (r.active) s.activeCalls++;
    s.missed = follower.missedCalls();
    return s;
}

std::string Controller::statusLine(double now) const {
    char b[200];
    if (!status(now).controlLocked && follower.state() != FollowState::Hopped && follower.state() != FollowState::Following) return "Waiting for the control channel";
    const CallRecord* best = nullptr;
    const uint64_t aud = follower.audibleCall();
    for (const CallRecord& r : log.records()) if (r.active && r.id == aud) best = &r;
    if (best && best->followed) {
        if (best->slot >= 0) snprintf(b, sizeof b, "Following TG %d on %.4f MHz, slot %d", best->tg, best->freqHz / 1e6, best->slot + 1);
        else snprintf(b, sizeof b, "Following TG %d on %.4f MHz", best->tg, best->freqHz / 1e6);
        return b;
    }
    const CallRecord* idle = nullptr;
    for (const CallRecord& r : log.records()) if (r.active) idle = &r;   // newest active call: why it is not followed
    if (idle && !idle->note.empty()) {
        snprintf(b, sizeof b, "Not following TG %d: ", idle->tg);
        return std::string(b) + idle->note;
    }
    if (follower.state() == FollowState::Returning) return "Returning to the control channel";
    if (follower.state() == FollowState::Searching) return "Looking for the control channel";
    return "Listening to the control channel: no calls";
}

std::vector<const CallRecord*> Controller::nowTalking() const {
    std::vector<const CallRecord*> v;
    for (const CallRecord& r : log.records()) if (r.active) v.push_back(&r);
    return v;
}

std::vector<Controller::TalkgroupRow> Controller::conversations() const {
    std::vector<TalkgroupRow> rows;
    const auto& v = log.records();
    for (size_t i = v.size(); i-- > 0;) {   // newest first
        const CallRecord& r = v[i];
        if (!r.group || r.tg < 0) continue;
        auto it = std::find_if(rows.begin(), rows.end(), [&](const TalkgroupRow& t) { return t.tg == r.tg; });
        if (it == rows.end()) {
            rows.emplace_back();
            it = rows.end() - 1;
            it->tg = r.tg; it->lastSec = r.lastSec; it->lastSrc = r.src; it->lastNote = r.note;
        }
        it->calls++;
        it->followed += r.followed;
        it->encrypted += r.encrypted;
        it->missed += r.missed;
        it->active = it->active || r.active;
        it->emergency = it->emergency || r.emergency;
        it->airSec += r.active ? r.lastSec - r.startSec : r.durSec;
    }
    return rows;
}

void Controller::clear() {
    sys.clear();
    log.clear();
    follower.reset();
    messages.clear();
    good_ = bad_ = 0;
    lastGood_ = -1e9;
}

} // namespace trunk
} // namespace dect2
