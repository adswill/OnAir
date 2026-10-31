// Phase 1 and Phase 2 voice channels behind one Phase 1 control channel, one retunable radio (p25_scenario.h): the grant's identifier decides (a
// standard identifier gives a Phase 1 call, a TDMA identifier a Phase 2 call on its slot), the radio hops to each, the right receiver reports the
// voice, and the radio comes home after each. Phase 2 is scrambled with the seed from the control channel's WACN / system / NAC. An encrypted Phase 2
// call (ESS) is logged as encrypted and left. A Phase 1 receiver on a Phase 2 carrier hears no voice (there is no signal-level phase detection:
// the phase is the identifier's). NOT covered: a real transmitter, AMBE+2 audio (placeholder frames).
#include "p25_scenario.h"
#include <cstdio>
using namespace dect2;
using namespace p25sim;
#include "../common/test_util.h"

int main() {
    const double rate = 288000;
    const Band b8 = band800();
    Band bt = bandTdma();                       // 770 MHz, 12.5 kHz, two slots
    const double ccHz = b8.freq(1), v1 = b8.freq(0x40), v2 = bt.freq(5, 2);   // channel 5: carrier 2, slot 2
    CcSpec site; site.bands = {b8, bt};
    RigCfg rc; rc.rate = rate; rc.follow.allowRetune = true; rc.follow.hangSec = 2.0;
    // ---- Phase 1 call, then a Phase 2 call, then an encrypted Phase 2 call
    {
        Rig rig(rc);
        rig.world.noiseSnrDb = 30; rig.world.dcSpur = 0.2;
        Impair imp; imp.cfoHz = 500;
        const std::vector<GrantWin> wins = {{{{b8.channel(0x40), 1201, 111}}, 1.0, 1.6}, {{{bt.channel(5), 3003, 333}}, 10.0, 10.6}, {{{bt.channel(4), 3004, 444}}, 20.0, 20.6}};
        addControl(rig.world, ccHz, site, wins, {}, 1e9, imp);
        addVoice(rig.world, v1, 0x293, 1201, 111, 1.0, 4.0, false, imp);
        rig.world.add(phase2("p2 clear", v2, p2Call(site, 2, 3003, 333, 4.0), 10.0, 15.0, imp));
        rig.world.add(phase2("p2 enc", bt.freq(4, 2), p2Call(site, 1, 3004, 444, 6.0, 0x84), 20.0, 27.0, imp));
        rig.start(ccHz);
        rig.runUntil(8.5);
        const trunk::CallRecord* a = rig.callForTg(1201);
        CHECK(a && a->followed && a->slot < 0, "phase 1 call: followed, not TDMA (slot %d)", a ? a->slot : -9);
        CHECK(rig.hopVoiceReports > 5 && rig.p2VoiceReports == 0, "phase 1 call: voice from the Phase 1 receiver (%llu), none from Phase 2 (%llu)", (unsigned long long)rig.hopVoiceReports, (unsigned long long)rig.p2VoiceReports);
        bool p1Retune = false;
        for (const auto& ac : rig.executed) if (ac.kind == trunk::Action::Retune && ac.callId && !ac.tdma && std::fabs(ac.freqHz - v1) < 1) p1Retune = true;
        CHECK(p1Retune, "phase 1 call: Retune action to %.5f MHz without tdma", v1 / 1e6);
        CHECK(std::fabs(rig.radio.dialHz - ccHz) < 1, "phase 1 call: home again (%.5f MHz)", rig.radio.dialHz / 1e6);
        const uint64_t p1Reports = rig.hopVoiceReports;
        rig.runUntil(18.0);
        const trunk::CallRecord* b = rig.callForTg(3003);
        CHECK(b && b->followed && b->slot == 1, "phase 2 call: followed on slot %d", b ? b->slot : -9);
        bool p2Retune = false;
        for (const auto& ac : rig.executed) if (ac.kind == trunk::Action::Retune && ac.callId && ac.tdma && ac.slot == 1 && std::fabs(ac.freqHz - v2) < 1) p2Retune = true;
        CHECK(p2Retune, "phase 2 call: Retune action to %.5f MHz with tdma, slot 1", v2 / 1e6);
        CHECK(rig.p2VoiceReports > 5, "phase 2 call: %llu voice reports from the Phase 2 receiver", (unsigned long long)rig.p2VoiceReports);
        CHECK(rig.hopVoiceReports == p1Reports, "phase 2 call: the Phase 1 receiver reported nothing");
        CHECK(b && !b->encrypted, "phase 2 call: clear");
        CHECK(std::fabs(rig.radio.dialHz - ccHz) < 1 && rig.ctl.follower.state() != trunk::FollowState::Hopped, "phase 2 call: home again (%s)", trunk::followStateName(rig.ctl.follower.state()));
        const uint64_t before = rig.tsbksFed;
        rig.runUntil(19.5);
        CHECK(rig.tsbksFed > before + 5, "phase 2 call: control messages flow again");
        rig.runUntil(30.0);
        const trunk::CallRecord* c = rig.callForTg(3004);
        CHECK(c && c->encrypted, "encrypted phase 2 call: logged encrypted (%s)", c ? c->note.c_str() : "none");
        CHECK(rig.tunes_home_after(20.0) < 26.5, "encrypted phase 2 call: left before the call ends");
    }
    // ---- a Phase 1 receiver on a Phase 2 carrier
    {
        World w; w.rate = rate; w.noiseSnrDb = 30;
        w.add(phase2("p2", 770e6, p2Call(site, 1, 3003, 333, 3.0), 0.0, 5.0));
        P25Receiver rx; rx.setSilent(true); rx.configure(rate);
        std::vector<cf32> buf(14400);
        for (double t = 0; t < 4.0; t += buf.size() / rate) { w.mix(770e6 + kTuneOffsetHz, false, buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        P25Telemetry t; rx.telemetry(t, 0);
        CHECK(t.voiceFrames == 0 && t.ldu1 == 0 && !t.now.active, "phase 1 receiver on Phase 2: %llu voice frames, %llu LDU1", (unsigned long long)t.voiceFrames, (unsigned long long)t.ldu1);
    }
    return testSummary("p25 scenario p2");
}
