// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN network health (lorawan_health.h): two simulated days of 30 end devices with reception loss, one device losing half its
// uplinks, a 16-bit counter wrap, a device reset, repeated frames (NbTrans 2), CRC failures, a device that cannot join, an hour of join
// requests without accepts, and the network falling back to SF12 at the end. Simulated only: no real LoRaWAN capture was used.
#include "dect2/lorawan/lorawan_health.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static int kinds(const std::vector<LorawanHealth::Finding>& v, const char* k) { int n = 0; for (const auto& f : v) if (f.kind == k) n++; return n; }

int main() {
    for (unsigned seed = 1; seed <= 3; seed++) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
        LorawanHealth h;
        std::vector<LorawanHealth::Finding> out;
        const double t0 = 1767225600, end = t0 + 2 * 86400;
        struct Dev { std::string addr; long fcnt; double loss; int sf; bool repeat; };
        std::vector<Dev> dev;
        for (int i = 0; i < 30; i++) { char b[12]; snprintf(b, sizeof b, "260B%04X", i); dev.push_back({b, i == 7 ? 65400 : (long)(u(r) * 500), i == 3 ? 0.5 : 0.05, 7 + i % 4, i == 5}); }
        std::vector<std::pair<double, LwHealthFrame>> frames;
        for (double t = t0; t < end; t += 600) {
            const bool slow = t >= end - 4 * 3600;
            for (auto& d : dev) {
                const double tt = t + u(r) * 500;
                if (d.addr == "260B0009" && std::fabs(t - (t0 + 86400)) < 1) d.fcnt = 0;   // the device was reset
                d.fcnt = (d.fcnt + 1) & 0xFFFF;
                const int nTx = d.repeat ? 2 : 1;
                for (int k = 0; k < nTx; k++) {
                    if (u(r) < d.loss) continue;
                    LwHealthFrame f; f.t = tt + k; f.mtype = 2; f.devAddr = d.addr; f.fcnt = d.fcnt; f.sf = slow ? 12 : d.sf; f.crcOk = u(r) > 0.02;
                    frames.push_back({f.t, f});
                }
            }
            // normal joins (a request, then an accept) now and then
            if (u(r) < 0.1) {
                LwHealthFrame j; j.t = t + 10; j.mtype = 0; j.devEui = "70B3D57ED0000001"; j.sf = 10; frames.push_back({j.t, j});
                LwHealthFrame a; a.t = t + 15; a.mtype = 1; a.sf = 10; frames.push_back({a.t, a});
            }
        }
        // a device that cannot join: a request every 4 minutes for 28 minutes, from 06:00 on the first day
        for (double t = t0 + 6 * 3600; t < t0 + 6 * 3600 + 1700; t += 240) { LwHealthFrame j; j.t = t; j.mtype = 0; j.devEui = "A84041000000BEEF"; j.sf = 12; frames.push_back({t, j}); }
        // an hour with many requests and no accept at all (12:00 - 13:00 on the second day)
        for (int i = 0; i < 14; i++) { LwHealthFrame j; j.t = t0 + 86400 + 12 * 3600 + i * 240; j.mtype = 0; j.devEui = "70B3D57ED00001" + std::to_string(10 + i); j.sf = 9; frames.push_back({j.t, j}); }
        std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [t, f] : frames) {
            if (f.mtype == 1 && t > t0 + 86400 + 12 * 3600 - 600 && t < t0 + 86400 + 13 * 3600 + 600) continue;   // no accepts around that hour
            h.add(f, out);
        }
        const auto* x = h.device("260B0003");
        CHECK(x && std::fabs(x->lossPct() - 50) < 8, "seed %u: loss of the bad device %.1f", seed, x ? x->lossPct() : -1);
        int fine = 0;
        for (const auto& d : h.devices()) if (d.id.rfind("260B", 0) == 0 && d.id != "260B0003" && d.lossPct() < 15) fine++;
        CHECK(fine == 29, "seed %u: %d devices with low loss", seed, fine);
        const auto* w = h.device("260B0007");
        CHECK(w && w->restarts == 0 && w->lossPct() < 10, "seed %u: wrap counted as restart %u / loss %.1f", seed, w ? w->restarts : 99, w ? w->lossPct() : -1);
        const auto* rs = h.device("260B0009");
        CHECK(rs && rs->restarts == 1, "seed %u: reset device restarts %u", seed, rs ? rs->restarts : 0);
        const auto* rp = h.device("260B0005");
        CHECK(rp && rp->repeats > 150 && rp->lossPct() < 10, "seed %u: repeats %u", seed, rp ? rp->repeats : 0);
        CHECK(kinds(out, "lorawan_loss") == 1, "seed %u: %d loss alerts", seed, kinds(out, "lorawan_loss"));
        CHECK(kinds(out, "lorawan_join_storm") == 1, "seed %u: %d join storm alerts", seed, kinds(out, "lorawan_join_storm"));
        CHECK(kinds(out, "lorawan_no_accepts") == 1, "seed %u: %d no-accept alerts", seed, kinds(out, "lorawan_no_accepts"));
        CHECK(kinds(out, "lorawan_sf_shift") == 1, "seed %u: %d SF shift alerts", seed, kinds(out, "lorawan_sf_shift"));
        const auto mixDay = h.sfMix(end - 5 * 3600, 20), mixNow = h.sfMix(end, 2);
        CHECK(mixNow[12] > 0.95 && mixDay[7] > 0.2 && mixDay[12] < 0.05, "seed %u: SF mix %.2f / %.2f / %.2f", seed, mixNow[12], mixDay[7], mixDay[12]);
    }
    return testSummary("lorawan_health");
}
