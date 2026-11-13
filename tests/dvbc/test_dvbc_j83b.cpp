// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// J.83 Annex B on simulated symbols: RS, interleaver modes, both QAM orders, noise, carrier quarter turns and the group phase.
#include "dect2/dvbc/j83b.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <map>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static void fill(uint8_t* p, uint64_t i) { static const int pids[4] = {0x31, 0x100, 0x101, 0x1FFF}; memset(p, 0, 188); const int pid = pids[(i * 7) % 4]; p[0] = 0x47; p[1] = pid >> 8; p[2] = pid & 255; static int cnt[4] = {}; p[3] = 0x10 | (cnt[(i * 7) % 4]++ & 15); for (int k = 4; k < 188; k++) p[k] = (uint8_t)(i * 31 + k * 17); }
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    {   // RS
        std::mt19937 r(1);
        for (int e = 0; e <= 3; e++) for (int t = 0; t < 200; t++) {
            uint8_t b[128], o[128]; for (int i = 0; i < 122; i++) b[i] = r() & 127;
            j83b::rsEncode(b); memcpy(o, b, 128);
            for (int k = 0; k < e; k++) b[r() % 128] ^= (r() % 127) + 1;
            int c = j83b::rsDecode(b);
            CHECK(c >= 0 && !memcmp(b, o, 128), "RS %d errors: %d", e, c);
        }
        uint8_t b[128]; for (int i = 0; i < 128; i++) b[i] = i; j83b::rsEncode(b); for (int k = 0; k < 5; k++) b[k * 7] ^= 3; CHECK(j83b::rsDecode(b) < 0, "RS 5 errors passed");
    }
    {   // constellations: quarter turns keep every bit except the two coded ones
        for (bool q : {false, true}) {
            const auto& p = j83b::points(q); const int mask = q ? 0x11 : 0x09; int bad = 0;
            for (size_t l = 0; l < p.size(); l++) { cf32 t(-p[l].imag(), p[l].real()); float bd = 9; size_t b = 0; for (size_t k = 0; k < p.size(); k++) if (std::norm(p[k] - t) < bd) { bd = std::norm(p[k] - t); b = k; } if ((l ^ b) & ~mask & (p.size() - 1)) bad++; }
            CHECK(bad == 0, "quarter turn changes %d %s labels", bad, q ? "256" : "64");
            CHECK(p.size() == (q ? 256u : 64u), "size");
        }
    }
    std::mt19937 rng(9); dect2::normal_distribution<float> nd(0.f, 1.f);
    struct C { bool q; int ctrl; double snr; int skip; int rot; bool swp; };
    for (const C c : std::initializer_list<C>{{false, 0, 30, 0, 0, false}, {false, 3, 22, 0, 1, false}, {false, 5, 26, 2, 2, false}, {false, 9, 28, 4, 3, false}, {false, 8, 24, 1, 0, true}, {false, 14, 23, 0, 0, false}, {true, 0, 32, 0, 0, false}, {true, 3, 30, 3, 1, false}, {true, 5, 28, 2, 2, false}, {true, 8, 30, 0, 3, true}, {true, 14, 27, 0, 0, false}}) {
        j83b::EncConfig ec; ec.qam256 = c.q; ec.control = c.ctrl; ec.swapAB = c.swp;
        j83b::J83bEncoder tx(ec);
        const int packets = c.q ? 12000 : 9000;
        std::vector<cf32> s;
        std::vector<uint8_t> sent(packets * 188);
        for (int i = 0; i < packets; i++) { uint8_t pk[188]; fill(pk, i); memcpy(&sent[i * 188], pk, 188); tx.push(pk); }
        tx.take(s);
        const float sg = (float)std::sqrt(std::pow(10.0, -c.snr / 10) / 2);
        cf32 rot = c.rot == 0 ? cf32(1, 0) : c.rot == 1 ? cf32(0, 1) : c.rot == 2 ? cf32(-1, 0) : cf32(0, -1);
        for (auto& v : s) v = v * rot + cf32(nd(rng) * sg, nd(rng) * sg);
        s.erase(s.begin(), s.begin() + c.skip);
        j83b::J83bDecoder rx; rx.configure(c.q);
        std::vector<uint8_t> got;
        rx.setPacketCallback([&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n * 188); });
        bool lock = false; size_t o = 0;
        while (o < s.size() && !lock) { const size_t k = std::min<size_t>(4096, s.size() - o); lock = rx.trial(s.data() + o, k); o += k; }
        CHECK(lock, "no lock: qam256 %d ctrl %d snr %.0f phase %d rot %d", c.q, c.ctrl, c.snr, c.skip, c.rot);
        if (!lock) continue;
        while (o < s.size()) { const size_t k = std::min<size_t>(4096, s.size() - o); rx.feed(s.data() + o, k); o += k; }
        const auto& st = rx.stats();
        // find: for each received packet, the sent index it equals (search around)
        int match = 0;
        {   // each received packet counted when it equals some sent packet (the sync slot apart)
            std::map<std::string, int> have;
            for (int k = 0; k < packets; k++) have[std::string((const char*)&sent[(size_t)k * 188] + 1, 187)] = 1;
            for (size_t i = 0; i + 188 <= got.size(); i += 188) if (have.count(std::string((const char*)&got[i] + 1, 187))) match++;
        }
        printf("qam%d ctrl %2d (I %d J %d) snr %.0f phase %d rot %d swap %d: frames %llu RS clean %llu fixed %llu failed %llu, bad frames %llu, packets %zu, %d match\n", c.q ? 256 : 64, c.ctrl, st.I, st.J, c.snr, c.skip, c.rot, c.swp, (unsigned long long)st.frames, (unsigned long long)st.rsClean, (unsigned long long)st.rsCorrected, (unsigned long long)st.rsFailed, (unsigned long long)st.framesBad, got.size() / 188, match);
        CHECK(st.control == c.ctrl, "control %d != %d", st.control, c.ctrl);
        CHECK(match > (int)(got.size() / 188 * 0.8) && got.size() > 188000, "qam256 %d ctrl %d: only %d of %zu packets matched", c.q, c.ctrl, match, got.size() / 188);
        CHECK(st.rsFailed < st.rsClean / 50, "too many RS failures (%llu)", (unsigned long long)st.rsFailed);
    }
    printf(fails ? "j83b FAILED\n" : "j83b passed\n");
    return fails ? 1 : 0;
}
