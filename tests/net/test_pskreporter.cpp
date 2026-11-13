// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK Reporter: the IPFIX encoder (templates, record round trip, size limit), the batcher (dedupe, interval) and the validators.
#include "dect2/net/pskreporter.h"
#include <cstdio>
#include <set>
#include <string>
using namespace dect2;
using namespace dect2::pskr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Spot mk(const char* call, uint32_t f, int snr, int64_t t, const char* mode = "FT8", const char* loc = "KO24") {
    Spot s; s.call = call; s.freqHz = f; s.snr = snr; s.time = t; s.mode = mode; s.locator = loc; return s;
}
int main() {
    const Receiver rx{"LY1ABC", "KO24ab", "OnAir 0.2.4", "dipole"};
    std::vector<Spot> sp = {mk("DL1XYZ", 14074500, -12, 1700000000), mk("W1AW", 7075123, 5, 1700000001, "FT4", ""), mk("JA1ZZZ", 14074800, -30, 1700000002, "WSPR")};
    std::vector<uint8_t> pkt;
    size_t n = encodePacket(pkt, rx, sp, 0, 1700000010, 7, 0xDEADBEEF, true);
    CHECK(n == 3, "all fit: %zu", n);
    CHECK(pkt.size() % 4 == 0 && pkt.size() < kMaxPacket, "size %zu", pkt.size());
    CHECK(pkt[0] == 0 && pkt[1] == 0x0A && (pkt[2] << 8 | pkt[3]) == (int)pkt.size(), "header");
    // the receiver template set (id 3, 0x2C long, linking id 0x9992, 4 fields, 1 scope field) follows the 16 byte header
    CHECK(pkt[16] == 0 && pkt[17] == 3 && pkt[18] == 0 && pkt[19] == 0x2C && pkt[20] == 0x99 && pkt[21] == 0x92 && pkt[23] == 4, "receiver template");
    CHECK(pkt[16 + 0x2C + 1] == 2 && pkt[16 + 0x2C + 3] == 0x3C && pkt[16 + 0x2C + 4] == 0x99 && pkt[16 + 0x2C + 5] == 0x93 && pkt[16 + 0x2C + 7] == 7, "sender template");
    Decoded d;
    CHECK(decodePacket(pkt.data(), pkt.size(), d), "decode");
    CHECK(d.hasTemplates && d.seq == 7 && d.streamId == 0xDEADBEEF && d.time == 1700000010, "header fields");
    CHECK(d.rx.call == "LY1ABC" && d.rx.locator == "KO24ab" && d.rx.software == "OnAir 0.2.4" && d.rx.antenna == "dipole", "receiver");
    CHECK(d.spots.size() == 3, "spots %zu", d.spots.size());
    if (d.spots.size() == 3) for (int i = 0; i < 3; i++) {
        const Spot &a = sp[i], &b = d.spots[i];
        CHECK(a.call == b.call && a.freqHz == b.freqHz && a.snr == b.snr && a.mode == b.mode && a.locator == b.locator && a.time == b.time, "spot %d", i);
    }
    // without templates: smaller, still decodes
    std::vector<uint8_t> p2;
    encodePacket(p2, rx, sp, 0, 1700000010, 10, 1, false);
    CHECK(p2.size() + 0x2C + 0x3C == pkt.size(), "template bytes %zu vs %zu", p2.size(), pkt.size());
    CHECK(decodePacket(p2.data(), p2.size(), d) && !d.hasTemplates && d.spots.size() == 3, "no templates");
    // a receiver-only update
    encodePacket(p2, rx, {}, 0, 1, 0, 1, false);
    CHECK(decodePacket(p2.data(), p2.size(), d) && d.spots.empty() && d.rx.call == "LY1ABC", "receiver only");
    // size limit: 300 spots split over several packets, nothing lost, each under the limit
    std::vector<Spot> big;
    for (int i = 0; i < 300; i++) big.push_back(mk(("DL" + std::to_string(i % 10) + "ABC").c_str(), 14074000 + i, i % 40 - 30, 1700000000 + i, "FT8", "JO62ab"));
    size_t at = 0; int pk = 0; size_t got = 0;
    while (at < big.size()) {
        const size_t k = encodePacket(pkt, rx, big, at, 1700000100, (uint32_t)at, 5, pk == 0);
        CHECK(k > 0 && pkt.size() <= kMaxPacket, "packet %d: %zu spots, %zu bytes", pk, k, pkt.size());
        if (!k) break;
        CHECK(decodePacket(pkt.data(), pkt.size(), d) && d.spots.size() == k && d.spots[0].freqHz == big[at].freqHz, "big decode %d", pk);
        at += k; got += k; pk++;
    }
    CHECK(got == 300 && pk >= 3, "big: %zu in %d packets", got, pk);
    // truncated / corrupt packets are refused
    encodePacket(pkt, rx, sp, 0, 1, 0, 1, true);
    CHECK(!decodePacket(pkt.data(), pkt.size() - 4, d), "truncated");
    // batcher
    Batcher b(300, 5, 600);
    CHECK(b.add(mk("DL1XYZ", 14074500, -5, 1000), 1000), "first");
    CHECK(!b.add(mk("DL1XYZ", 14074700, -9, 1010), 1010), "same sender, mode, band: duplicate");
    CHECK(b.add(mk("DL1XYZ", 7075000, -9, 1010), 1010), "other band");
    CHECK(b.add(mk("DL1XYZ", 14074500, -9, 1010, "FT4"), 1010), "other mode");
    CHECK(!b.due(1100), "not due before the interval");
    CHECK(b.due(1300), "due after 5 minutes");
    CHECK(b.add(mk("A1AAA", 14074500, 0, 1020), 1020) && b.add(mk("B1BBB", 14074500, 0, 1020), 1020), "more");
    CHECK(b.due(1020), "full (5)");
    CHECK(b.take(1300).size() == 5 && b.pending() == 0 && !b.due(5000), "take");
    CHECK(!b.add(mk("DL1XYZ", 14074500, -5, 1301), 1301), "still deduped after take");
    CHECK(b.add(mk("DL1XYZ", 14074500, -5, 1700), 1700), "dedupe window over");
    // validators
    CHECK(validCallsign("LY1ABC") && validCallsign("DL1XYZ/P") && validCallsign("3DA0XY") && !validCallsign("ABC") && !validCallsign("12345") && !validCallsign("ly1abc") && !validCallsign("LY 1ABC"), "callsigns");
    CHECK(validLocator("KO24") && validLocator("KO24ab") && validLocator("KO24ab12") && !validLocator("KO2") && !validLocator("ZZ24") && !validLocator("KO24yy") && !validLocator("KO24a"), "locators");
    CHECK(sendsInText("CQ CQ DE LY1ABC LY1ABC K", "LY1ABC") && !sendsInText("DL1XYZ DE LY1ABC", "DL1XYZ") && !sendsInText("DE LY1ABCD", "LY1ABC"), "DE");
    // the sender thread stays off and silent until the user opts in
    PskReporter::get().setDialHz(14074000);
    PskReporter::get().spot("FT8", 1500, -5, "DL1XYZ", "JO62");
    CHECK(!PskReporter::get().want() && PskReporter::get().status().state == "off", "off by default");
    PskReporterConfig c; c.on = true; c.call = "ly1abc"; c.locator = "KO24ab"; c.host = "127.0.0.1"; c.port = 9;
    PskReporter::get().configure(c);
    CHECK(PskReporter::get().want(), "on");
    PskReporter::get().spot("FT8", 1500, -5, "DL1XYZ", "JO62");
    PskReporter::get().spot("FT8", 1510, -5, "DL1XYZ", "JO62");
    CHECK(PskReporter::get().status().duplicates == 1 && PskReporter::get().status().state.find("1 spots waiting") != std::string::npos, "queued: %s", PskReporter::get().status().state.c_str());
    c.call = "x"; PskReporter::get().configure(c);
    CHECK(!PskReporter::get().want(), "invalid callsign: nothing is collected");
    // multi-band: spots from 5 bands (each with its own dial) all reach the sender, each in its own frequency group; nothing is sent for real
    {
        PskReporterConfig mc; mc.on = true; mc.call = "PA9900"; mc.locator = "JO22ab"; mc.host = "127.0.0.1"; mc.port = 9;
        std::vector<std::vector<uint8_t>> sent;
        { PskReporterConfig off = mc; off.on = false; PskReporter::get().configure(off); }   // drops what the earlier checks left pending
        PskReporter::get().configure(mc);
        PskReporter::get().setSenderForTest([&](const uint8_t* d, size_t n) { sent.emplace_back(d, d + n); return true; });
        PskReporter::get().setDialHz(7074000);   // the radio is tuned to 40 m; the other bands come with their own dial
        const double dials[5] = {3573000, 7074000, 10136000, 14074000, 21074000};
        for (int i = 0; i < 5; i++) for (int k = 0; k < 3; k++) PskReporter::get().spot("FT8", 1000 + 100 * k, -10.f, "DL" + std::to_string(i) + "ABC", "JO62", i == 1 ? 0.0 : dials[i]);
        PskReporter::get().flushNowForTest(1700000000);
        std::set<uint32_t> bands; size_t total = 0;
        for (const auto& p : sent) {
            CHECK(p.size() <= 1024, "packet %zu bytes", p.size());
            Decoded dd; CHECK(decodePacket(p.data(), p.size(), dd), "mb decode");
            CHECK(dd.rx.call == "PA9900", "receiver call");
            for (const Spot& sp : dd.spots) { bands.insert(sp.freqHz / 1000000); total++; CHECK(sp.mode == "FT8" && sp.freqHz % 1000000 > 0, "spot"); }
        }
        CHECK(total == 5, "5 calls x 3 slots on the same band deduped to 5 spots: %zu", total);
        CHECK(bands.size() == 5 && bands.count(3) && bands.count(7) && bands.count(10) && bands.count(14) && bands.count(21), "all 5 bands present: %zu", bands.size());
        // a single band is unchanged: tuned dial + audio offset
        sent.clear();
        PskReporter::get().setDialHz(14074000);
        PskReporter::get().spot("FT8", 1234, -3.f, "W1AW", "FN31");
        PskReporter::get().flushNowForTest(1700000400);
        Decoded d1;
        CHECK(sent.size() == 1 && decodePacket(sent[0].data(), sent[0].size(), d1) && d1.spots.size() == 1 && d1.spots[0].freqHz == 14075234 && d1.spots[0].snr == -3, "single band");
        PskReporter::get().setSenderForTest(nullptr);
        PskReporter::get().stop();
    }
    PskReporter::get().stop();
    printf(fails ? "FAILED %d\n" : "pskreporter OK\n", fails);
    return fails != 0;
}
