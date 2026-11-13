// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #454: the multi-programme mux of the TV transmitters, through the real chain: TxRequest -> makeTxGenerator -> IQ at the radio's rate -> receiver -> transport stream.
// PAT / SDT carry every programme, and the stream's own clock (PCR) runs at the speed of the signal: the padding must fit the channel's real net bit rate.
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/t2/t2gen.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/tx/tx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

struct TsStats { std::set<int> programmes; std::set<std::string> names; std::map<int, uint64_t> pids; double pcrSec = 0; uint64_t packets = 0; };

static void parseSections(const uint8_t* p, TsStats& s) {
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    if (!(p[1] & 0x40) || (pid != 0 && pid != 0x11)) return;
    const uint8_t* q = p + 4 + (p[3] & 0x20 ? 1 + p[4] : 0);
    q += 1 + q[0];
    const int len = ((q[1] & 15) << 8) | q[2];
    if (q + 3 + len > p + 188) return;
    if (pid == 0 && q[0] == 0) { for (int i = 8; i + 4 <= 3 + len - 4; i += 4) { const int n = (q[i] << 8) | q[i + 1]; if (n) s.programmes.insert(n); } }
    if (pid == 0x11 && q[0] == 0x42) {
        for (int i = 11; i + 5 <= 3 + len - 4;) {
            const int dl = ((q[i + 3] & 15) << 8) | q[i + 4];
            for (int d = i + 5; d + 2 <= i + 5 + dl; d += 2 + q[d + 1]) if (q[d] == 0x48) { const int pl = q[d + 3]; s.names.insert(std::string((const char*)q + d + 5 + pl, q[d + 4 + pl])); }
            i += 5 + dl;
        }
    }
}

static TsStats runDtmb(int mapOpt, int rateOpt, double seconds) {
    const TxModeSpec* m = txModeById("dtmb");
    TxRequest r; r.modeId = "dtmb"; r.serviceId = 100;
    DemoTsService a; a.name = "Programme A"; DemoTsService b; b.name = "Programme B"; b.picture = 1;
    r.mux = {a, b};
    r.synth.modeOpt[1] = mapOpt; r.synth.modeOpt[2] = rateOpt;
    TsStats st;
    const double fs = 10e6;
    std::string err;
    auto g = makeTxGenerator(*m, r, fs, err);
    CHECK(g, "generator: %s", err.c_str());
    if (!g) return st;
    DtmbReceiver rx;
    rx.configure(fs, 8.0);
    int64_t firstPcr = -1, lastPcr = -1; int pcrPid = -1;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* k = p + i * 188; st.packets++;
            const int pid = ((k[1] & 0x1F) << 8) | k[2]; st.pids[pid]++;
            parseSections(k, st);
            if ((k[3] & 0x20) && k[4] >= 7 && (k[5] & 0x10)) {
                if (pcrPid < 0) pcrPid = pid;
                if (pid == pcrPid) { const int64_t c = ((int64_t)k[6] << 25) | (k[7] << 17) | (k[8] << 9) | (k[9] << 1) | (k[10] >> 7); if (firstPcr < 0) firstPcr = c; lastPcr = c; }
            }
        }
    });
    std::vector<cf32> buf(1 << 16);
    for (double t = 0; t < seconds; t += buf.size() / fs) { g->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    if (firstPcr >= 0) st.pcrSec = (double)(lastPcr - firstPcr) / 90000.0;
    return st;
}

static void t2Rate(int plpMod, int plpCod) {   // the bit rate the generator reports is the number of packets it really takes
    TxParams tp; tp.payload = true; tp.plpMod = plpMod; tp.plpCod = plpCod;
    uint64_t pulled = 0;
    tp.tsSource = [&](uint8_t* p) { memset(p, 0, 188); p[0] = 0x47; pulled++; };
    T2Generator g(tp);
    const double native = 64e6 / 7;
    std::vector<cf32> f;
    size_t samples = 0;
    for (int i = 0; i < 20; i++) { g.nextFrame(f); samples += f.size(); }
    const double real = (double)pulled * 1504.0 / ((double)samples / native);
    printf("T2 mod %d cod %d: %.3f Mbit/s reported, %.3f taken\n", plpMod, plpCod, g.tsBitrate(native) / 1e6, real / 1e6);
    CHECK(std::fabs(g.tsBitrate(native) - real) < 0.02 * real, "T2 bit rate %.0f, packets taken at %.0f", g.tsBitrate(native), real);
    CHECK(std::fabs(g.tsBitrate(native) - 24e6) > 1e6 || plpMod == 3, "this PLP is not the 24 Mbit/s the old code assumed");
}

int main() {
    // QPSK 0.4 carries about 5.4 Mbit/s: with the old fixed 20 Mbit/s padding the PCR ran at a fifth of the signal
    for (int k = 0; k < 3; k++) {
        const int maps[3] = {0, 3, 2}, rates[3] = {0, 1, 2};
        const double dur = 6.0;
        TsStats s = runDtmb(maps[k], rates[k], dur);
        printf("DTMB map %d rate %d:", maps[k], rates[k]);
        for (int p : s.programmes) printf(" %d", p);
        for (auto& n : s.names) printf(" [%s]", n.c_str());
        printf(", %llu packets, PCR span %.2f s of %.1f s\n", (unsigned long long)s.packets, s.pcrSec, dur);
        CHECK(s.programmes.size() == 2 && s.programmes.count(100) && s.programmes.count(101), "both programmes in the PAT");
        CHECK(s.names.count("Programme A") && s.names.count("Programme B"), "both names in the SDT");
        CHECK(s.pids.count(0x100) && s.pids.count(0x101) && s.pids.count(0x110) && s.pids.count(0x111), "video and audio PIDs of both");
        CHECK(s.pcrSec > 0.85 * (dur - 1.2) && s.pcrSec < 1.02 * dur, "PCR span %.2f s in %.1f s of signal: wrong speed", s.pcrSec, dur);
    }
    t2Rate(2, 2); t2Rate(0, 0); t2Rate(1, 3);
    return testSummary("tx_mux");
}
