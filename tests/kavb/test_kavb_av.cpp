// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB video / audio layer: packets, the GF(256) erasure code, fragmentation, packetiser / depacketiser with cell and frame loss, bit budget per profile, and the
// whole chain (synthetic AV payload -> packetiser -> PHY -> channel -> receiver -> depacketiser) for one second of profile A.
#include "kavb_test_util.h"
#include "dect2/kavb/kavb_av.h"
#include <set>
using namespace dect2::kavb;
using namespace kavbtest;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void testPacket() {
    AvPacket p;
    p.type = PktType::Audio; p.stream = 3; p.seq = 0xBEEF;
    p.payload = {1, 2, 3, 4, 5};
    auto d = serializePacket(p);
    CHECK(d.size() == 5 + kPacketOverhead, "size %zu", d.size());
    AvPacket q;
    CHECK(parsePacket(d.data(), d.size(), q) == (int)d.size(), "parse");
    CHECK(q.type == PktType::Audio && q.stream == 3 && q.seq == 0xBEEF && q.payload == p.payload, "fields");
    CHECK(parsePacket(d.data(), d.size() - 1, q) == 0, "short = need more");
    for (size_t i = 0; i < d.size(); i++) {
        auto e = d;
        e[i] ^= 0x10;
        const int r = parsePacket(e.data(), e.size(), q);
        CHECK(r <= 0 || (i >= 3 && i <= 4 && r == 0), "corrupt byte %zu accepted (r=%d)", i, r);
    }
    AvPacket empty;
    empty.type = PktType::Text;
    auto de = serializePacket(empty);
    CHECK(parsePacket(de.data(), de.size(), q) == (int)de.size() && q.payload.empty(), "empty payload");
}

static void testFec() {
    Rng r(5);
    for (int idx = 0; idx < kNumFecProfiles; idx++) {
        const FecProfile f = fecProfile(idx);
        const int N = f.data + f.parity;
        CHECK(N % 4 == 0 && (N / 4 & (N / 4 - 1)) == 0, "profile %d: group of %d cells is not a power-of-two number of frames", idx, N);
        std::vector<std::vector<uint8_t>> data;
        for (int i = 0; i < f.data; i++) data.push_back(randomBytes(r, 178));
        auto par = fecEncode(idx, data);
        CHECK((int)par.size() == f.parity, "parity count");
        for (int trial = 0; trial < 20; trial++) {
            std::vector<std::vector<uint8_t>> cells = data;
            cells.insert(cells.end(), par.begin(), par.end());
            const int lose = f.parity == 0 ? 0 : (int)r.below(f.parity + 1);
            std::set<int> gone;
            while ((int)gone.size() < lose) gone.insert((int)r.below(N));
            for (int g : gone) cells[g].clear();
            CHECK(fecRecover(idx, cells), "profile %d: %d erasures not recovered", idx, lose);
            for (int i = 0; i < f.data; i++) CHECK(cells[i] == data[i], "profile %d cell %d differs", idx, i);
        }
        if (f.parity > 0) {
            std::vector<std::vector<uint8_t>> cells = data;
            cells.insert(cells.end(), par.begin(), par.end());
            for (int i = 0; i <= f.parity; i++) cells[i].clear();   // P + 1 erasures among the data
            CHECK(!fecRecover(idx, cells), "profile %d: P+1 erasures claimed recovered", idx);
        }
    }
}

static void testFragment() {
    Rng r(8);
    uint16_t seq = 65530;
    auto unit = randomBytes(r, 5000);
    auto pk = fragmentUnit(PktType::Video, 0, seq, 123456, true, unit, 900);
    CHECK(pk.size() == 6, "fragments %zu", pk.size());
    Reassembler ra;
    Reassembler::Unit u;
    int done = 0;
    for (auto& p : pk) done += ra.push(p, u) ? 1 : 0;
    CHECK(done == 1 && u.bytes == unit && u.ptsMs == 123456 && u.key, "reassembly");
    // a lost middle fragment drops the unit
    Reassembler rb;
    int got = 0;
    for (size_t i = 0; i < pk.size(); i++)
        if (i != 2) got += rb.push(pk[i], u) ? 1 : 0;
    CHECK(got == 0, "unit with a hole accepted");
    // an empty unit
    uint16_t s2 = 0;
    auto e = fragmentUnit(PktType::Data, 1, s2, 0, false, {}, 100);
    CHECK(e.size() == 1, "empty unit packets");
}

static std::vector<AvPacket> makeStream(const Profile& p, int fec, double seconds, double vbps, std::vector<std::vector<uint8_t>>* frames) {
    Packetiser pk(p, fec);
    SyntheticAv src(vbps, 30.0, 96000.0, std::min<size_t>(1200, pk.maxPayload()));
    std::vector<AvPacket> sent;
    AvPacket a;
    while (src.timeSec() < seconds) {
        src.next(a);
        sent.push_back(a);
        pk.push(a);
    }
    *frames = pk.takeFrames(true);
    return sent;
}

static void testPacketiserLoss() {
    const Profile p = profileOf(0, false);
    for (int fec : {0, 1, 2, 3, 4, 5, 6, 7}) {
        std::vector<std::vector<uint8_t>> frames;
        auto sent = makeStream(p, fec, 0.4, 1500000.0, &frames);
        const FecProfile f = fecProfile(fec);
        const int cellBytes = p.blockPayloadBytes;
        CHECK(frames.size() % (size_t)((f.data + f.parity) / 4) == 0, "fec %d: frames %zu not whole groups", fec, frames.size());
        for (auto& fr : frames) CHECK((int)fr.size() == framePayloadBytes(p), "frame size");
        // no loss: everything comes back exactly
        {
            Depacketiser d(p);
            for (size_t i = 0; i < frames.size(); i++) {
                std::vector<std::vector<uint8_t>> cells;
                for (int b = 0; b < 4; b++) cells.emplace_back(frames[i].begin() + b * cellBytes, frames[i].begin() + (b + 1) * cellBytes);
                d.pushCells((int)i, fec, cells);
            }
            d.flush();
            auto got = d.takePackets();
            CHECK(got.size() == sent.size(), "fec %d: no loss: %zu of %zu packets", fec, got.size(), sent.size());
            for (size_t i = 0; i < got.size() && i < sent.size(); i++)
                CHECK(got[i].type == sent[i].type && got[i].seq == sent[i].seq && got[i].payload == sent[i].payload, "fec %d packet %zu differs", fec, i);
        }
        if (f.parity == 0) continue;
        // P cells lost per group (spread randomly): still everything
        {
            Rng r(40 + fec);
            Depacketiser d(p);
            const int N = f.data + f.parity;
            std::set<long> lostCells;
            for (size_t g = 0; g * N < frames.size() * 4; g++) {
                std::set<int> gone;
                while ((int)gone.size() < f.parity) gone.insert((int)r.below(N));
                for (int c : gone) lostCells.insert((long)g * N + c);
            }
            for (size_t i = 0; i < frames.size(); i++) {
                std::vector<std::vector<uint8_t>> cells;
                for (int b = 0; b < 4; b++)
                    if (lostCells.count((long)i * 4 + b)) cells.emplace_back();
                    else cells.emplace_back(frames[i].begin() + b * cellBytes, frames[i].begin() + (b + 1) * cellBytes);
                d.pushCells((int)i, fec, cells);
            }
            d.flush();
            auto got = d.takePackets();
            CHECK(got.size() == sent.size(), "fec %d: P losses per group: %zu of %zu packets", fec, got.size(), sent.size());
            for (size_t i = 0; i < got.size() && i < sent.size(); i++) CHECK(got[i].payload == sent[i].payload, "fec %d recovered packet %zu differs", fec, i);
            CHECK(d.stats().groupsRecovered > 0, "fec %d: no group needed recovery?", fec);
        }
        // too many losses in one group: that group's packets are partly lost, every other group intact, nothing corrupted
        {
            Depacketiser d(p);
            std::set<long> lostCells;
            for (int c = 0; c <= f.parity; c++) lostCells.insert(c);   // group 0: P + 1 cells (data cells 0..P)
            for (size_t i = 0; i < frames.size(); i++) {
                std::vector<std::vector<uint8_t>> cells;
                for (int b = 0; b < 4; b++)
                    if (lostCells.count((long)i * 4 + b)) cells.emplace_back();
                    else cells.emplace_back(frames[i].begin() + b * cellBytes, frames[i].begin() + (b + 1) * cellBytes);
                d.pushCells((int)i, fec, cells);
            }
            d.flush();
            auto got = d.takePackets();
            CHECK(d.stats().groupsLost == 1, "fec %d: groupsLost %ld", fec, d.stats().groupsLost);
            std::set<std::pair<int, int>> sentKeys;
            for (auto& s : sent) sentKeys.insert({(int)s.type, s.seq});
            for (auto& g : got) {
                bool match = false;
                for (auto& s : sent)
                    if (s.type == g.type && s.seq == g.seq && s.payload == g.payload) { match = true; break; }
                CHECK(match, "fec %d: a packet that was never sent (or corrupted) was output", fec);
            }
            CHECK(got.size() < sent.size() && got.size() + 80 > sent.size() / 1, "fec %d: packets %zu of %zu after one lost group", fec, got.size(), sent.size());
        }
    }
}

static void testBudget() {
    printf("KAVB_BUDGET profile fec  raw      phyBlocks  afterCell  afterFec   forVideo   (Mbit/s; 900-byte packets, 96 kbit/s audio, 2 kbit/s side data)\n");
    for (int id = 0; id < kNumProfiles; id++)
        for (int fec : {0, 1, 2}) {
            const Profile p = profileOf(id, false);
            const BitBudget b = bitBudget(p, fec);
            printf("KAVB_BUDGET %c       %d    %.3f    %.3f      %.3f      %.3f      %.3f\n", p.name, fec, b.rawBits / 1e6, b.phyBlocks / 1e6, b.afterCellHeader / 1e6, b.afterFec / 1e6, b.forVideo / 1e6);
            CHECK(b.phyBlocks < b.rawBits && b.afterFec <= b.afterCellHeader && b.forVideo < b.afterFec, "budget ordering");
        }
    const Profile a = profileOf(0, false);
    const BitBudget ba = bitBudget(a, 1);
    CHECK(ba.forVideo > 1.0e6, "profile A has less than 1 Mbit/s for video: %.0f", ba.forVideo);
}

static void testEndToEnd() {
    const Profile p = profileOf(0, false);
    const int fec = 1;   // 14 + 2
    Packetiser pk(p, fec);
    const BitBudget bb = bitBudget(p, fec);
    SyntheticAv src(bb.forVideo * 0.9, 30.0, 96000.0, 1200);
    std::vector<AvPacket> sent;
    AvPacket a;
    while (src.timeSec() < 0.5) {
        src.next(a);
        sent.push_back(a);
        pk.push(a);
    }
    auto frames = pk.takeFrames(true);
    Rng rng(77);
    Channel ch;
    ch.snrDb = 13.5;
    ch.cfoCycPerSym = 0.007;
    ch.delaySamples = 0.31;
    ch.phase = 1.0;
    Transmitter tx(0);
    auto iq = applyChannel(tx.modulate(frames, fec, 0, 0, 0), ch, rng, 1000);
    Receiver rx;
    Depacketiser dp(p);
    size_t pos = 0;
    int frOk = 0, frAll = 0;
    while (pos < iq.size()) {
        const size_t n = std::min(iq.size() - pos, (size_t)20000);
        for (const auto& f : rx.push(iq.data() + pos, n)) {
            frAll++;
            frOk += f.allOk ? 1 : 0;
            dp.pushFrame(f);
        }
        pos += n;
    }
    dp.flush();
    auto got = dp.takePackets();
    size_t match = 0;
    std::vector<size_t> lostIdx;
    {
        size_t gi = 0;
        for (size_t si = 0; si < sent.size(); si++) {
            if (gi < got.size() && got[gi].type == sent[si].type && got[gi].seq == sent[si].seq && got[gi].payload == sent[si].payload) { match++; gi++; }
        }
    }
    printf("KAVB_E2E frames sent=%zu received=%d allOk=%d packets sent=%zu recovered=%zu groups ok=%ld recovered=%ld lost=%ld\n", frames.size(), frAll, frOk, sent.size(), match,
           dp.stats().groupsOk, dp.stats().groupsRecovered, dp.stats().groupsLost);
    CHECK(frAll == (int)frames.size(), "frames received %d of %zu", frAll, frames.size());
    CHECK(match == sent.size() && got.size() == sent.size(), "packets recovered %zu of %zu (output %zu)", match, sent.size(), got.size());
    // the receiver side: reassemble the video units
    Reassembler rv;
    Reassembler::Unit u;
    int units = 0;
    for (const auto& g : got)
        if (g.type == PktType::Video && rv.push(g, u)) {
            CHECK(u.bytes == SyntheticAv::unitBytes(0, (uint32_t)units, u.bytes.size()), "video unit %d content", units);
            units++;
        }
    CHECK(units >= 14, "video units %d", units);
}

int main() {
    testPacket();
    testFec();
    testFragment();
    testPacketiserLoss();
    testBudget();
    testEndToEnd();
    if (g_fail) { printf("test_kavb_av: %d FAILED\n", g_fail); return 1; }
    printf("test_kavb_av: ok\n");
    return 0;
}
