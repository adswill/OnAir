// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RDS-TMC: group 3A announcement, single-group and multi-group 8A messages, repeats, lost groups, and a full path through the FM generator and receiver.
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/fm/rds_tmc.h"
#include <cstdio>
#include <string>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint16_t hdr(int low) { return (uint16_t)(0x8000 | (1 << 10) | (10 << 5) | low); }   // group 8A, TP, PTY 10
static const uint16_t kB3A = (uint16_t)(0x3000 | (1 << 10) | (10 << 5) | 16);               // 3A announcing group 8A
static const uint16_t kC3A = (1 << 6) | (1 << 4);                                           // variant 0: LTN 1, AFI 0, enhanced mode

// bits "0000011..." packed into 28-bit pieces (zero padded)
static std::vector<uint32_t> pack(const std::string& bits, int pieces) {
    std::string s = bits;
    s.resize((size_t)pieces * 28, '0');
    std::vector<uint32_t> v;
    for (int i = 0; i < pieces; i++) { uint32_t x = 0; for (int j = 0; j < 28; j++) x = x << 1 | (uint32_t)(s[(size_t)i * 28 + j] - '0'); v.push_back(x); }
    return v;
}
static std::string bin(int v, int n) { std::string s; for (int i = n - 1; i >= 0; i--) s += (char)('0' + ((v >> i) & 1)); return s; }

struct G { uint16_t b, c, d; };
// the three groups of the test's multi-group message: stationary traffic (101), location 4321, extent 2, CI 5; duration 1 hour,
// speed limit 30 km/h, additional event slippery road (1003)
static std::vector<G> multi() {
    const std::string ff = bin(0, 4) + bin(3, 3) + bin(3, 4) + bin(6, 5) + bin(9, 4) + bin(1003, 11) + bin(14, 4);
    const auto p = pack(ff, 2);
    return {{hdr(5), (uint16_t)(0x8000 | (2 << 11) | 101), 4321},
            {hdr(5), (uint16_t)(0x4000 | (1 << 12) | (p[0] >> 16)), (uint16_t)(p[0] & 0xFFFF)},
            {hdr(5), (uint16_t)((0 << 12) | (p[1] >> 16)), (uint16_t)(p[1] & 0xFFFF)}};
}
static const G kSingle = {hdr(8 | 2), (uint16_t)((1 << 14) | (3 << 11) | 701), 12345};   // roadworks, 3 more locations, negative direction, persistence 2

static void checkMulti(const TmcMessage& m) {
    CHECK(m.event == 101 && m.text == "stationary traffic", "multi-group event %d '%s'", m.event, m.text.c_str());
    CHECK(m.location == 4321 && m.extent == 2 && !m.negative && m.multiGroup, "multi-group location %d extent %d", m.location, m.extent);
    CHECK(m.durationCode == 3 && m.durationText == "1 hour", "multi-group duration %d '%s'", m.durationCode, m.durationText.c_str());
    CHECK(m.more.size() == 2 && m.more[0] == "speed limit 30 km/h" && m.more[1] == "slippery road", "multi-group details (%zu)", m.more.size());
}
static void checkSingle(const TmcMessage& m) {
    CHECK(m.event == 701 && m.text == "roadworks" && m.location == 12345 && m.extent == 3 && m.negative && !m.multiGroup, "single-group message %d '%s' loc %d ext %d", m.event, m.text.c_str(), m.location, m.extent);
    CHECK(m.durationCode == 2 && m.durationText == "until tonight", "single-group duration '%s'", m.durationText.c_str());
}

static FmTelemetry runStream(double cnr, int blocks) {
    FmGenConfig c; c.rate = 2e6; c.cnrDb = cnr;
    std::vector<G> sent = multi();
    sent.insert(sent.begin(), kSingle);
    sent.insert(sent.begin(), G{kB3A, kC3A, 0xCD46});
    for (const G& g : sent) c.extraGroups.push_back({g.b, g.c, g.d});
    FmGenerator gen(c);
    FmReceiver rx; rx.setSilent(true); rx.configure(c.rate);
    std::vector<cf32> buf;
    FmTelemetry t; uint64_t seq = 0;
    for (int i = 0; i < blocks; i++) {
        buf.clear(); gen.generate(65536, buf); rx.feed(buf.data(), buf.size());
        FmTelemetry x; if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
    }
    return t;
}

int main() {
    const double t0 = 1.0e9;
    {   // announcement and messages
        TmcDecoder d;
        d.onGroup8A(kSingle.b, kSingle.c, kSingle.d, t0);   // before the announcement: not TMC yet
        CHECK(d.messages(t0).empty() && !d.active(), "a message before the 3A group was taken");
        d.onGroup3A(kB3A, kC3A, 0xCD46);
        const TmcSystem& s = d.system();
        CHECK(s.seen && s.aid == 0xCD46 && s.ltn == 1 && s.afi == 0 && s.mode == 1, "3A: seen %d ltn %d afi %d mode %d", s.seen, s.ltn, s.afi, s.mode);
        d.onGroup3A(kB3A, (uint16_t)(0x4000 | (21 << 6)), 0xCD46);
        CHECK(d.system().sid == 21 && d.system().ltn == 1, "3A variant 1: SID %d", d.system().sid);
        TmcDecoder other; other.onGroup3A(kB3A, kC3A, 0x4BD7);
        CHECK(!other.active(), "another ODA taken for TMC");
        d.onGroup8A(kSingle.b, kSingle.c, kSingle.d, t0);
        for (const G& g : multi()) d.onGroup8A(g.b, g.c, g.d, t0 + 1);
        auto m = d.messages(t0 + 2);
        CHECK(m.size() == 2, "%zu messages, expected 2", m.size());
        if (m.size() == 2) { checkMulti(m[0]); checkSingle(m[1]); CHECK(m[1].expires == t0 + 28800, "expiry %.0f", m[1].expires - t0); }
        // repeats are listed once; their time runs from the last reception
        d.onGroup8A(kSingle.b, kSingle.c, kSingle.d, t0 + 600);
        for (const G& g : multi()) d.onGroup8A(g.b, g.c, g.d, t0 + 600);
        m = d.messages(t0 + 601);
        CHECK(m.size() == 2, "repeats listed %zu times", m.size());
        if (m.size() == 2) CHECK(m[0].count == 2 && m[1].count == 2 && m[1].expires == t0 + 600 + 28800, "repeat count %d", m[1].count);
        // expiry: the multi-group message (1 hour) is gone after that, the roadworks stay
        m = d.messages(t0 + 600 + 3601);
        CHECK(m.size() == 1 && m[0].event == 701, "expiry left %zu messages", m.size());
        // a lost group never makes a message
        TmcDecoder e; e.onGroup3A(kB3A, kC3A, 0xCD47);
        const auto g = multi();
        e.onGroup8A(g[0].b, g[0].c, g[0].d, t0); e.onGroup8A(g[2].b, g[2].c, g[2].d, t0);   // the second group is missing
        e.onGroup8A(g[1].b, g[1].c, g[1].d, t0); e.onGroup8A(g[1].b, g[1].c, g[1].d, t0);   // a second group twice
        CHECK(e.messages(t0).empty(), "a message from a lost group");
        TmcDecoder f; f.onGroup3A(kB3A, kC3A, 0xCD46);
        f.onGroup8A(g[1].b, g[1].c, g[1].d, t0); f.onGroup8A(g[2].b, g[2].c, g[2].d, t0);   // the first group is missing
        CHECK(f.messages(t0).empty(), "a message without its first group");
        f.onGroup8A(hdr(16 | 8 | 1), kSingle.c, kSingle.d, t0);   // tuning information
        f.onGroup8A(hdr(8), 701, 0, t0);                           // location 0
        CHECK(f.messages(t0).empty(), "tuning information or location 0 listed");
    }
    {   // through the generator, the receiver and the RDS decoder; the groups come round again and again
        const FmTelemetry t = runStream(45, 300);
        CHECK(t.tmc.seen && t.tmc.ltn == 1 && t.tmc.mode == 1, "receiver: TMC not announced");
        CHECK(t.tmcMessages.size() == 2, "receiver: %zu messages (groups %llu, first event %d)", t.tmcMessages.size(), (unsigned long long)t.rdsGroups, t.tmcMessages.empty() ? 0 : t.tmcMessages[0].event);
        if (t.tmcMessages.size() == 2) {
            checkMulti(t.tmcMessages[0]); checkSingle(t.tmcMessages[1]);
            CHECK(t.tmcMessages[0].count >= 2, "receiver: message not repeated (%d)", t.tmcMessages[0].count);
        }
        CHECK(t.psName == "TESTFM  ", "PS '%s' with TMC groups in the stream", t.psName.c_str());
    }
    {   // a noisy channel: blocks fail, and nothing but the sent messages may be listed
        const FmTelemetry t = runStream(17, 600);
        printf("noisy: blocks ok %.0f%%, groups %llu, TMC %d, messages %zu\n", t.rdsBlockOkPct, (unsigned long long)t.rdsGroups, t.tmc.seen, t.tmcMessages.size());
        for (const TmcMessage& m : t.tmcMessages)
            CHECK((m.event == 701 && m.location == 12345) || (m.event == 101 && m.location == 4321), "false message: event %d location %d", m.event, m.location);
        CHECK(t.tmcMessages.size() <= 2, "more messages than were sent (%zu)", t.tmcMessages.size());
    }
    printf(fails ? "tmc: %d FAILED\n" : "tmc: all passed\n", fails);
    return fails ? 1 : 0;
}
