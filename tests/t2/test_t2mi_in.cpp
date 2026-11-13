// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// T2-MI ingest: baseband frames of a T2 test generator packed as T2-MI in TS packets, mixed with other PIDs, and two PLPs. The inner transport streams must come back whole.
// Simulated only.
#include "dect2/t2/t2mi_in.h"
#include "dect2/t2/t2gen.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::vector<uint8_t> pack(const std::vector<uint8_t>& bits) { std::vector<uint8_t> o(bits.size() / 8, 0); for (size_t i = 0; i < bits.size() / 8; i++) for (int k = 0; k < 8; k++) o[i] |= (uint8_t)(bits[i * 8 + (size_t)k] << (7 - k)); return o; }
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int interleave : {0, 1}) {
        struct Src { TxParams p; std::unique_ptr<T2Generator> g; int plp; int pid; } s[2];
        uint64_t count = 0;
        auto ts = [&](uint8_t* p) { memset(p, 0, 188); p[0] = 0x47; p[1] = 0x01; p[2] = 0x23; p[3] = 0x10 | (count++ & 15); for (int k = 4; k < 188; k++) p[k] = (uint8_t)(count * 7 + k); };
        s[0].p.dataSymbols = 20; s[0].p.payload = true; s[0].p.plpTi = 0; s[0].p.tsSource = ts; s[0].plp = 0; s[0].p.plpMod = 1; s[0].p.plpCod = 2;
        s[1].p = s[0].p; s[1].plp = 5; s[1].p.plpId = 5; s[1].p.seed = 7;
        s[0].g.reset(new T2Generator(s[0].p)); s[1].g.reset(new T2Generator(s[1].p));
        std::vector<uint8_t> outer; uint8_t cc = 0, sf = 0, cnt = 0;
        std::vector<cf32> tmp;
        for (int fr = 0; fr < 6; fr++) {
            for (int i = 0; i < 2; i++) {
                if (interleave == 0 && i == 1 && fr % 2 == 0) continue;
                s[i].g->nextFrame(tmp);
                for (auto& bb : s[i].g->lastBbFrames()) { auto pk = t2mi::bbPacket(cnt++, sf, (uint8_t)fr, (uint8_t)s[i].plp, true, pack(bb)); t2mi::toTs(pk, 0x1000, cc, outer); }
            }
            uint8_t o[188]; memset(o, 0xA5, 188); o[0] = 0x47; o[1] = 0x42; o[2] = 0x22; o[3] = 0x10; outer.insert(outer.end(), o, o + 188);   // a PES-like other PID
        }
        std::map<int, uint64_t> got;
        T2miIngest in; in.setSink([&](int plp, const uint8_t* p) { CHECK(p[0] == 0x47, "sync"); got[plp]++; });
        for (size_t i = 0; i + 188 <= outer.size(); i += 188) in.feed(outer.data() + i);
        printf("interleave %d: pid 0x%X, plps %zu, packets plp0 %llu plp5 %llu, lost %llu/%llu\n", interleave, in.pid(), in.plps().size(), (unsigned long long)in.packets(0), (unsigned long long)in.packets(5), (unsigned long long)in.framesLost(0), (unsigned long long)in.framesLost(5));
        CHECK(in.found() && in.pid() == 0x1000, "pid");
        CHECK(got[0] > 40 && got[5] > 40, "packets %llu %llu", (unsigned long long)got[0], (unsigned long long)got[5]);
    }
    printf(fails ? "t2mi in FAILED\n" : "t2mi in passed\n");
    return fails ? 1 : 0;
}
