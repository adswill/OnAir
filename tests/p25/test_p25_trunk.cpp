// Trunking logic: the system model from TSBKs, the follow state machine (grant -> follow -> end -> return), priority and lockout, the rotating
// control channel, the talkgroup CSV, encrypted grants, and malformed / random inputs. No RF: the messages go through tsbkBits / tsbkParse first.
#include "dect2/p25/trunk.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

static p25::Tsbk wire(const p25::Tsbk& t) {   // through the bit layout and the CRC like a received one
    uint8_t b[96];
    p25::tsbkBits(t, b);
    p25::Tsbk r;
    if (!p25::tsbkParse(b, r)) { printf("FAIL: tsbk did not survive its CRC\n"); fails++; }
    return r;
}

static void feed(Controller& c, const p25::Tsbk& t, double now, int nac = 0x293) { c.onTsbk(wire(t), nac, now); }

static int count(const std::vector<Action>& a, Action::Kind k) { int n = 0; for (const auto& x : a) n += x.kind == k; return n; }

// a site with identifier 1 at 851.00625 MHz / 12.5 kHz: channel 0x1010 = 851.20625 MHz
static void setupSite(Controller& c, double now) {
    feed(c, gen::idenUp(1, 851006250, 12500, -45000000, 12500), now);
    feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), now);
    feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1001, 0x70), now);
}

static double chHz(int ch) { return 851006250 + (ch & 0xFFF) * 12500.0; }

// A fake second receiver: does what the host does with OpenRemote / CloseRemote, and lets a test make it fail
struct FakeVoiceRadio {
    bool present = true, failRetune = false;
    bool open = false, tdma = false; int slot = -1; double hz = 0; uint64_t call = 0;
    int opens = 0, closes = 0;
    bool voice = true;           // the radio hears the call once tuned
    void apply(Controller& c, double now) {
        for (const Action& a : c.takeActions()) {
            if (a.kind == Action::OpenRemote) {
                if (!present) { c.onRemoteFailed(a.callId, "second radio is gone", now); continue; }
                if (failRetune) { c.onRemoteFailed(a.callId, "retune failed", now); continue; }
                open = true; opens++; tdma = a.tdma; slot = a.slot; hz = a.freqHz; call = a.callId;
            } else if (a.kind == Action::CloseRemote) { open = false; closes++; if (call == a.callId) call = 0; }
        }
    }
};

int main() {
    // ---- system model
    {
        Controller c;
        setupSite(c, 1);
        feed(c, gen::adjSts(0x3A1, 1, 6, 0x1009, 0x70), 1);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 1);
        const SiteInfo& s = c.sys.site();
        CHECK(s.valid && s.wacn == 0xBEE00 && s.sysId == 0x3A1 && s.rfss == 1 && s.site == 5 && s.nac == 0x293, "site %05X %03X %d %d", s.wacn, s.sysId, s.rfss, s.site);
        CHECK(std::fabs(s.ccHz - chHz(0x1001)) < 1, "cc %f", s.ccHz);
        CHECK(c.sys.adjacent().size() == 1 && std::fabs(c.sys.adjacent()[0].ccHz - chHz(0x1009)) < 1, "adjacent");
        const auto cand = c.sys.controlCandidates();
        CHECK(cand.size() == 4 && std::fabs(cand[0] - chHz(0x1001)) < 1, "%zu candidates", cand.size());
        CHECK(c.sys.iden(1).valid && std::fabs(c.sys.iden(1).spacingHz - 12500) < 1 && std::fabs(c.sys.iden(1).txOffsetHz + 45e6) < 1, "iden");
        // VU identifier: 6.25 kHz, +5 MHz
        feed(c, gen::idenUpVu(2, 460000000, 6250, 5000000, 4), 1);
        CHECK(c.sys.iden(2).vu && std::fabs(c.sys.iden(2).spacingHz - 6250) < 1 && std::fabs(c.sys.iden(2).txOffsetHz - 5e6) < 1 && std::fabs(c.sys.iden(2).bwHz - 6250) < 1, "vu iden");
        CHECK(std::fabs(c.sys.channelFreq(0x2004) - 460025000) < 1, "vu channel %f", c.sys.channelFreq(0x2004));
        CHECK(c.sys.channelFreq(0x5001) == 0, "unknown identifier gives 0");
        // TDMA identifier, two slots: channel 5 is carrier 2 slot 1
        feed(c, gen::idenUpTdma(3, 3, 770000000, 12500, -30000000), 1);
        int slot = -2; bool tdma = false;
        const double f = c.sys.channelFreq(0x3005, &slot, &tdma);
        CHECK(tdma && slot == 1 && std::fabs(f - (770000000 + 2 * 12500.0)) < 1, "tdma %f slot %d", f, slot);
        feed(c, gen::idenUpTdma(4, 9, 770000000, 12500, 0), 1);
        CHECK(c.sys.channelFreq(0x4001) == 0, "unknown channel type is not placed");
    }
    // ---- grants, calls, the follow cycle inside a wide capture
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);   // covers the whole 851 MHz block
        FollowConfig fc; fc.maxSlices = 2; fc.hangSec = 2;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x04, 0x1010, 1201, 555), 1.0);
        auto a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 1 && count(a, Action::RecordStart) == 1, "open + record: %zu actions", a.size());
        CHECK(a.size() >= 1 && std::fabs(a[0].freqHz - chHz(0x1010)) < 1, "slice frequency");
        CHECK(c.follower.state() == FollowState::Following, "state %s", followStateName(c.follower.state()));
        CHECK(c.nowTalking().size() == 1 && c.nowTalking()[0]->tg == 1201 && c.nowTalking()[0]->src == 555, "now talking");
        feed(c, gen::grpVGrantUpd(0x1010, 1201, 0x1010, 1201), 2.0);   // a repeat keeps it going
        feed(c, gen::grpVGrant(0x04, 0x1010, 1201, 555), 2.5);
        CHECK(c.takeActions().empty(), "repeats do not open another slice");
        c.tick(3.0);
        CHECK(c.nowTalking().size() == 1, "still going 0.5 s after the last repeat");
        c.tick(5.0);
        a = c.takeActions();
        CHECK(count(a, Action::CloseSlice) == 1 && count(a, Action::RecordStop) == 1, "close + stop record");
        CHECK(c.nowTalking().empty() && c.follower.state() == FollowState::ControlHold, "ended, back to hold (%s)", followStateName(c.follower.state()));
        CHECK(c.log.records().size() == 1 && std::fabs(c.log.records()[0].durSec - 1.5) < 1e-6 && c.log.records()[0].followed && std::fabs(c.log.records()[0].freqHz - chHz(0x1010)) < 1, "log: dur %.2f", c.log.records()[0].durSec);
        // a terminator ends a call at the next tick
        feed(c, gen::grpVGrant(0x04, 0x1011, 1202, 556), 10.0);
        c.takeActions();
        const uint64_t id = c.log.records().back().id;
        c.onVoice(id, 10.5);
        c.onTerminator(id, 10.6);
        c.tick(10.7);
        CHECK(count(c.takeActions(), Action::CloseSlice) == 1 && !c.log.records().back().active, "terminator ends the call");
        // unit to unit
        feed(c, gen::uuVGrant(0x1012, 777, 888), 20.0);
        const CallRecord& u = c.log.records().back();
        CHECK(!u.group && u.tg == 777 && u.src == 888 && u.followed, "unit to unit call");
        c.tick(30.0);
    }
    // ---- slices, priority, lockout, scan lists
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.maxSlices = 2;
        c.follower.setConfig(fc);
        c.tgs.upsert(2001).priority = 5;
        c.tgs.upsert(2002).priority = 3;
        c.tgs.upsert(2003).priority = 1;
        c.tgs.upsert(2004).lockout = true;
        feed(c, gen::grpVGrant(4, 0x1010, 2001, 1), 1);
        feed(c, gen::grpVGrant(4, 0x1011, 2002, 2), 1.1);
        c.takeActions();
        CHECK(c.follower.activeSlices() == 2, "two slices");
        feed(c, gen::grpVGrant(4, 0x1012, 2009, 3), 1.2);   // no priority: busy, not followed
        CHECK(c.takeActions().empty() && c.log.records().back().missed && c.follower.missedCalls() == 1, "third call is busy");
        feed(c, gen::grpVGrant(4, 0x1013, 2003, 4), 1.3);   // highest priority preempts the lowest (2001)
        auto a = c.takeActions();
        CHECK(count(a, Action::CloseSlice) == 1 && count(a, Action::OpenSlice) == 1, "preempt: %zu actions", a.size());
        CHECK(c.follower.audibleCall() == c.log.records().back().id, "the priority call is the audible one");
        bool lowGone = false;
        for (const auto& r : c.log.records()) if (r.tg == 2001 && !r.active) lowGone = true;
        CHECK(lowGone, "the preempted call ended");
        feed(c, gen::grpVGrant(4, 0x1014, 2004, 5), 1.4);
        CHECK(c.takeActions().empty() && c.log.records().back().note == "locked out", "lockout: '%s'", c.log.records().back().note.c_str());
        // scan-only: only listed talkgroups with listen on
        Controller d;
        setupSite(d, 0);
        d.setCapture(851.1e6, 2.4e6);
        d.tgs.upsert(10).list = "Fire";
        d.tgs.upsert(11).list = "Police";
        d.tgs.setScanOnly(true);
        d.tgs.enableList("Fire", true);
        feed(d, gen::grpVGrant(4, 0x1010, 10, 1), 1);
        feed(d, gen::grpVGrant(4, 0x1011, 11, 1), 1);
        feed(d, gen::grpVGrant(4, 0x1012, 12, 1), 1);
        CHECK(d.follower.activeSlices() == 1 && d.log.records().size() == 3, "scan list: %d slices", d.follower.activeSlices());
        CHECK(d.log.records()[1].note == "its list is off" && d.log.records()[2].note == "not in the scan list", "notes '%s' '%s'", d.log.records()[1].note.c_str(), d.log.records()[2].note.c_str());
    }
    // ---- encrypted: logged, never followed, never recorded
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::grpVGrant(0x40, 0x1010, 3001, 9), 1);   // service options bit 0x40
        c.tgs.upsert(3002).mode = "DE";
        feed(c, gen::grpVGrant(0, 0x1011, 3002, 9), 1);       // the list says encrypted
        CHECK(c.takeActions().empty() && c.follower.activeSlices() == 0, "no actions for encrypted calls");
        const auto& r = c.log.records();
        CHECK(r.size() == 2 && r[0].encrypted && r[1].encrypted && !r[0].followed && !r[1].followed, "logged as encrypted");
        CHECK(c.log.exportCsv().find("encrypted") != std::string::npos, "csv says encrypted");
    }
    // ---- outside the capture: hop with one radio, return; or say it needs more
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);   // only the control channel
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        FollowConfig fc; fc.hangSec = 1.5; fc.allowRetune = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(4, 0x5010, 4001, 1), 1.0);
        auto a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && a[0].kind == Action::Retune && std::fabs(a[0].freqHz - (856000000 + 16 * 12500.0)) < 1, "retune to the voice channel");
        CHECK(c.follower.state() == FollowState::Hopped, "hopped");
        c.onVoice(c.log.records().back().id, 2.0);
        c.tick(3.0);
        CHECK(c.follower.state() == FollowState::Hopped, "voice keeps it there");
        c.tick(4.0);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a.back().freqHz - chHz(0x1001)) < 1 && c.follower.state() == FollowState::Returning, "back to the control channel (%s)", followStateName(c.follower.state()));
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 4.5);
        CHECK(c.follower.state() == FollowState::ControlHold, "control channel is back");
        // a radio that may not retune: says what is needed
        Controller d;
        setupSite(d, 0);
        d.setCapture(chHz(0x1001), 0);
        feed(d, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        FollowConfig f2; f2.allowRetune = false;
        d.follower.setConfig(f2);
        feed(d, gen::grpVGrant(4, 0x5010, 4001, 1), 1.0);
        a = d.takeActions();
        bool said = false;
        for (const auto& x : a) if (x.kind == Action::Notice && x.text.find("needs a wider capture or a second radio") != std::string::npos) said = true;
        CHECK(said && d.log.records().back().missed && d.log.records().back().note.find("second radio") != std::string::npos, "needs a wider capture or a second radio");
        // while one call is followed inside the capture a second one outside does not take the radio away
        Controller e;
        setupSite(e, 0);
        FollowConfig fe; fe.allowRetune = true;
        e.follower.setConfig(fe);
        e.setCapture(851.1e6, 1.0e6);
        feed(e, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        feed(e, gen::grpVGrant(4, 0x1010, 5001, 1), 1);
        e.takeActions();
        feed(e, gen::grpVGrant(4, 0x5010, 5002, 1), 1.1);
        a = e.takeActions();
        CHECK(count(a, Action::Retune) == 0 && e.log.records().back().missed, "no hop away from a running call");
    }
    // ---- identifier table across a change of site, and FDMA channel types of an IDEN_UP_TDMA
    {
        Controller c;
        setupSite(c, 1);                       // site 5 of system 3A1, identifier 1 at 851.00625 MHz
        feed(c, gen::idenUp(2, 769000000, 12500, 0, 12500), 1);
        feed(c, gen::adjSts(0x3A1, 1, 6, 0x1009, 0x70), 1);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 1);
        CHECK(c.sys.channelFreq(0x1010) > 0 && c.sys.adjacent().size() == 1, "site A known");
        // ten seconds later the radio is on another system's control channel: its site id arrives before its identifier updates
        feed(c, gen::netSts(0xBEE00, 0x2D1, 0x2001, 0x70), 11);
        feed(c, gen::rfssSts(0x2D1, 1, 3, 0x2001, 0x70), 11);
        CHECK(!c.sys.iden(1).valid && !c.sys.iden(2).valid && c.sys.adjacent().empty(), "the old site's identifiers and neighbours are dropped");
        CHECK(c.sys.channelFreq(0x1010) == 0, "no frequency from the old site's table (%f)", c.sys.channelFreq(0x1010));
        feed(c, gen::idenUp(1, 770000000, 12500, 0, 12500), 11.5);
        CHECK(std::fabs(c.sys.channelFreq(0x1010) - (770000000 + 16 * 12500.0)) < 1, "the new table places the channel");
        // an identifier announced a moment before the site id (the same cycle) survives it
        Controller d;
        feed(d, gen::idenUp(1, 770000000, 12500, 0, 12500), 20);
        feed(d, gen::netSts(0xBEE00, 0x2D1, 0x2001, 0x70), 20.5);
        feed(d, gen::rfssSts(0x2D1, 1, 3, 0x2001, 0x70), 20.6);
        feed(d, gen::rfssSts(0x2D1, 1, 4, 0x2001, 0x70), 21);   // a site change within a second: the table row is current
        CHECK(d.sys.iden(1).valid, "a fresh identifier row stays");
        // channel types 0..2 of an IDEN_UP_TDMA are plain carriers, 3 is two slots, the others stay unplaced
        feed(d, gen::idenUpTdma(5, 0, 770000000, 12500, 0), 21);
        int slot = -2; bool tdma = true;
        const double f = d.sys.channelFreq(0x5004, &slot, &tdma);
        CHECK(!tdma && slot == -1 && std::fabs(f - (770000000 + 4 * 12500.0)) < 1, "FDMA channel type: %f slot %d tdma %d", f, slot, (int)tdma);
    }
    // ---- rotating control channel
    {
        Controller c;
        setupSite(c, 0);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.ccLossSec = 2; fc.ccDwellSec = 1; fc.allowRetune = true;
        c.follower.setConfig(fc);
        c.tick(1.0);
        CHECK(c.follower.state() == FollowState::ControlHold, "holding");
        c.tick(3.0);
        auto a = c.takeActions();
        CHECK(c.follower.state() == FollowState::Searching && count(a, Action::Retune) == 0, "control lost: searching, the radio is on candidate 1 already (no retune)");
        c.tick(4.5);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a[0].freqHz - chHz(0x1020)) < 1, "next candidate");
        feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1020, 0x70), 5.0);   // heard it
        CHECK(c.follower.state() == FollowState::ControlHold, "found, holding");
    }
    // ---- issue #348: the radio hops to a call and comes back to a receiver that needs seconds to lock again. Model: a retune costs 4 s of acquisition,
    //      the site's control channels lock, nothing else does. The follower must get its control channel back without a search tour.
    {
        const double base = 770031250;
        auto hz = [&](int ch) { return base + ch * 12500.0; };
        Controller c;
        feed(c, gen::idenUp(1, base, 12500, 0, 12500), 0);
        feed(c, gen::netSts(0xBEE00, 0x1A7, 0x1019, 0x70), 0);
        feed(c, gen::rfssSts(0x1A7, 2, 14, 0x1019, 0x70), 0);
        feed(c, gen::sccb(2, 14, 0x10DA, 0x114C), 0);
        feed(c, gen::adjSts(0x1A7, 2, 15, 0x1BB8, 0x70), 0);   // a neighbour whose channel this site's table would place far outside the band
        const double ccHz = hz(0x19);
        CHECK(std::fabs(c.sys.site().ccHz - ccHz) < 1, "site cc %f", c.sys.site().ccHz);
        const auto own = c.sys.ownControlChannels();
        CHECK(own.size() == 3 && std::fabs(own[0] - ccHz) < 1 && std::fabs(own[1] - hz(0xDA)) < 1 && std::fabs(own[2] - hz(0x14C)) < 1, "own control channels: %zu", own.size());
        CHECK(c.sys.controlCandidates().size() == 4, "controlCandidates() keeps the adjacent site for its other users");
        c.setCapture(ccHz, 0);
        FollowConfig fc; fc.allowRetune = true;   // default timers
        c.follower.setConfig(fc);
        double radio = ccHz, lockAt = 0;
        int searchRetunes = 0;
        bool visitedOutside = false, back = false;
        auto isOwn = [&](double f) { for (double o : own) if (std::fabs(o - f) < 1) return true; return false; };
        for (double t = 0.2; t < 60; t += 0.1) {
            const bool lockedNow = isOwn(radio) && t >= lockAt;
            if (lockedNow && std::fmod(t, 0.2) < 0.1) feed(c, gen::rfssSts(0x1A7, 2, 14, 0x1019, 0x70), t);
            if (lockedNow && t > 1 && t < 1.5) feed(c, gen::grpVGrant(4, 0x1000 | 89, 5001, 77), t);   // 771.14375 MHz: outside the capture
            c.tick(t);
            for (const Action& a : c.takeActions()) {
                if (a.kind != Action::Retune) continue;
                const bool hop = a.callId != 0;
                if (!hop && std::fabs(a.freqHz - ccHz) > 1) searchRetunes++;
                if (!isOwn(a.freqHz) && !hop) visitedOutside = true;
                radio = a.freqHz; lockAt = t + 4.0;
            }
            if (t > 20 && c.follower.state() == FollowState::ControlHold && std::fabs(radio - ccHz) < 1) back = true;
        }
        CHECK(back, "back on the control channel after the call (state %s)", followStateName(c.follower.state()));
        CHECK(searchRetunes == 0 && !visitedOutside, "no search tour: %d retunes, outside %d", searchRetunes, (int)visitedOutside);
        CHECK(c.follower.state() == FollowState::ControlHold, "still holding at the end (%s)", followStateName(c.follower.state()));
    }
    // ---- issue #348, control channel drop and re-acquire. Model: a retune costs 4 s of acquisition; only the frequency the system uses as its control
    //      channel at that moment locks. The control channel drops for 40 s (same frequency), then the site moves it to its secondary channel.
    {
        const double base = 770031250;
        auto hz = [&](int ch) { return base + ch * 12500.0; };
        Controller c;
        feed(c, gen::idenUp(1, base, 12500, 0, 12500), 0);
        feed(c, gen::netSts(0xBEE00, 0x1A7, 0x1019, 0x70), 0);
        feed(c, gen::rfssSts(0x1A7, 2, 14, 0x1019, 0x70), 0);
        feed(c, gen::sccb(2, 14, 0x10DA, 0x114C), 0);
        feed(c, gen::adjSts(0x1A7, 2, 15, 0x1BB8, 0x70), 0);
        const double cc1 = hz(0x19), cc2 = hz(0xDA);
        c.setCapture(cc1, 0);
        FollowConfig fc; fc.allowRetune = true;
        c.follower.setConfig(fc);
        double radio = cc1, lockAt = 0, dropStart = 20, dropEnd = 60, movedAt = 80;
        int retunes = 0, outside = 0;
        double relockSec = -1, movedLockSec = -1;
        for (double t = 0.2; t < 160; t += 0.1) {
            const double live = t < movedAt ? cc1 : cc2;   // the control channel in use
            const bool up = std::fabs(radio - live) < 1 && t >= lockAt && !(t > dropStart && t < dropEnd);
            if (up && std::fmod(t, 0.2) < 0.1) {
                feed(c, gen::rfssSts(0x1A7, 2, 14, t < movedAt ? 0x1019 : 0x10DA, 0x70), t);
                if (t >= dropEnd && relockSec < 0 && t < movedAt) relockSec = t - dropEnd;
                if (t >= movedAt && movedLockSec < 0 && std::fabs(radio - cc2) < 1) movedLockSec = t - movedAt;
            }
            c.tick(t);
            for (const Action& a : c.takeActions()) {
                if (a.kind != Action::Retune) continue;
                retunes++;
                if (std::fabs(a.freqHz - cc1) > 1 && std::fabs(a.freqHz - cc2) > 1 && std::fabs(a.freqHz - hz(0x14C)) > 1) outside++;
                radio = a.freqHz; lockAt = t + 4.0;
            }
        }
        CHECK(outside == 0, "recovery never leaves the site's own channels: %d", outside);
        CHECK(relockSec >= 0 && relockSec < 20, "re-locked after the drop: %.1f s after it ended", relockSec);
        CHECK(movedLockSec >= 0 && movedLockSec < 45, "found the moved control channel: %.1f s", movedLockSec);
        CHECK(c.follower.state() == FollowState::ControlHold && std::fabs(radio - cc2) < 1, "holding the moved control channel (%s)", followStateName(c.follower.state()));
        // the next loss starts with what was heard last
        const auto l = c.follower.heardControlChannels();
        CHECK(!l.empty() && std::fabs(l[0] - cc2) < 1, "last-good control channel is first in the hunt order");
        (void)retunes;
    }
    // ---- a first step on the channel the radio is on, silent for the loss time: the next candidate comes after one more loss time, not a whole dwell
    {
        Controller c;
        setupSite(c, 0);
        feed(c, gen::sccb(1, 5, 0x1020, 0x1021), 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.ccLossSec = 4; fc.ccDwellSec = 12; fc.allowRetune = true;
        c.follower.setConfig(fc);
        c.tick(4.5);
        auto a = c.takeActions();
        CHECK(c.follower.state() == FollowState::Searching, "lost after the loss time");
        c.tick(9.0);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a[0].freqHz - chHz(0x1020)) < 1, "second candidate after one more loss time (%d retunes)", count(a, Action::Retune));
    }
    // ---- talkgroup CSV
    {
        TalkgroupList l;
        const std::string csv = "Decimal,Hex,Alpha Tag,Mode,Description,Tag,Category\r\n"
                                "1201,4B1,\"Fire Disp\",D,\"Fire, dispatch\",Fire Dispatch,Metro Fire\r\n"
                                "1202,4B2,Fire Tac 1,DE,\"Quote \"\"x\"\" here\",Fire Tac,Metro Fire\r\n"
                                "abc,,bad row,D,,,\r\n"
                                "70000,,out of range,D,,,\r\n"
                                "\r\n"
                                "1203,,PD,T,,Law Dispatch,Metro PD\r\n";
        std::string err; int skipped = 0;
        const int n = l.importCsv(csv, &err, &skipped);
        CHECK(n == 3 && skipped == 2, "%d rows, %d skipped", n, skipped);
        CHECK(l.find(1201) && l.find(1201)->alpha == "Fire Disp" && l.find(1201)->desc == "Fire, dispatch" && l.find(1201)->category == "Metro Fire", "fields");
        CHECK(l.find(1202) && l.find(1202)->desc == "Quote \"x\" here" && l.find(1202)->encryptedMode(), "quotes and mode");
        l.find(1201)->priority = 2; l.find(1203)->lockout = true; l.find(1203)->list = "PD, all";
        const std::string out = l.exportCsv();
        TalkgroupList m;
        int sk2 = 0;
        CHECK(m.importCsv(out, nullptr, &sk2) == 3 && sk2 == 0, "round trip");
        CHECK(m.find(1201)->priority == 2 && m.find(1203)->lockout && m.find(1203)->list == "PD, all" && m.find(1202)->desc == "Quote \"x\" here", "round trip fields");
        // columns in another order and hex only
        TalkgroupList h;
        CHECK(h.importCsv("Hex,Alpha Tag,Decimal\n4B1,A,\n,B,1300\n") == 2 && h.find(1201) && h.find(1300), "reordered / hex-only columns");
        // no header: RadioReference order
        TalkgroupList n2;
        CHECK(n2.importCsv("500,1F4,Name,D,Desc,Tag,Cat\n") == 1 && n2.find(500)->alpha == "Name", "headerless");
        CHECK(n2.importCsv("") == 0 && n2.importCsv("\n\n") == 0, "empty input");
    }
    // ---- call log CSV
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        c.tgs.upsert(1201).alpha = "Fire, Disp";
        feed(c, gen::grpVGrant(4, 0x1010, 1201, 555), 1);
        c.tick(10);
        const std::string csv = c.log.exportCsv(&c.tgs);
        CHECK(csv.find("\"Fire, Disp\"") != std::string::npos && csv.find("851.20625") != std::string::npos && csv.find(",555,") != std::string::npos, "log csv: %s", csv.c_str());
    }
    // ---- DMR Tier III grants (layout under test is the simplified one documented in docs/trunking.md)
    {
        Controller c;
        c.dmr.setChannel(3, 450.125e6);
        c.setCapture(450.1e6, 1e6);
        uint8_t d[8] = {0x00, 0x03, 0x00, 0x00, 0x64, 0x00, 0x00, 0x2A};
        c.onDmrCsbk(0x30, 0, d, 1.0);
        CHECK(c.log.records().size() == 1 && c.log.records()[0].dmr && c.log.records()[0].tg == 100 && c.log.records()[0].src == 42 && c.log.records()[0].followed, "dmr grant");
        d[0] = 0x40; d[4] = 0x65;   // privacy, another talkgroup
        c.onDmrCsbk(0x30, 0, d, 5.0);
        CHECK(c.log.records().size() == 2 && c.log.records()[1].encrypted && !c.log.records()[1].followed, "dmr privacy not followed");
        c.onDmrCsbk(0x30, 0x10, d, 6.0);   // a manufacturer's
        CHECK(c.log.records().size() == 2, "manufacturer CSBK ignored");
    }
    // ---- malformed and random input
    {
        srand(7);
        Controller c;
        c.setCapture(851.1e6, 2.4e6);
        c.tgs.setScanOnly(true);
        FollowConfig fc; fc.maxSlices = 3;
        c.follower.setConfig(fc);
        double now = 0;
        for (int i = 0; i < 20000; i++) {
            p25::Tsbk t;
            t.opcode = rand() % 64; t.mfid = rand() % 5 ? 0 : rand() % 256;
            t.args = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand();
            if (i % 7 == 0) t.opcode = (int[]){0x00, 0x02, 0x03, 0x04, 0x33, 0x34, 0x39, 0x3A, 0x3B, 0x3C, 0x3D}[rand() % 11];
            now += (rand() % 100) / 100.0;
            c.onTsbk(t, rand() & 0xFFF, now);
            if (i % 5 == 0) c.onTsbkBad(now);
            if (i % 3 == 0) c.tick(now);
            if (i % 11 == 0) c.takeActions();
            if (i % 997 == 0) c.setCapture((rand() % 2) ? 851.1e6 : 0, (rand() % 2) ? 2.4e6 : 0);
            if (i % 500 == 0) { TgEntry& e = c.tgs.upsert(rand() % 5000); e.listen = rand() & 1; e.lockout = rand() % 4 == 0; e.priority = rand() % 11; }
            if (c.follower.activeSlices() > 3) { CHECK(false, "more slices than allowed"); break; }
        }
        std::string junk;
        for (int i = 0; i < 20000; i++) junk += (char)(rand() % 6 == 0 ? '"' : rand() % 6 == 0 ? ',' : rand() % 8 == 0 ? '\n' : rand() % 256);
        TalkgroupList l;
        l.importCsv(junk);
        l.importCsv("Decimal,Hex\n\"unterminated,1\n99999999999999999999,\n-5,\n");
        CHECK(true, "survived");
        uint8_t d[8];
        for (int i = 0; i < 2000; i++) { for (auto& b : d) b = (uint8_t)rand(); c.onDmrCsbk(0x30 + rand() % 4, rand() % 3 ? 0 : 1, d, now += 0.01); }
    }
    // ---- P25 Phase 2 grants (a TDMA identifier, two slots per carrier): followed with the carrier and the slot, one at a time inside the capture
    {
        Controller c;
        setupSite(c, 0);
        feed(c, gen::idenUpTdma(6, 3, 851006250, 12500, -45000000), 0);   // channel 6-0x21: carrier 0x10 (851.20625 MHz), slot 1
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.hangSec = 2;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x04, 0x6021, 7001, 11), 1.0);
        auto a = c.takeActions();
        bool open = false;
        for (const auto& x : a) if (x.kind == Action::OpenSlice && x.tdma && x.slot == 1 && std::fabs(x.freqHz - chHz(0x1010)) < 1) open = true;
        CHECK(open, "Phase 2 grant opens a Phase 2 slice on the carrier with its slot (%zu actions)", a.size());
        CHECK(c.log.records().back().followed && c.log.records().back().slot == 1, "Phase 2 call followed");
        // the other slot of a carrier while one Phase 2 call runs inside the capture: busy (one Phase 2 receiver)
        feed(c, gen::grpVGrant(0x04, 0x6020, 7002, 12), 1.1);
        a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 0 && c.log.records().back().missed && c.log.records().back().note.find("one Phase 2 call") != std::string::npos,
              "second Phase 2 call: '%s'", c.log.records().back().note.c_str());
        // a Phase 1 call next to it is still followed
        feed(c, gen::grpVGrant(0x04, 0x1012, 7003, 13), 1.2);
        a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 1 && !a[0].tdma, "Phase 1 call beside the Phase 2 one");
        // the voice channel says encrypted: logged so, and the slice closes
        const uint64_t id = c.log.records()[c.log.records().size() - 3].id;
        c.onEncrypted(id, 1.5);
        c.tick(1.6);
        a = c.takeActions();
        bool closed = false;
        for (const auto& x : a) if (x.kind == Action::CloseSlice && x.callId == id && x.tdma) closed = true;
        CHECK(closed && c.log.byId(id)->encrypted && c.log.byId(id)->active && !c.log.byId(id)->followed, "encrypted on the voice channel: left, still running");
        // its grant keeps being repeated on the control channel: that must not open it again (the open / close loop heard on the NSW PSN)
        const size_t nRec = c.log.records().size();
        feed(c, gen::grpVGrant(0x04, 0x6021, 7001, 11), 1.7);
        c.tick(1.8);
        a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 0 && c.log.records().size() == nRec, "grant repeat of the encrypted call: %zu actions, %zu records", a.size(), c.log.records().size());
        c.tick(5.0);   // no more grants: it ends like any call nobody follows
        CHECK(!c.log.byId(id)->active, "the encrypted call ends when its grants stop");
        // a wide capture without whole-span decoding: the Phase 2 call is still taken in place (the second mode needs no carrier decoders),
        // a Phase 1 call in it hops the radio, or with retuning off says what to turn on
        {
            Controller w;
            setupSite(w, 0);
            feed(w, gen::idenUpTdma(6, 3, 851006250, 12500, -45000000), 0);
            w.setCapture(851.1e6, 30.72e6, false);
            FollowConfig fw; fw.allowRetune = false;
            w.follower.setConfig(fw);
            feed(w, gen::grpVGrant(0x04, 0x6021, 7101, 21), 1.0);
            a = w.takeActions();
            CHECK(count(a, Action::OpenSlice) == 1 && a[0].tdma && a[0].slot == 1, "Phase 2 in a capture without whole-span decoding: '%s'", w.log.records().back().note.c_str());
            feed(w, gen::grpVGrant(0x04, 0x1012, 7102, 22), 1.1);
            a = w.takeActions();
            CHECK(count(a, Action::OpenSlice) == 0 && w.log.records().back().note.find("whole-span decoding") != std::string::npos, "Phase 1 there: '%s'", w.log.records().back().note.c_str());
            Controller h;
            setupSite(h, 0);
            FollowConfig fh; fh.allowRetune = true;
            h.follower.setConfig(fh);
            h.setCapture(851.1e6, 30.72e6, false);
            feed(h, gen::grpVGrant(0x04, 0x1012, 7103, 23), 1.0);
            a = h.takeActions();
            CHECK(count(a, Action::Retune) == 1 && count(a, Action::OpenSlice) == 0, "Phase 1 in the capture without whole-span decoding hops");
        }
        // Phase 2 following off: the old note
        Controller d;
        setupSite(d, 0);
        feed(d, gen::idenUpTdma(6, 3, 851006250, 12500, -45000000), 0);
        d.setCapture(851.1e6, 2.4e6);
        FollowConfig f2; f2.followPhase2 = false;
        d.follower.setConfig(f2);
        feed(d, gen::grpVGrant(0x04, 0x6021, 7001, 11), 1.0);
        CHECK(d.takeActions().empty() && !d.log.records().back().followed && d.log.records().back().note.find("Phase 2 following is off") != std::string::npos, "Phase 2 off");
        // outside the capture: the radio hops with the slot, the voice keeps it there (no grant repeats are heard on the voice channel)
        Controller e;
        setupSite(e, 0);
        feed(e, gen::idenUpTdma(6, 3, 856000000, 12500, 0), 0);
        e.setCapture(chHz(0x1001), 0);
        FollowConfig f3; f3.hangSec = 1.5; f3.allowRetune = true;
        e.follower.setConfig(f3);
        feed(e, gen::grpVGrant(0x04, 0x6020, 7004, 14), 1.0);
        a = e.takeActions();
        CHECK(count(a, Action::Retune) == 1 && a[0].tdma && a[0].slot == 0 && std::fabs(a[0].freqHz - (856000000 + 16 * 12500.0)) < 1, "Phase 2 hop with slot 0");
        const uint64_t hid = e.log.records().back().id;
        for (double t = 1.5; t < 6; t += 0.5) { e.onVoice(hid, t); e.tick(t); }
        CHECK(e.follower.state() == FollowState::Hopped, "voice keeps the Phase 2 hop (%s)", followStateName(e.follower.state()));
        e.tick(8);
        a = e.takeActions();
        CHECK(count(a, Action::Retune) == 1 && !a.back().tdma && std::fabs(a.back().freqHz - chHz(0x1001)) < 1, "back to the control channel after the call");
    }
    // ---- voice radio: a second receiver takes calls outside the capture, the control channel radio stays
    {
        Controller c;
        setupSite(c, 0);
        feed(c, gen::idenUpTdma(6, 3, 856000000, 12500, 0), 0);
        c.setCapture(chHz(0x1001), 0);   // only the control channel
        FollowConfig fc; fc.hangSec = 1.5; fc.voiceRadio = true; fc.remoteStartSec = 3.0;
        c.follower.setConfig(fc);
        FakeVoiceRadio vr;
        // Phase 1 call: the voice radio opens, the control radio is not retuned
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.0);
        auto a = c.takeActions();
        CHECK(count(a, Action::OpenRemote) == 1 && count(a, Action::Retune) == 0 && count(a, Action::OpenSlice) == 0, "grant outside the capture goes to the voice radio, no retune");
        CHECK(!a.empty() && a[0].kind == Action::OpenRemote && !a[0].tdma && std::fabs(a[0].freqHz - chHz(0x1010)) < 1, "voice radio frequency");
        const uint64_t id1 = c.log.records().back().id;
        // put the actions back through the fake: it gets them again
        for (const Action& x : a) if (x.kind == Action::OpenRemote) { vr.open = true; vr.call = x.callId; vr.hz = x.freqHz; vr.opens++; }
        CHECK(c.follower.state() == FollowState::Following && c.follower.sliceFreqs(false).empty(), "following, and the wide bank list stays empty (%s)", followStateName(c.follower.state()));
        // the control channel keeps being heard and its grant repeats do not keep a call alive that the voice radio does not hear
        for (double t = 1.2; t < 2.2; t += 0.2) { feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), t); c.onVoice(id1, t); c.tick(t); }
        CHECK(c.log.records().back().active && c.follower.remoteActive() && c.follower.remoteCall() == id1, "voice keeps the call");
        // a second grant for another talkgroup while the voice radio is busy: not followed, not hopped, logged
        feed(c, gen::grpVGrant(0x00, 0x1012, 4002, 22), 2.0);
        a = c.takeActions();
        CHECK(count(a, Action::OpenRemote) == 0 && count(a, Action::Retune) == 0 && c.log.records().back().missed && c.log.records().back().note.find("voice radio") != std::string::npos,
              "two grants in a row: the second is missed with a note (%s)", c.log.records().back().note.c_str());
        // a higher priority talkgroup preempts
        c.tgs.upsert(4003).priority = 1;
        c.tgs.upsert(4001).priority = 5;
        // (the running call was granted before its priority was set: its slot priority is the default 0, i.e. none = lowest)
        feed(c, gen::grpVGrant(0x00, 0x1013, 4003, 23), 2.1);
        a = c.takeActions();
        CHECK(count(a, Action::CloseRemote) == 1 && count(a, Action::OpenRemote) == 1 && a[0].kind == Action::RecordStop, "a higher priority call preempts: close then open (%d)", (int)a.size());
        const uint64_t id3 = c.log.records().back().id;
        CHECK(c.follower.remoteCall() == id3, "voice radio is on the new call");
        // terminator: the call ends after the hang time, the voice radio is closed, the follower is back on the control channel (state ControlHold)
        c.onTerminator(id3, 3.0);
        c.tick(3.0);
        a = c.takeActions();
        CHECK(count(a, Action::CloseRemote) == 1 && !c.follower.remoteActive(), "terminator closes the voice radio");
        c.tick(3.1);
        CHECK(c.follower.state() == FollowState::ControlHold && count(c.takeActions(), Action::Retune) == 0, "back to idle with no control-channel retune (%s)", followStateName(c.follower.state()));
        CHECK(!c.log.records().back().active, "call is over in the log");
    }
    {   // hang time: voice stops, the call ends hangSec later; voice that goes on keeps it
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.hangSec = 2.0; fc.voiceRadio = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.0);
        const uint64_t id = c.log.records().back().id;
        (void)c.takeActions();
        for (double t = 1.2; t < 4; t += 0.2) { c.onVoice(id, t); c.tick(t); }
        CHECK(c.follower.remoteActive(), "long call kept by voice");
        c.tick(5.0);
        CHECK(c.follower.remoteActive(), "inside the hang time");
        c.tick(6.1);
        CHECK(!c.follower.remoteActive() && count(c.takeActions(), Action::CloseRemote) == 1 && !c.log.records().back().missed, "hang time over: closed, call not marked missed");
    }
    {   // Phase 2 call on the voice radio carries the slot
        Controller c;
        setupSite(c, 0);
        feed(c, gen::idenUpTdma(6, 3, 856000000, 12500, 0), 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.voiceRadio = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x04, 0x6021, 7005, 15), 1.0);
        const auto a = c.takeActions();
        CHECK(count(a, Action::OpenRemote) == 1 && a[0].tdma && a[0].slot == 1, "Phase 2 voice radio call with slot 1");
    }
    {   // encrypted: grants that say so are never sent; a voice channel that says so ends the call at once
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.voiceRadio = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x40, 0x1010, 4004, 24), 1.0);
        CHECK(count(c.takeActions(), Action::OpenRemote) == 0 && c.log.records().back().encrypted, "encrypted grant never opens the voice radio");
        feed(c, gen::grpVGrant(0x00, 0x1011, 4005, 25), 2.0);
        (void)c.takeActions();
        const uint64_t id = c.log.records().back().id;
        c.onEncrypted(id, 2.3);
        c.tick(2.31);
        const auto b = c.takeActions();
        CHECK(count(b, Action::CloseRemote) == 1 && !c.follower.remoteActive() && c.log.records().back().encrypted && !c.log.records().back().followed, "encrypted voice channel: voice radio left at once");
        feed(c, gen::grpVGrant(0x00, 0x1011, 4005, 25), 2.5);   // the grant repeats: it must not open again
        CHECK(count(c.takeActions(), Action::OpenRemote) == 0, "no reopen of an encrypted call");
    }
    {   // failures: the second radio went away / the retune failed / the radio never hears the call
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.voiceRadio = true; fc.remoteStartSec = 3.0; fc.remoteBackoffSec = 3.0;
        c.follower.setConfig(fc);
        FakeVoiceRadio vr;
        vr.failRetune = true;
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.0);
        vr.apply(c, 1.0);
        const CallRecord& r = c.log.records().back();
        CHECK(!vr.open && r.missed && !r.followed && r.note.find("retune failed") != std::string::npos && !c.follower.remoteActive(), "failed retune: logged missed with the reason (%s)", r.note.c_str());
        CHECK(c.follower.state() != FollowState::NoControl && count(c.takeActions(), Action::Retune) == 0, "control channel radio untouched");
        // within the back-off a repeat is skipped, afterwards it tries again
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.5);
        CHECK(count(c.takeActions(), Action::OpenRemote) == 0, "back-off after a failure");
        vr.failRetune = false;
        c.tick(2.0);
        c.tick(4.4);
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 4.5);
        vr.apply(c, 4.5);
        CHECK(vr.open && vr.opens == 1 && c.follower.remoteActive(), "tries again after the back-off");
        // the radio disappears while on a call
        vr.present = false;
        c.onRemoteFailed(vr.call, "second radio is gone", 5.0);
        vr.apply(c, 5.0);
        CHECK(!c.follower.remoteActive() && c.log.records().back().note.find("gone") != std::string::npos, "second radio disappearing ends the call");
        // a radio that never hears the call: dropped after remoteStartSec, then the back-off
        vr.present = true;
        feed(c, gen::grpVGrant(0x00, 0x1014, 4006, 26), 10.0);
        vr.apply(c, 10.0);
        CHECK(c.follower.remoteActive(), "call open on the voice radio");
        for (double t = 10.5; t < 12.5; t += 0.5) { feed(c, gen::grpVGrant(0x00, 0x1014, 4006, 26), t); c.tick(t); }   // grant repeats do not keep it alive
        CHECK(c.follower.remoteActive(), "still waiting for the voice radio to sync");
        c.tick(13.5);
        vr.apply(c, 13.5);
        CHECK(!c.follower.remoteActive() && c.log.records().back().missed && c.log.records().back().note.find("heard nothing") != std::string::npos, "never heard: dropped (%s)", c.log.records().back().note.c_str());
        feed(c, gen::grpVGrant(0x00, 0x1014, 4006, 26), 14.0);
        CHECK(count(c.takeActions(), Action::OpenRemote) == 0, "back-off after a call the radio never heard");
    }
    {   // single radio behaviour untouched: a grant inside the capture still takes the slice path with the voice radio on
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.voiceRadio = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.0);
        const auto a = c.takeActions();
        CHECK(count(a, Action::OpenSlice) == 1 && count(a, Action::OpenRemote) == 0, "inside the capture: single-radio slice path");
        // and with the option off an outside grant is missed as before
        Controller d;
        setupSite(d, 0);
        d.setCapture(chHz(0x1001), 0);
        feed(d, gen::grpVGrant(0x00, 0x1010, 4001, 21), 1.0);
        CHECK(count(d.takeActions(), Action::OpenRemote) == 0 && d.log.records().back().missed, "voice radio off: unchanged");
    }
    // ---- the followed call ends while another one is still on the air: its next grant repeat moves the follower to it
    {   // a slice inside the capture
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        FollowConfig fc; fc.maxSlices = 1; fc.hangSec = 1.0;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(4, 0x1010, 6001, 1), 1.0);
        const uint64_t a1 = c.log.records().back().id;
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.1);
        const uint64_t b1 = c.log.records().back().id;
        CHECK(count(c.takeActions(), Action::OpenSlice) == 1 && c.log.byId(b1)->missed && c.follower.missedCalls() == 1, "second call busy");
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.5);   // a repeat while still busy: not missed twice
        CHECK(c.takeActions().empty() && c.follower.missedCalls() == 1 && c.log.records().size() == 2, "busy repeat: no action, counted once");
        c.onTerminator(a1, 2.0);
        c.tick(2.0);
        CHECK(count(c.takeActions(), Action::CloseSlice) == 1 && c.follower.activeSlices() == 0, "first call ends");
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 2.2);
        auto a = c.takeActions();
        const CallRecord* b = c.log.byId(b1);
        CHECK(count(a, Action::OpenSlice) == 1 && a[0].callId == b1 && std::fabs(a[0].freqHz - chHz(0x1011)) < 1, "the ongoing call gets the slice");
        CHECK(b->followed && !b->missed && b->active && c.follower.missedCalls() == 0 && c.follower.audibleCall() == b1 && c.log.records().size() == 2, "joined, same log record, not missed");
    }
    {   // the voice radio
        Controller c;
        setupSite(c, 0);
        c.setCapture(chHz(0x1001), 0);
        FollowConfig fc; fc.hangSec = 1.0; fc.voiceRadio = true;
        c.follower.setConfig(fc);
        feed(c, gen::grpVGrant(0x00, 0x1010, 6001, 1), 1.0);
        const uint64_t a1 = c.log.records().back().id;
        feed(c, gen::grpVGrant(0x00, 0x1012, 6002, 2), 1.1);
        const uint64_t b1 = c.log.records().back().id;
        CHECK(count(c.takeActions(), Action::OpenRemote) == 1 && c.log.byId(b1)->missed, "voice radio busy");
        c.onVoice(a1, 1.5);
        c.onTerminator(a1, 2.0);
        c.tick(2.0);
        CHECK(count(c.takeActions(), Action::CloseRemote) == 1, "first call ends");
        feed(c, gen::grpVGrant(0x00, 0x1012, 6002, 2), 2.2);
        auto a = c.takeActions();
        CHECK(count(a, Action::OpenRemote) == 1 && c.follower.remoteCall() == b1 && c.log.byId(b1)->followed, "the voice radio goes to the ongoing call");
    }
    {   // one radio: a call inside the capture blocks a hop; when it ends the radio hops to the call still running outside
        Controller c;
        setupSite(c, 0);
        FollowConfig fc; fc.allowRetune = true; fc.hangSec = 1.0;
        c.follower.setConfig(fc);
        c.setCapture(851.1e6, 1.0e6);
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        feed(c, gen::grpVGrant(4, 0x1010, 6001, 1), 1.0);
        const uint64_t a1 = c.log.records().back().id;
        feed(c, gen::grpVGrant(4, 0x5010, 6002, 2), 1.1);
        const uint64_t b1 = c.log.records().back().id;
        CHECK(count(c.takeActions(), Action::Retune) == 0 && c.log.byId(b1)->missed, "no hop while a call runs");
        c.onTerminator(a1, 2.0);
        c.tick(2.0);
        c.takeActions();
        feed(c, gen::grpVGrant(4, 0x5010, 6002, 2), 2.2);
        auto a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && a[0].callId == b1 && c.follower.state() == FollowState::Hopped, "hops to the ongoing call (%s)", followStateName(c.follower.state()));
    }
    // an encrypted or locked out call that is still running is not joined
    {
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        c.tgs.upsert(6002).lockout = true;
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.0);
        feed(c, gen::grpVGrant(0x40, 0x1012, 6003, 3), 1.0);
        c.takeActions();
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.5);
        feed(c, gen::grpVGrant(0x40, 0x1012, 6003, 3), 1.5);
        CHECK(c.takeActions().empty() && c.follower.activeSlices() == 0, "not joined");
    }
    // ---- fix35-p25: single-radio hop gets a start grace; a hop that heard nothing backs off; late updates, emergency, unplaced types, status line
    {   // H1: hop, no voice for 3 s: still hopped; then voice keeps it open
        Controller c;
        setupSite(c, 0);
        FollowConfig fc; fc.allowRetune = true;
        c.follower.setConfig(fc);
        c.setCapture(851.1e6, 0);
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        feed(c, gen::grpVGrant(4, 0x5010, 6001, 1), 1.0);
        const uint64_t id = c.log.records().back().id;
        CHECK(count(c.takeActions(), Action::Retune) == 1 && c.follower.state() == FollowState::Hopped, "hops");
        c.tick(4.0);
        CHECK(c.follower.state() == FollowState::Hopped && count(c.takeActions(), Action::Retune) == 0, "H1: still on the voice channel after 3 s without voice");
        c.onVoice(id, 4.2);
        c.tick(5.5);
        CHECK(c.follower.state() == FollowState::Hopped, "H1: voice keeps the hop open past the start grace");
        c.tick(7.0);
        CHECK(c.follower.state() == FollowState::Returning && count(c.takeActions(), Action::Retune) == 1, "H1: hang time after voice, then home");
    }
    {   // H1: no voice ever: returns after the start timeout, and the same talkgroup does not re-hop at once
        Controller c;
        setupSite(c, 0);
        FollowConfig fc; fc.allowRetune = true;
        c.follower.setConfig(fc);
        c.setCapture(851.1e6, 0);
        feed(c, gen::idenUp(5, 856000000, 12500, 0, 12500), 0);
        feed(c, gen::grpVGrant(4, 0x5010, 6001, 1), 1.0);
        c.takeActions();
        c.tick(8.0);
        CHECK(c.follower.state() == FollowState::Returning && count(c.takeActions(), Action::Retune) == 1, "H1: heard nothing: back to the control channel");
        c.follower.onControlSeen(8.5);
        feed(c, gen::grpVGrant(4, 0x5010, 6001, 1), 9.0);
        CHECK(count(c.takeActions(), Action::Retune) == 0, "H1: backed off after a hop that heard nothing");
        feed(c, gen::grpVGrant(4, 0x5010, 6001, 1), 17.0);
        CHECK(count(c.takeActions(), Action::Retune) == 1, "H1: tries again after the back-off");
    }
    {   // M7: an update for a call that has just ended opens nothing
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.0);
        const uint64_t id = c.log.records().back().id;
        c.takeActions();
        c.onTerminator(id, 1.5);
        c.tick(1.6);
        c.takeActions();
        const size_t n = c.log.records().size();
        feed(c, gen::grpVGrantUpd(0x1011, 6002, 0x1011, 6002), 1.9);
        CHECK(c.log.records().size() == n && count(c.takeActions(), Action::OpenSlice) == 0, "M7: late update ignored");
        feed(c, gen::grpVGrantUpd(0x1011, 6002, 0x1011, 6002), 12.0);
        CHECK(count(c.takeActions(), Action::OpenSlice) == 1, "M7: a later update is a call joined late");
    }
    {   // M8: an emergency call pre-empts a running low priority one
        Controller c;
        setupSite(c, 0);
        FollowConfig fc; fc.maxSlices = 1;
        c.follower.setConfig(fc);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::grpVGrant(4, 0x1011, 6001, 1), 1.0);
        c.takeActions();
        feed(c, gen::grpVGrant(0x84, 0x1012, 6002, 2), 1.2);
        auto a = c.takeActions();
        CHECK(count(a, Action::CloseSlice) == 1 && count(a, Action::OpenSlice) == 1, "M8: emergency pre-empts");
    }
    {   // M10: a channel type this build cannot place says so
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::idenUpTdma(6, 5, 851006250, 12500, 0), 0);
        feed(c, gen::grpVGrant(4, 0x6010, 6001, 1), 1.0);
        const CallRecord& r = c.log.records().back();
        CHECK(r.note.find("channel type 5") != std::string::npos, "M10: honest reason (%s)", r.note.c_str());
    }
    {   // M9: Phase 2 grant before NET_STS: a reason, no follow
        Controller c;
        feed(c, gen::idenUpTdma(1, 3, 851006250, 12500, 0), 0);
        c.setCapture(851.1e6, 2.4e6);
        feed(c, gen::grpVGrant(4, 0x1010, 6001, 1), 1.0);
        CHECK(count(c.takeActions(), Action::OpenSlice) == 0 && c.log.records().back().note.find("waiting for the system broadcast") != std::string::npos, "M9");
        feed(c, gen::netSts(0xBEE00, 0x3A1, 0x1001, 0x70), 1.2);
        feed(c, gen::grpVGrant(4, 0x1010, 6001, 1), 1.5);
        CHECK(count(c.takeActions(), Action::OpenSlice) == 1, "M9: once the seed is known");
    }
    {   // M6: a hunt does not adopt a control channel of another NAC
        Follower f;
        FollowConfig fc; fc.allowRetune = true; fc.ccLossSec = 2; fc.ccDwellSec = 3;
        f.setConfig(fc);
        CallLog log;
        f.setCapture(851e6, 0);
        f.setControlChannel(851.0e6, {851.0e6, 852.0e6});
        f.onControlSeen(0.1, 0x293);
        f.tick(0.2, log);
        f.tick(5.0, log);   // lost, searching
        CHECK(f.state() == FollowState::Searching, "searching (%s)", followStateName(f.state()));
        f.onControlSeen(5.5, 0x111);
        CHECK(f.state() == FollowState::Searching, "M6: other NAC not adopted");
        f.onControlSeen(6.0, 0x293);
        CHECK(f.state() == FollowState::ControlHold, "M6: own NAC adopted");
    }
    {   // status line
        Controller c;
        setupSite(c, 0);
        c.setCapture(851.1e6, 2.4e6);
        c.tick(0.5);
        feed(c, gen::grpVGrant(4, 0x1011, 6002, 2), 1.0);
        const std::string s = c.statusLine(1.0);
        CHECK(s.find("Following TG 6002 on 851.") == 0, "status: %s", s.c_str());
        Controller d;
        setupSite(d, 0);
        d.setCapture(851.1e6, 0);
        feed(d, gen::grpVGrant(4, 0x5010, 6003, 2), 1.0);
        feed(d, gen::idenUp(5, 856000000, 12500, 0, 12500), 1.0);
        feed(d, gen::grpVGrant(4, 0x5010, 6003, 2), 1.1);
        const std::string t = d.statusLine(1.1);
        CHECK(t.find("Not following TG 6003") == 0 && t.find("second radio") != std::string::npos, "status reason: %s", t.c_str());
    }
    return testSummary("p25 trunk");
}
