// Trunk following inside a WIDE capture (p25_scenario.h): the control channel and the voice channels are all in one 1.2 MHz capture at different
// offsets, the radio never retunes. Phase 1 calls get a slice of the whole-span bank (WideModes follow-only, the app's wideFollow), a Phase 2 call
// runs on the second Phase 2 receiver; the voice reaches the call log, the slices close with the calls, no retune happens. Also: more granted calls
// than slices (the extra one is logged as missed), and a call whose channel is in the capture's edge guard (not taken as inside). Impaired.
#include "p25_scenario.h"
#include <cstdio>
using namespace dect2;
using namespace p25sim;
#include "../common/test_util.h"

int main() {
    const double rate = 1200000;
    const Band b8 = band800();
    const double ccHz = b8.freq(1), centre = 851200000;
    const double v1 = b8.freq(0x20), v2 = b8.freq(0x30);   // 851.40625 and 851.60625 MHz
    CcSpec site; site.bands = {b8};
    Band bt = bandTdma(); bt.baseHz = 851500000; bt.txOffsetHz = 0;   // Phase 2 channels inside the capture
    site.bands.push_back(bt);
    // ---- two Phase 1 calls at once, a Phase 2 call after them
    {
        RigCfg rc; rc.rate = rate; rc.wideSlices = true; rc.follow.maxSlices = 2; rc.follow.allowRetune = true;
        Rig rig(rc);
        rig.world.noiseSnrDb = 28; rig.world.dcSpur = 0.2;
        Impair i1; i1.cfoHz = 300; Impair i2; i2.cfoHz = -500; i2.mpDelayUs = 30; i2.mpGain = 0.2; i2.mpFadeHz = 2;
        const std::vector<Grant> gr = {{b8.channel(0x20), 1201, 111}, {b8.channel(0x30), 1202, 222}};
        addVoice(rig.world, v1, 0x293, 1201, 111, 1.0, 5.0, false, i1);
        addVoice(rig.world, v2, 0x293, 1202, 222, 1.2, 5.0, false, i2);
        const std::vector<Grant> g2 = {{bt.channel(3), 3003, 333}};
        addControl(rig.world, ccHz, site, std::vector<GrantWin>{{gr, 1.0, 1.8}, {g2, 12.0, 12.8}}, {}, 1e9, i1);
        rig.world.add(phase2("p2 voice", bt.freq(3, 2), p2Call(site, 2, 3003, 333, 4.0), 12.0, 17.5, i1));
        rig.startWide(centre, ccHz);
        rig.runUntil(9.0);
        const trunk::CallRecord* a = rig.callForTg(1201);
        const trunk::CallRecord* b = rig.callForTg(1202);
        CHECK(a && a->followed && b && b->followed, "two slices: both calls followed (%d %d)", a ? a->followed : -1, b ? b->followed : -1);
        CHECK(rig.countActions(trunk::Action::OpenSlice) >= 2 || rig.maxSlicesOpen >= 2, "two slices: %d slices opened, %d at most at once", rig.countActions(trunk::Action::OpenSlice), rig.maxSlicesOpen);
        CHECK(rig.radio.retunes == 0 && rig.countActions(trunk::Action::Retune) == 0, "two slices: the radio never retuned (%d, %d retune actions)", rig.radio.retunes, rig.countActions(trunk::Action::Retune));
        CHECK(rig.wideVoiceReports > 10, "two slices: %llu voice reports from the bank", (unsigned long long)rig.wideVoiceReports);
        CHECK(a && b && a->src == 111 && b->src == 222 && std::fabs(a->freqHz - v1) < 1 && std::fabs(b->freqHz - v2) < 1, "two slices: units and frequencies");
        rig.runUntil(11.5);
        CHECK(a && b && !rig.callForTg(1201)->active && !rig.callForTg(1202)->active, "two slices: calls ended");
        CHECK(rig.countActions(trunk::Action::CloseSlice) >= 2, "two slices: %d slices closed", rig.countActions(trunk::Action::CloseSlice));
        CHECK(rig.ctl.follower.activeSlices() == 0, "two slices: none left open");
        // Phase 2 inside the capture
        rig.runUntil(19.0);
        const trunk::CallRecord* p = rig.callForTg(3003);
        CHECK(p && p->followed && p->slot == 1, "phase 2 inside: followed on slot %d", p ? p->slot : -9);
        CHECK(rig.p2VoiceReports > 5, "phase 2 inside: %llu voice reports", (unsigned long long)rig.p2VoiceReports);
        CHECK(rig.radio.retunes == 0, "phase 2 inside: still no retune");
        bool tdmaOpen = false;
        for (const auto& ac : rig.executed) if (ac.kind == trunk::Action::OpenSlice && ac.tdma) tdmaOpen = true;
        CHECK(tdmaOpen, "phase 2 inside: OpenSlice with tdma");
    }
    // ---- three granted calls, two slices: the third is missed with a reason
    {
        RigCfg rc; rc.rate = rate; rc.wideSlices = true; rc.follow.maxSlices = 2; rc.follow.allowRetune = false;
        Rig rig(rc);
        rig.world.noiseSnrDb = 30;
        const double v3 = b8.freq(0x28);
        const std::vector<Grant> gr = {{b8.channel(0x20), 1201, 111}, {b8.channel(0x30), 1202, 222}, {b8.channel(0x28), 1203, 333}};
        addControl(rig.world, ccHz, site, gr, 1.0, 1.8);
        addVoice(rig.world, v1, 0x293, 1201, 111, 1.0, 4.0);
        addVoice(rig.world, v2, 0x293, 1202, 222, 1.0, 4.0);
        addVoice(rig.world, v3, 0x293, 1203, 333, 1.0, 4.0);
        rig.startWide(centre, ccHz);
        rig.runUntil(4.0);
        int open = 0, missed = 0;
        for (int tg : {1201, 1202, 1203}) { const trunk::CallRecord* r = rig.callForTg(tg); if (r && r->followed) open++; if (r && r->missed) missed++; }
        CHECK(rig.maxSlicesOpen <= 2, "slice limit: %d open at most", rig.maxSlicesOpen);
        CHECK(open == 2 && missed == 1, "slice limit: %d followed, %d missed", open, missed);
    }
    // ---- a channel inside the edge guard is not 'inside'
    {
        trunk::Follower f;
        f.setCapture(centre, rate, true);
        CHECK(f.inSpan(centre + rate / 2 - 50000) && !f.inSpan(centre + rate / 2 - 5000), "edge guard");
    }
    return testSummary("p25 scenario slice");
}
