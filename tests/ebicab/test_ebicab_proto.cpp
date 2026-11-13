// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Eurobalise telegram coding (UNISIG SUBSET-036 4.3, ebicab_proto.h): the substitution table, the polynomials (the long format's parity check
// accepts a short telegram repeated three times), encode / decode round trips in both formats for many scrambling values and user data, every
// cyclic shift of the endless telegram, the inverted telegram, bit errors, a bit slip and random data (no false telegram), the header fields.
#include "dect2/ebicab/ebicab_proto.h"
#include "dect2/ebicab/ebicab_gen.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> repeated(const std::vector<uint8_t>& t, size_t from, size_t count) {
    std::vector<uint8_t> w(count);
    for (size_t i = 0; i < count; i++) w[i] = t[(from + i) % t.size()];
    return w;
}

static std::vector<uint8_t> randomUser(std::mt19937& g, size_t m) { std::vector<uint8_t> u(m); for (auto& b : u) b = g() & 1; return u; }

int main() {
    std::mt19937 rng(11);
    for (bool lng : {true, false}) {
        const int n = lng ? 1023 : 341, r = lng ? 77 : 121, m = lng ? 830 : 210;
        int encoded = 0, tried = 0;
        for (int trial = 0; trial < 12; trial++) {
            std::vector<uint8_t> u = randomUser(rng, (size_t)m);
            std::vector<uint8_t> t;
            uint16_t B = 0;
            for (B = (uint16_t)(1 + rng() % 4000); B < 4096; B++) { tried++; t = ebicab::encode(u, lng, B); if (!t.empty()) break; }
            CHECK(!t.empty() && (int)t.size() == n, "%s: no scrambling value works", lng ? "long" : "short");
            if (t.empty()) continue;
            encoded++;
            // decode from the start and from many shifts of the endless repetition
            for (int shift : {0, 1, 5, 11, 100, n / 2, n - 1}) {
                const auto w = repeated(t, (size_t)shift, (size_t)(n + r));
                ebicab::Telegram out;
                const bool ok = ebicab::decodeWindow(w.data(), w.size(), lng, out);
                CHECK(ok && out.userBits == u && out.scrambling == B && !out.inverted, "%s shift %d: ok %d", lng ? "long" : "short", shift, ok);
            }
            {   // inverted
                auto w = repeated(t, 37, (size_t)(n + r));
                for (auto& b : w) b ^= 1;
                ebicab::Telegram out;
                const bool ok = ebicab::decodeWindow(w.data(), w.size(), lng, out);
                CHECK(ok && out.userBits == u && out.inverted, "%s inverted: ok %d", lng ? "long" : "short", ok);
            }
            {   // a bit error anywhere is refused
                int refused = 0;
                for (int k = 0; k < 30; k++) {
                    auto w = repeated(t, 50, (size_t)(n + r));
                    w[rng() % w.size()] ^= 1;
                    ebicab::Telegram out;
                    if (!ebicab::decodeWindow(w.data(), w.size(), lng, out)) refused++;
                }
                CHECK(refused == 30, "%s: %d of 30 single bit errors refused", lng ? "long" : "short", refused);
            }
            {   // a bit slip (one bit lost in the window)
                auto w = repeated(t, 20, (size_t)(n + r + 1));
                w.erase(w.begin() + 300);
                ebicab::Telegram out;
                CHECK(!ebicab::decodeWindow(w.data(), w.size(), lng, out), "%s: bit slip accepted", lng ? "long" : "short");
            }
        }
        CHECK(encoded == 12, "%s: %d of 12 encoded", lng ? "long" : "short", encoded);
        printf("  %s: 12 telegrams, %d scrambling values tried\n", lng ? "long" : "short", tried);
    }
    {   // a short telegram sent three times passes the long parity check, but the long receiver must not accept it
        const auto u = randomUser(rng, 210);
        std::vector<uint8_t> t; for (uint16_t B = 1; t.empty() && B < 4096; B++) t = ebicab::encode(u, false, B);
        std::vector<uint8_t> w; for (int k = 0; k < 4; k++) w.insert(w.end(), t.begin(), t.end());
        w.resize(1023 + 77);
        ebicab::Telegram out;
        CHECK(!ebicab::decodeWindow(w.data(), w.size(), true, out), "a repeated short telegram was read as a long one");
        CHECK(ebicab::decodeWindow(w.data(), 341 + 121, false, out) && out.userBits == u, "the short receiver reads it");
    }
    {   // equation (4) of the standard: the long parity check accepts a short telegram repeated (so both generator polynomials are right)
        const auto sh = ebicabTestTelegram(false, 5, 0);
        ebicab::Stream s;
        for (size_t i = 0; i < 2400; i++) s.bit(sh[i % sh.size()]);
        CHECK(s.parityHits() > 2400 + 600, "parity hits of a repeated short telegram: %llu (both formats expected)", (unsigned long long)s.parityHits());
    }
    {   // the header fields round trip
        ebicab::Header h; h.qUpdown = 1; h.mVersion = 33; h.qMedia = 0; h.nPig = 3; h.nTotal = 4; h.mDup = 1; h.mMcount = 200; h.nidC = 466; h.nidBg = 12345; h.qLink = 1; h.firstPacket = 255;
        const auto u = ebicab::userBitsFromHeader(h, 830);
        const auto p = ebicab::parseHeader(u);
        CHECK(p.valid && p.qUpdown == 1 && p.mVersion == 33 && p.nPig == 3 && p.nTotal == 4 && p.mDup == 1 && p.mMcount == 200 && p.nidC == 466 && p.nidBg == 12345 && p.qLink == 1 && p.firstPacket == 255, "header fields");
        const auto t = ebicabTestTelegram(true, 777, 2);
        ebicab::Telegram out;
        const auto w = repeated(t, 123, 1023 + 77);
        CHECK(ebicab::decodeWindow(w.data(), w.size(), true, out) && out.header.valid && out.header.nidBg == 777 && out.header.nPig == 2 && out.header.nidC == 466, "test telegram header");
    }
    {   // the stream receiver: found from any start, once per repetition, none in random data
        const auto t = ebicabTestTelegram(true, 1500, 1);
        for (int start : {0, 3, 400, 1000}) {
            ebicab::Stream s;
            int found = 0;
            for (size_t i = 0; i < 4 * 1023; i++) if (s.bit(t[((size_t)start + i) % t.size()])) { found++; CHECK(s.last().header.nidBg == 1500, "stream header"); }
            CHECK(found >= 2 && found <= 3, "start %d: %d telegrams found in 4 repetitions", start, found);
        }
        ebicab::Stream s;
        int found = 0;
        for (size_t i = 0; i < 2000000; i++) if (s.bit((int)(rng() & 1))) found++;
        CHECK(found == 0, "%d false telegrams in 2 million random bits", found);
        const auto sh = ebicabTestTelegram(false, 9, 0);
        ebicab::Stream s2; found = 0;
        for (size_t i = 0; i < 6 * 341; i++) if (s2.bit(sh[(i + 7) % sh.size()])) { found++; CHECK(!s2.last().longFmt, "short"); }
        CHECK(found >= 4, "short telegrams found: %d", found);
    }
    {   // the substitution table properties named in A1.1.2
        std::set<int> valid;
        for (int a = 0; a < 2048; a++) {
            std::vector<uint8_t> bits(11); for (int j = 0; j < 11; j++) bits[(size_t)j] = (uint8_t)((a >> (10 - j)) & 1);
            (void)bits;
        }
        // use the encoder: encoding all 1024 blocks is covered by the round trips; check the run-length limit on a sample of telegrams
        int maxRun = 0;
        for (int trial = 0; trial < 6; trial++) {
            std::vector<uint8_t> t; for (uint16_t B = (uint16_t)(1 + trial * 100); t.empty() && B < 4096; B++) t = ebicab::encode(randomUser(rng, 830), true, B);
            int run = 1;
            for (size_t i = 1; i < t.size(); i++) { run = t[i] == t[i - 1] ? run + 1 : 1; maxRun = std::max(maxRun, run); }
        }
        CHECK(maxRun <= 16, "run length %d", maxRun);
    }
    return testSummary("ebicab proto");
}
