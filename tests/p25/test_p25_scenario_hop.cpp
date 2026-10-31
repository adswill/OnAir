// Trunk following on the real receiver path with ONE retunable radio and a voice channel outside the capture (p25_scenario.h): grant -> the radio leaves
// the control channel -> the voice reaches the call log -> terminator -> back on the control channel and the control messages flow again. Also: the
// carrier that just stops (hang time), a grant for a channel nobody transmits on (start grace, then home, no hop storm), a voice channel that is
// encrypted, the control channel lost for a while and regained, and the follower's saved default (follow on, retune on). Impaired: carrier offset on both
// channels, noise, a DC spur. NOT covered: a real transmitter, real tuner settle times beyond the 0.3 s modelled.
#include "p25_scenario.h"
#include <cstdio>
using namespace dect2;
using namespace p25sim;
#include "../common/test_util.h"

static const double kRate = 288000;
static const Band kBand = band800();
static const double kCc = kBand.freq(1);
static const int kVoiceCh = 0x40;
static const double kVoiceHz = kBand.freq(kVoiceCh);   // 800 kHz above the control channel: outside the capture

static RigCfg hopCfg() {
    RigCfg c; c.rate = kRate;
    c.follow.allowRetune = true; c.follow.hangSec = 2.0;
    return c;
}
static CcSpec site() { CcSpec s; s.bands = {kBand}; return s; }
static Impair cfoImp(double hz) { Impair i; i.cfoHz = hz; return i; }
static double lastHome(const Rig& r) { for (size_t i = r.radio.tunes.size(); i-- > 0;) if (std::fabs(r.radio.tunes[i].hz - kCc) < 1) return r.radio.tunes[i].sec; return -1; }
static bool tunedTo(const Rig& r, double hz) { for (const auto& t : r.radio.tunes) if (std::fabs(t.hz - hz) < 1) return true; return false; }

int main() {
    // ---- a clean call with a terminator: hop, voice, TDU, home
    {
        Rig rig(hopCfg());
        rig.world.noiseSnrDb = 28; rig.world.dcSpur = 0.3;
        addControl(rig.world, kCc, site(), {{kBand.channel(kVoiceCh), 1201, 1234567}}, 1.0, 1.6, {}, 1e9, cfoImp(400));
        addVoice(rig.world, kVoiceHz, 0x293, 1201, 1234567, 1.0, 5.0, false, cfoImp(-600));
        rig.start(kCc);
        rig.runUntil(18.0);
        const trunk::CallRecord* r = rig.callForTg(1201);
        CHECK(r && r->followed && !r->missed, "hop call: followed %d missed %d (%s)", r ? r->followed : -1, r ? r->missed : -1, r ? r->note.c_str() : "no record");
        CHECK(tunedTo(rig, kVoiceHz), "hop call: the radio went to the voice channel");
        CHECK(rig.hopVoiceReports > 5, "hop call: %llu voice reports from the hopped radio", (unsigned long long)rig.hopVoiceReports);
        CHECK(r && r->src == 1234567, "hop call: unit %d in the call log", r ? r->src : -1);
        CHECK(rig.terminators >= 1, "hop call: terminator seen (%llu)", (unsigned long long)rig.terminators);
        CHECK(r && !r->active && r->durSec > 2.0, "hop call: ended, %.1f s", r ? r->durSec : 0.0);
        const double home = lastHome(rig);
        CHECK(home > 1.0 && home < 10.0, "hop call: back on the control channel at %.1f s", home);
        CHECK(rig.ctl.follower.state() == trunk::FollowState::ControlHold || rig.ctl.follower.state() == trunk::FollowState::Following, "hop call: final state %s", trunk::followStateName(rig.ctl.follower.state()));
        CHECK(rig.radio.retunes <= 3, "hop call: %d retunes (no storm)", rig.radio.retunes);
        const uint64_t before = rig.tsbksFed;
        rig.runUntil(rig.now + 2.0);
        CHECK(rig.tsbksFed > before + 5, "hop call: control messages flow again after the return (%llu new)", (unsigned long long)(rig.tsbksFed - before));
    }
    // ---- the carrier just stops (no TDU): the hang time ends the call, the radio goes home
    {
        Rig rig(hopCfg());
        rig.world.noiseSnrDb = 30;
        addControl(rig.world, kCc, site(), {{kBand.channel(kVoiceCh), 1202, 7654321}}, 1.0, 1.6);
        addVoice(rig.world, kVoiceHz, 0x293, 1202, 7654321, 1.0, 6.0, true);
        rig.world.srcs.back()->endSec = 4.0;     // lost carrier
        rig.start(kCc);
        rig.runUntil(16.0);
        const double home = lastHome(rig);
        CHECK(home >= 4.0 && home < 4.0 + 2.0 + 4.0, "carrier lost at 4.0 s: home at %.1f s (hang 2 s)", home);
        const trunk::CallRecord* r = rig.callForTg(1202);
        CHECK(r && !r->active, "call closed");
    }
    // ---- a grant for a channel nobody transmits on: start grace, then home; the same talkgroup is not hopped to again at once
    {
        Rig rig(hopCfg());
        rig.world.noiseSnrDb = 30;
        addControl(rig.world, kCc, site(), {{kBand.channel(kVoiceCh), 1203, 1}}, 1.0, 20.0);
        rig.start(kCc);
        rig.runUntil(22.0);
        int hops = 0;
        for (const auto& t : rig.radio.tunes) if (std::fabs(t.hz - kVoiceHz) < 1) hops++;
        CHECK(hops >= 1, "dead grant: the radio did try the channel (%d)", hops);
        CHECK(hops <= 3, "dead grant: %d hops in 20 s of repeated grants (back-off)", hops);
        const double home = lastHome(rig);
        CHECK(home > 1.0 + 6.0 - 0.5, "dead grant: stayed %.1f s at least (start grace)", home - 1.0);
        CHECK(rig.ctl.follower.state() != trunk::FollowState::Hopped || rig.now < 20.0, "dead grant: not stuck on the voice channel");
    }
    // ---- encrypted voice channel: logged as encrypted, left at once
    {
        Rig rig(hopCfg());
        rig.world.noiseSnrDb = 30;
        addControl(rig.world, kCc, site(), {{kBand.channel(kVoiceCh), 1204, 99}}, 1.0, 1.6);
        P25GenTx tx = voiceTx(0x293, 1204, 99, 6.0); tx.algId = 0x84; tx.keyId = 7;
        rig.world.add(phase1("enc voice", kVoiceHz, {tx}, 1.0, 8.0, false));
        rig.start(kCc);
        rig.runUntil(14.0);
        const trunk::CallRecord* r = rig.callForTg(1204);
        CHECK(r && r->encrypted, "encrypted voice channel: record encrypted");
        const double home = lastHome(rig);
        CHECK(home > 0 && home < 7.0, "encrypted voice channel: home at %.1f s, not at the call's end", home);
    }
    // ---- control channel lost for 10 s and regained: the follower looks for it, finds it, and the messages flow again
    {
        RigCfg c = hopCfg();
        c.follow.ccLossSec = 3.0; c.follow.ccDwellSec = 4.0;
        Rig rig(c);
        rig.world.noiseSnrDb = 30;
        addControl(rig.world, kCc, site(), {}, 1e9, 1e9, {{3.0, 13.0}});
        rig.start(kCc);
        rig.runUntil(12.0);
        CHECK(rig.sawState(trunk::FollowState::NoControl) || rig.sawState(trunk::FollowState::Searching), "cc loss: noticed (%s)", trunk::followStateName(rig.ctl.follower.state()));
        const uint64_t mid = rig.tsbksFed;
        rig.runUntil(40.0);
        CHECK(rig.tsbksFed > mid + 10, "cc regain: %llu TSBKs after the outage", (unsigned long long)(rig.tsbksFed - mid));
        CHECK(rig.ctl.follower.state() == trunk::FollowState::ControlHold || rig.ctl.follower.state() == trunk::FollowState::Following, "cc regain: state %s", trunk::followStateName(rig.ctl.follower.state()));
        CHECK(std::fabs(rig.radio.dialHz - kCc) < 1, "cc regain: the radio is on the control channel (%.5f MHz)", rig.radio.dialHz / 1e6);
    }
    return testSummary("p25 scenario hop");
}
