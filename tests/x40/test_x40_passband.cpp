// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passband signal classifier: each signal class alone, all of them at once, keyed signals that stop and start, a level sweep, noise alone. Simulated signals only.
#include "x40_gen2.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static dect2::X40Telemetry go(const std::vector<PbSig>& s, double fs, double sec, unsigned seed = 1) { PbGen g(s, fs, seed); return runStream(x40::Passband, fs, sec, [&](size_t i0, size_t n, cf32* o) { g.render(i0, n, o); }); }
static std::string classAt(const X40Telemetry& t, double khz) {   // the class of the signal named near khz, from the oldest row
    std::string c; for (auto& r : t.rows) if (r.kind == "Signal" && std::fabs(atof(r.id.c_str()) - khz) < 0.5) c = r.text.substr(0, r.text.find(':')); return c;
}
static int signalRows(const X40Telemetry& t) { return countKind(t, "Signal"); }

int main() {
    for (double fs : {48000.0, 96000.0, 250000.0}) {
        struct C { PbSig s; const char* want; } cases[] = {
            {{PbSig::Carrier, 10000, 25}, "unmodulated carrier"}, {{PbSig::Cw, -6000, 25}, "CW (keyed)"}, {{PbSig::Rtty, 4000, 28}, "narrow data (PSK / FSK)"},
            {{PbSig::Ssb, -3000, 40}, "SSB / voice-like"}, {{PbSig::Wide, -14000, 42}, "wideband steady signal (FM or high-rate data)"}};
        for (auto& c : cases) {
            auto t = go({c.s}, fs, 14.0);
            const double fk = (c.s.kind == PbSig::Ssb ? c.s.freq + 1500 : c.s.freq) / 1000;
            const std::string got = classAt(t, fk);
            CHECK(got == c.want, "fs %.0f kind %d at %.1f kHz: got '%s' want '%s' (%d rows)", fs, (int)c.s.kind, fk, got.c_str(), c.want, signalRows(t));
        }
    }
    // all at once, 22 s: five signals named, each with its class
    {
        std::vector<PbSig> all = {{PbSig::Carrier, 10000, 25}, {PbSig::Cw, -6000, 25}, {PbSig::Rtty, 4000, 28}, {PbSig::Ssb, -3000, 40}, {PbSig::Wide, -16000, 42}, {PbSig::Cw, 17000, 22}};
        auto t = go(all, 96000, 22.0, 3);
        CHECK(classAt(t, 10) == "unmodulated carrier" && classAt(t, -6) == "CW (keyed)" && classAt(t, 4) == "narrow data (PSK / FSK)" && classAt(t, -1.5) == "SSB / voice-like" && classAt(t, -16) == "wideband steady signal (FM or high-rate data)" && classAt(t, 17) == "CW (keyed)",
              "mixed: %s | %s | %s | %s | %s | %s", classAt(t, 10).c_str(), classAt(t, -6).c_str(), classAt(t, 4).c_str(), classAt(t, -1.5).c_str(), classAt(t, -16).c_str(), classAt(t, 17).c_str());
        CHECK(signalRows(t) == 6, "rows %d", signalRows(t));
    }
    // a carrier that stops at 8 s and comes back at 12 s is one signal (the gap is shorter than the track memory only when under 0.6 s: here it is two)
    { auto t = go({{PbSig::Carrier, 6000, 25, 0, 8}, {PbSig::Carrier, 6000, 25, 12, 1e9}}, 96000, 22.0); CHECK(signalRows(t) == 2, "two appearances: %d rows", signalRows(t)); }
    // level sweep: bin power of the signal over the noise in one 94 Hz bin
    printf("level sweep (carrier / CW / SSB, 96 ksps, 14 s): class named\n");
    for (double db : {22.0, 18.0, 14.0, 12.0, 10.0}) {
        auto a = go({{PbSig::Carrier, 10000, db}}, 96000, 14.0); auto b = go({{PbSig::Cw, -6000, db}}, 96000, 14.0); auto c = go({{PbSig::Ssb, -3000, db + 14}}, 96000, 14.0);
        printf("  %.0f dB: carrier '%s', CW '%s', SSB '%s'\n", db, classAt(a, 10).c_str(), classAt(b, -6).c_str(), classAt(c, -1.5).c_str());
        if (db >= 18) CHECK(classAt(a, 10) == "unmodulated carrier" && classAt(b, -6) == "CW (keyed)" && classAt(c, -1.5) == "SSB / voice-like", "level %.0f dB", db);
    }
    // noise alone: 3 minutes, no signal
    { int rows = 0; for (int k = 0; k < 6; k++) rows += signalRows(go({}, 96000, 30.0, 40 + k)); printf("noise alone, 3 minutes: %d false signals\n", rows); CHECK(rows == 0, "false signals %d", rows); }
    printf(fails ? "x40 passband: %d FAILED\n" : "x40 passband: ok\n", fails);
    return fails ? 1 : 0;
}
