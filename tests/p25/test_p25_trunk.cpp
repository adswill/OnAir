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
        CHECK(c.follower.state() == FollowState::Searching && count(a, Action::Retune) == 1, "control lost: searching");
        c.tick(4.5);
        a = c.takeActions();
        CHECK(count(a, Action::Retune) == 1 && std::fabs(a[0].freqHz - chHz(0x1020)) < 1, "next candidate");
        feed(c, gen::rfssSts(0x3A1, 1, 5, 0x1020, 0x70), 5.0);   // heard it
        CHECK(c.follower.state() == FollowState::ControlHold, "found, holding");
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
    return testSummary("p25 trunk");
}
