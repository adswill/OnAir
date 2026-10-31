// The real receiver on a synthetic control channel (p25_scenario.h): C4FM from the generator, impaired, through P25Receiver, the TSBK telemetry and the
// Controller. Band plans of 700 / 800 MHz (12.5 kHz), VHF / UHF (identifier update VU) and a TDMA identifier: the site ids, the identifier maths
// (base + channel * spacing, TDMA carrier and slot) and the frequency of every grant, clean and with a carrier offset, symbol clock error, noise,
// two-ray fading and a DC spur. NOT covered: a real transmitter.
#include "p25_scenario.h"
#include <cstdio>
using namespace dect2;
using namespace p25sim;
#include "../common/test_util.h"

struct Profile { const char* name; Impair imp; double snr; double dc; double ppm; };

int main() {
    const double ccHz = band800().freq(1);
    const Band b8 = band800(), b7 = band700(), bu = bandUhf(), bv = bandVhf(), bt = bandTdma();
    const Profile profiles[] = {
        {"clean", Impair(), 40, 0, 0},
        {"cfo +1.2 kHz, clock +40 ppm, 22 dB", [] { Impair i; i.cfoHz = 1200; i.clockPpm = 40; return i; }(), 22, 0.3, 0},
        {"cfo -900 Hz, fading, tuner 2 ppm, 24 dB", [] { Impair i; i.cfoHz = -900; i.mpDelayUs = 40; i.mpGain = 0.25; i.mpFadeHz = 3; return i; }(), 24, 0.2, 2},
    };
    for (const Profile& pr : profiles) {
        RigCfg rc; rc.rate = 480000;
        Rig rig(rc);
        rig.world.noiseSnrDb = pr.snr; rig.world.dcSpur = pr.dc; rig.world.tunerPpm = pr.ppm;
        CcSpec cc; cc.bands = {b8, b7, bu, bv, bt};
        const std::vector<Grant> gr = {{b8.channel(0x10), 1201, 1111}, {b7.channel(0x20), 1202, 2222}, {bu.channel(0x30), 1203, 3333}, {bv.channel(0x08), 1204, 4444}, {bt.channel(5), 1205, 5555}};
        addControl(rig.world, ccHz, cc, gr, 0, 1e9, {}, 1e9, pr.imp);
        rig.start(ccHz);
        rig.ctl.follower.setConfig([&] { auto f = rig.ctl.follower.config(); f.allowRetune = false; return f; }());
        rig.runUntil(8.0);
        const trunk::SiteInfo& s = rig.ctl.sys.site();
        CHECK(s.valid && s.wacn == cc.wacn && s.sysId == cc.sysId && s.rfss == cc.rfss && s.site == cc.site && s.nac == cc.nac, "%s: site %05X %03X %d-%d nac %03X", pr.name, s.wacn, s.sysId, s.rfss, s.site, s.nac);
        CHECK(rig.ctl.sys.adjacent().size() == 1 && rig.ctl.sys.adjacent()[0].site == cc.site + 1, "%s: adjacent site", pr.name);
        for (const Band& b : {b8, b7, bu, bv, bt}) {
            const trunk::IdenInfo& ii = rig.ctl.sys.iden(b.id);
            CHECK(ii.valid && std::fabs(ii.baseHz - b.baseHz) < 10 && std::fabs(ii.spacingHz - b.spacingHz) < 1 && std::fabs(ii.txOffsetHz - b.txOffsetHz) < 1000, "%s: identifier %d base %.5f MHz spacing %.1f offset %.3f", pr.name, b.id, ii.baseHz / 1e6, ii.spacingHz, ii.txOffsetHz / 1e6);
        }
        CHECK(std::fabs(rig.ctl.sys.channelFreq(b8.channel(0x10)) - b8.freq(0x10)) < 1, "%s: 800 MHz channel", pr.name);
        CHECK(std::fabs(rig.ctl.sys.channelFreq(b7.channel(0x20)) - b7.freq(0x20)) < 1, "%s: 700 MHz channel", pr.name);
        CHECK(std::fabs(rig.ctl.sys.channelFreq(bu.channel(0x30)) - bu.freq(0x30)) < 1, "%s: UHF channel", pr.name);
        CHECK(std::fabs(rig.ctl.sys.channelFreq(bv.channel(0x08)) - bv.freq(0x08)) < 1, "%s: VHF channel", pr.name);
        int slot = -1; bool tdma = false;
        const double ft = rig.ctl.sys.channelFreq(bt.channel(5), &slot, &tdma);
        CHECK(tdma && slot == 1 && std::fabs(ft - bt.freq(5, 2)) < 1, "%s: TDMA channel %.5f MHz slot %d", pr.name, ft / 1e6, slot);
        struct E { int tg; double hz; bool tdma; } exp[] = {{1201, b8.freq(0x10), false}, {1202, b7.freq(0x20), false}, {1203, bu.freq(0x30), false}, {1204, bv.freq(0x08), false}, {1205, bt.freq(5, 2), true}};
        for (const E& e : exp) {
            const trunk::CallRecord* r = rig.callForTg(e.tg);
            CHECK(r && std::fabs(r->freqHz - e.hz) < 1, "%s: grant of TG %d at %.5f MHz in the call log (%.5f)", pr.name, e.tg, e.hz / 1e6, r ? r->freqHz / 1e6 : 0.0);
        }
        CHECK(rig.ctl.status(rig.now).crcOkPct > 70, "%s: control message success %.0f %%", pr.name, rig.ctl.status(rig.now).crcOkPct);
        printf("  %s: %llu TSBKs, success %.0f %%\n", pr.name, (unsigned long long)rig.tsbksFed, rig.ctl.status(rig.now).crcOkPct);
    }
    return testSummary("p25 scenario cc");
}
