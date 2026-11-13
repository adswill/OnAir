// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UHF RFID (EPC Gen2) reader commands: every command type, Tari 6.25 to 25 us, data-1 of 1.5 to 2 Tari, TRcal ratios, reader clock 1 % off, carrier offset
// and drift, input rates, noise sweep with the false-command count, bad CRCs and 15 minutes of carrier plus noise. Simulated signals only.
#include "x40_gen2.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int count(const X40Telemetry& t, const char* kind) { return countKind(t, kind); }
static size_t total(const X40Telemetry& t) { return t.rows.size(); }
static std::vector<std::pair<Bits, bool>> sequence() {
    return {{uhfQuery(4, 2, 3, 2, 1, 1, 0), true}, {uhfAck(0xBEEF), false}, {uhfQRep(2), false}, {uhfQAdj(1, 6), false}, {uhfNak(), false},
            {uhfSelect(4, 0, 1, 32, "1010011100110101"), false}, {uhfReqRn(0x1234), false}, {uhfRead(3, 5, 4, 0xA5A5), false}};
}
static const char* kKinds[] = {"Query", "ACK", "QueryRep", "QueryAdjust", "NAK", "Select", "Req_RN", "Read"};
static bool all(const X40Telemetry& t) { for (auto k : kKinds) if (count(t, k) != 1) return false; return total(t) == 8; }

int main() {
    // the content of every command
    {
        auto t = run(x40::UhfRfid, uhfReader(Uhf{}, sequence(), 2.0e6), 2.0e6);
        CHECK(all(t), "all commands once: %zu rows", total(t));
        for (auto& r : t.rows) {
            if (r.kind == "Query") CHECK(r.id == "Q=4 S2" && r.text.find("Miller 4") != std::string::npos && r.text.find("16 slots") != std::string::npos && r.text.find("target B") != std::string::npos, "query: %s | %s", r.id.c_str(), r.text.c_str());
            if (r.kind == "ACK") CHECK(r.id == "BEEF", "ack %s", r.id.c_str());
            if (r.kind == "Req_RN") CHECK(r.id == "1234", "req_rn %s", r.id.c_str());
            if (r.kind == "Select") CHECK(r.text.find("1010011100110101") != std::string::npos && r.text.find("from bit 32") != std::string::npos, "select %s", r.text.c_str());
            if (r.kind == "Read") CHECK(r.text.find("4 words") != std::string::npos && r.text.find("User") != std::string::npos && r.text.find("A5A5") != std::string::npos, "read %s", r.text.c_str());
        }
    }
    // timing: Tari, data-1 ratio, TRcal ratio, depth, clock error, input rate
    for (double fs : {1.0e6, 2.0e6, 2.4e6}) for (double tari : {6.25e-6, 12.5e-6, 25e-6}) for (double one : {1.5, 2.0}) {
        if (fs < 2.3e6 && tari < 7e-6) continue;       // 6 samples per Tari at the 1 Msps working rate: only claimed from 2.4 Msps
        Uhf u; u.tari = tari; u.one = one; u.trcalRt = tari < 7e-6 ? 1.5 : 2.5; u.depth = 0.85;
        auto t = run(x40::UhfRfid, uhfReader(u, sequence(), fs), fs);
        CHECK(all(t), "fs %.1f Mhz tari %.2f us one %.1f: %zu rows", fs / 1e6, tari * 1e6, one, total(t));
    }
    for (double ppm : {10000.0, -10000.0}) { Uhf u; u.tariPpm = ppm; CHECK(all(run(x40::UhfRfid, uhfReader(u, sequence(), 2e6), 2e6)), "clock %.0f ppm", ppm); }
    // carrier offset +-80 kHz with drift, and an off-centre tuning that is announced
    for (double off : {0.0, 80000.0, -80000.0}) { auto x = uhfReader(Uhf{}, sequence(), 2e6); mix(x, off, 20000, 2e6); CHECK(all(run(x40::UhfRfid, x, 2e6)), "offset %.0f", off); }
    { auto x = uhfReader(Uhf{}, sequence(), 2e6); mix(x, 300000, 0, 2e6); CHECK(all(run(x40::UhfRfid, x, 2e6, 300000)), "off-centre 300 kHz"); }
    // a wrong CRC: not listed
    { auto q = uhfQuery(3, 0); q.back() ^= 1; auto r = uhfRead(1, 0, 2, 7); r[20] ^= 1; auto t = run(x40::UhfRfid, uhfReader(Uhf{}, {{q, true}, {r, false}, {uhfAck(0x0F0F), false}}, 2e6), 2e6);
      CHECK(total(t) == 1 && count(t, "ACK") == 1, "bad CRC listed: %zu rows", total(t)); }
    // noise
    printf("noise sweep, depth 90 %%, Tari 12.5 us, carrier to noise in 600 kHz, 4 runs of 8 commands:\n");
    for (double cnr : {30.0, 24.0, 20.0, 17.0, 14.0, 11.0}) {
        int ok = 0, tot = 0, falseRows = 0;
        for (unsigned s = 1; s <= 4; s++) {
            auto x = uhfReader(Uhf{}, sequence(), 2e6); addNoise(x, noiseFor(cnr, 600000, 2e6), s);
            auto t = run(x40::UhfRfid, x, 2e6); ok += (int)total(t) <= 8 ? (int)total(t) : 8; tot += 8; if (total(t) > 8) falseRows += (int)total(t) - 8;
        }
        printf("  %.0f dB: %d of %d commands, %d extra rows\n", cnr, ok, tot, falseRows);
        if (cnr >= 20) CHECK(ok == tot && falseRows == 0, "%.0f dB: %d of %d, %d extra", cnr, ok, tot, falseRows);
    }
    // 15 minutes of carrier plus noise, and of noise alone
    {
        size_t falseRows = 0;
        for (int k = 0; k < 15; k++) {
            std::vector<cf32> x((size_t)(2e6 * 60 / 4), cf32(k % 2 ? 1.f : 0.f, 0)); addNoise(x, noiseFor(20, 600000, 2e6), 50 + k);
            falseRows += total(run(x40::UhfRfid, x, 2e6));
        }
        printf("noise (carrier 20 dB over it, or none), 225 s of 2 Msps: %zu false commands\n", falseRows);
        CHECK(falseRows == 0, "false commands %zu", falseRows);
    }
    printf(fails ? "x40 uhfrfid: %d FAILED\n" : "x40 uhfrfid: ok\n", fails);
    return fails ? 1 : 0;
}
