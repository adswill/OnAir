// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The trunking follower fed by the real receiver: the synthetic control channel (identifier update, network / RFSS / adjacent status, voice grants)
// goes through the C4FM demodulator, the TSBK decoder and the telemetry queue into the Controller. Silent.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    P25GenConfig c;
    c.rate = 500000; c.snrDb = 25; c.cfoHz = 300; c.loop = false;
    P25GenTx tx;
    tx.kind = P25GenTx::Control;
    tx.durSec = 4;
    c.txs.push_back(tx);
    P25Generator g(c);
    P25Receiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    trunk::Controller ctl;
    ctl.setCapture(851.1e6, 2.4e6);
    std::set<uint64_t> seen;
    uint64_t lastSeq = 0;
    std::vector<cf32> buf(25000);
    double t = 0;
    for (double s = 0; s < tx.durSec + 1.5; s += buf.size() / c.rate) {
        g.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        P25Telemetry tel;
        if (rx.telemetry(tel, lastSeq)) {
            lastSeq = tel.seq;
            for (const auto& r : tel.tsbks) {
                if (!seen.insert(r.seq).second) continue;
                p25::Tsbk k;
                k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
                ctl.onTsbk(k, r.nac, t);
            }
        }
        t += buf.size() / c.rate;
        ctl.tick(t);
    }
    const trunk::SiteInfo& s = ctl.sys.site();
    printf("  %zu TSBKs, site %s, %zu adjacent, %zu calls, state %s\n", seen.size(), s.valid ? "yes" : "no", ctl.sys.adjacent().size(), ctl.log.records().size(), trunk::followStateName(ctl.follower.state()));
    CHECK(seen.size() > 10, "%zu TSBKs", seen.size());
    CHECK(s.valid && s.wacn == tx.wacn && s.sysId == tx.sysId && s.rfss == tx.rfss && s.site == tx.site && s.nac == tx.nac, "site ids");
    CHECK(ctl.sys.adjacent().size() == 1 && ctl.sys.adjacent()[0].site == tx.adjSite, "adjacent site");
    CHECK(ctl.sys.iden(1).valid && std::fabs(ctl.sys.iden(1).baseHz - tx.idenBaseHz) < 10, "identifier update");
    bool found = false;
    for (const auto& r : ctl.log.records())
        if (r.tg == tx.grantTg && r.src == tx.grantSrc && std::fabs(r.freqHz - (tx.idenBaseHz + 0x10 * 12500.0)) < 10) found = true;
    CHECK(found, "grant of TG %d on %.5f MHz in the call log", tx.grantTg, (tx.idenBaseHz + 0x10 * 12500.0) / 1e6);
    CHECK(ctl.status(t).tsbkGood > 10 && ctl.status(t).crcOkPct > 80, "success rate %.0f %%", ctl.status(t).crcOkPct);
    return testSummary("p25 trunk rf");
}
