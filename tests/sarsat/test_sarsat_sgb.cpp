// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT second-generation beacons (C/S T.018): the PRN segments against the 64 first chips of Table 2.2, the BCH generator and check against annex B.1,
// the worked example message (decoded fields, position, 23 hex ID), repair of 6 bit errors, then the receiver on the generated signal: clean, in noise,
// with carrier offsets, a chip rate error and at other sample rates. Noise alone must give nothing.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/sarsat/sgb_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::sarsat;
#include "../common/test_util.h"

static std::string hex64(const std::vector<uint8_t>& c) {
    std::string s;
    for (int i = 0; i < 64; i += 4) { char b[2]; snprintf(b, sizeof b, "%X", (c[(size_t)i] << 3) | (c[(size_t)i + 1] << 2) | (c[(size_t)i + 2] << 1) | c[(size_t)i + 3]); s += b; }
    return s;
}
static std::vector<uint8_t> fromBin(const std::string& s) {
    std::vector<uint8_t> v;
    for (char c : s) if (c == '0' || c == '1') v.push_back((uint8_t)(c - '0'));
    return v;
}

static SarsatTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false) {
    SarsatReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
    auto syn = makeSarsatSynth(cfg, rate);
    std::vector<cf32> buf(32768);
    SarsatTelemetry t;
    uint64_t seq = 0;
    unsigned s = 4242;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        SarsatTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static int countFound(const SarsatTelemetry& t, const char* what) {
    int found = 0;
    for (const SgbSpec& w : sgbTestBeacons()) {
        std::vector<uint8_t> bits = sgbEncode(w);
        Beacon ref;
        sgbDecode(bits, ref);
        bool ok = false;
        for (const SarsatEntry& e : t.log)
            if (e.b.second && e.b.hexId == ref.hexId && e.b.hasPos == ref.hasPos && std::fabs(e.b.lat - ref.lat) < 1e-9 && std::fabs(e.b.lon - ref.lon) < 1e-9 && e.b.rotId == ref.rotId && e.b.beaconType == ref.beaconType) { ok = true; break; }
        CHECK(ok, "%s: missing beacon %s", what, ref.hexId.c_str());
        found += ok;
    }
    int stray = 0;
    for (const SarsatEntry& e : t.log) {
        bool known = false;
        for (const SgbSpec& w : sgbTestBeacons()) { std::vector<uint8_t> b = sgbEncode(w); Beacon r; sgbDecode(b, r); known |= r.hexId == e.b.hexId; }
        if (!known) { stray++; printf("  stray %s snr %.1f cfo %.0f\n", e.b.hexId.c_str(), e.snrDb, e.cfoHz); }
    }
    CHECK(stray == 0, "%s: %d messages that were not sent", what, stray);
    return found;
}

int main() {
    // Table 2.2: the first 64 chips of the four segments
    CHECK(hex64(sgbPrn(false, false)) == "80000108421284A1", "normal I: %s", hex64(sgbPrn(false, false)).c_str());
    CHECK(hex64(sgbPrn(true, false)) == "3F8358BAD030F231", "normal Q: %s", hex64(sgbPrn(true, false)).c_str());
    CHECK(hex64(sgbPrn(false, true)) == "0F934A4D4CF3028D", "self-test I: %s", hex64(sgbPrn(false, true)).c_str());
    CHECK(hex64(sgbPrn(true, true)) == "14973DC716CDE124", "self-test Q: %s", hex64(sgbPrn(true, true)).c_str());
    // annex B.1: generator polynomial, and the worked example (202 information bits and the 48 bit check)
    CHECK(sgbGenerator() == 0x1C7EB85DF3C97ull, "generator %llx", (unsigned long long)sgbGenerator());
    const std::vector<uint8_t> info = fromBin("0000 0000 1110 0110 0000 1000 1111 0100 1100 1001 1000 0110 0001 1001 0110 0001 1000 1000 1010 0000 0100 0111 1100 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 1111 1111 1111 1100 0000 0001 0000 0000 1100 0001 1010 0000 0000 1001 0110 00");
    CHECK(info.size() == 202, "example length %zu", info.size());
    const uint64_t bch = sgbBch(info);
    CHECK(bch == 0x492A4FC57A49ull, "example BCH %llx", (unsigned long long)bch);
    std::vector<uint8_t> msg = info;
    for (int i = 47; i >= 0; i--) msg.push_back((uint8_t)((bch >> i) & 1));
    CHECK(sgbCorrect(msg) == 0, "example word is not clean");
    Beacon ex;
    CHECK(sgbDecode(msg, ex), "example does not decode");
    CHECK(ex.tac == 230 && ex.serial == 573 && ex.mid == 201 && ex.homing && !ex.rls, "example fields %d %d %d", ex.tac, ex.serial, ex.mid);
    CHECK(ex.hasPos && std::fabs(ex.lat - 48.793153539336956) < 2e-5 && std::fabs(ex.lon - 69.00875866413116) < 2e-5, "example position %.6f %.6f", ex.lat, ex.lon);
    CHECK(ex.beaconType == "ELT" && ex.rotId == 0, "example type %s rot %d", ex.beaconType.c_str(), ex.rotId);
    // annex B.2 lists the test protocol flag as 1 although bit 43 of the same example is 0: with the flag taken from bit 43 the last digits differ (...D0 against ...D8)
    CHECK(ex.hexId == "9934039823D000000000000", "example 23 hex ID %s", ex.hexId.c_str());
    // up to 6 errors are repaired, 7 are not claimed to be
    {
        for (int errs = 1; errs <= 6; errs++) {
            std::vector<uint8_t> w = msg;
            for (int k = 0; k < errs; k++) w[(size_t)((k * 37 + 5) % 250)] ^= 1;
            CHECK(sgbCorrect(w) == errs && w == msg, "%d errors not repaired", errs);
        }
        std::vector<uint8_t> w = msg;
        for (int k = 0; k < 9; k++) w[(size_t)((k * 29 + 3) % 250)] ^= 1;
        const std::vector<uint8_t> before = w;
        const int r = sgbCorrect(w);
        CHECK(r < 0 || w != msg, "9 errors gave the original back");
        (void)before;
    }
    // every test beacon: encode -> decode, positions, default position
    for (const SgbSpec& s : sgbTestBeacons()) {
        std::vector<uint8_t> b = sgbEncode(s);
        Beacon o;
        CHECK(sgbCorrect(b) == 0 && sgbDecode(b, o), "test beacon");
        CHECK(o.tac == s.tac && o.serial == s.serial && o.mid == s.mid && o.hasPos == s.hasPos && o.rotId == s.rotId && o.testProto == s.test, "fields of %s", o.hexId.c_str());
        if (s.hasPos) CHECK(std::fabs(o.lat - s.lat) < 2e-5 && std::fabs(o.lon - s.lon) < 2e-5, "position %.5f %.5f", o.lat, o.lon);
    }

    // the receiver
    const double rate = 256000;
    const double seconds = 4 * 4.0 + 3;   // four bursts of 1 s, 3 s apart, and the ring that has to fill
    {
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeOpt[2] = 1;
        countFound(run(cfg, rate, seconds), "clean");
    }
    {
        SynthConfig cfg; cfg.snrDb = 12; cfg.cfoHz = 6000; cfg.modeOpt[2] = 1;
        countFound(run(cfg, rate, seconds), "12 dB, +6 kHz");
    }
    {
        SynthConfig cfg; cfg.snrDb = 12; cfg.cfoHz = -9500; cfg.modeOpt[2] = 1;
        countFound(run(cfg, rate, seconds), "12 dB, -9.5 kHz");
    }
    {   // chip rate 25 ppm off (a LEO Doppler shift) and a 300 Hz offset
        SynthConfig cfg; cfg.snrDb = 15; cfg.cfoHz = 300; cfg.modeVal[0] = 25; cfg.modeOpt[2] = 1;
        countFound(run(cfg, rate, seconds), "chip rate +25 ppm");
    }
    {
        SynthConfig cfg; cfg.snrDb = 15; cfg.cfoHz = 2000; cfg.modeOpt[2] = 1;
        countFound(run(cfg, 1024000, seconds), "1.024 Msps");
        countFound(run(cfg, 2400000, seconds), "2.4 Msps");
    }
    {
        SynthConfig cfg; cfg.snrDb = 7; cfg.cfoHz = 1000; cfg.modeOpt[2] = 1;
        countFound(run(cfg, rate, seconds * 2), "7 dB");
    }
    {
        SynthConfig cfg;
        const SarsatTelemetry t = run(cfg, rate, 25, true);
        CHECK(t.log.empty(), "noise gave %zu messages", t.log.size());
    }
    return testSummary("test_sarsat_sgb");
}
