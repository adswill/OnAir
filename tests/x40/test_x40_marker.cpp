// ILS marker beacon receiver: outer (400 Hz dashes, 2 per second), middle (1300 Hz dot-dash, 95 pairs per minute) and inner (3000 Hz dots, 6 per second) markers on an AM carrier with a carrier offset,
// slow fading and noise; the marker is named from the tone and the keying is measured. Wrong keying is listed as "Tone only"; two tones at once, an identification tone (1020 Hz Morse), noise alone and a plain carrier
// give no marker. Simulated signals only: no recording of a marker beacon was available.
#include "x40_gen4.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<X40Row> rows(const X40Telemetry& t, const char* kind) { std::vector<X40Row> v; for (auto& r : t.rows) if (r.kind == kind) v.push_back(r); return v; }
static X40Telemetry go(KeyedAm& k, double fs, double seconds, double offset = 0) { return runChunks(x40::Marker, fs, seconds, std::ref(k), offset, 75e6, 0.5); }

int main() {
    const double fs = 48000;
    struct M { const char* name; double hz; std::vector<std::pair<double, double>> el; double rateLo, rateHi; };
    const M ms[] = {{"Outer marker", 400, outerMarker(), 1.7, 2.3}, {"Middle marker", 1300, middleMarker(), 80, 110}, {"Inner marker", 3000, innerMarker(), 5.0, 7.0}};
    for (const M& m : ms) {
        for (double lo : {0.0, 2500.0, -3500.0}) for (double fade : {0.0, 0.3}) {
            KeyedAm k; k.fs = fs; k.elems = m.el; k.toneHz = m.hz; k.lo = lo; k.fadeDepth = fade; k.noise = noiseFor(30, 12000, fs); k.startSec = 1.0; k.stopSec = 9.0; k.seed = 5;
            auto t = go(k, fs, 14.0);
            const auto r = rows(t, "Marker");
            CHECK(r.size() == 1 && r[0].id == m.name, "%s lo %.0f fade %.1f: %zu marker rows (%s)", m.name, lo, fade, r.size(), r.empty() ? "" : r[0].id.c_str());
            CHECK(rows(t, "Tone only").empty(), "%s: tone-only rows", m.name);
            if (r.size() == 1) {
                const double rate = atof(strstr(r[0].text.c_str(), "Hz, ") + 4);
                CHECK(rate >= m.rateLo && rate <= m.rateHi, "%s: keying %.2f outside %.1f - %.1f (%s)", m.name, rate, m.rateLo, m.rateHi, r[0].text.c_str());
            }
        }
    }
    // noise sweep: the carrier to noise in 12 kHz at which each marker is still named
    printf("noise sweep (carrier to noise in 12 kHz): marker rows named right / tone-only / wrong\n");
    for (const M& m : ms) for (double cn : {30.0, 24.0, 18.0, 12.0, 8.0, 4.0, 0.0, -4.0}) {
        int right = 0, tone = 0, wrong = 0;
        for (unsigned seed : {1u, 2u, 3u, 4u}) { KeyedAm k; k.fs = fs; k.elems = m.el; k.toneHz = m.hz; k.noise = noiseFor(cn, 12000, fs); k.stopSec = 9; k.seed = seed; auto t = go(k, fs, 12.0);
            for (auto& r : rows(t, "Marker")) { if (r.id == m.name) right++; else wrong++; } tone += (int)rows(t, "Tone only").size(); }
        printf("  %-14s %4.0f dB: %d / %d / %d of 4\n", m.name, cn, right, tone, wrong);
        if (cn >= 18) CHECK(right == 4 && wrong == 0, "%s at %.0f dB: %d right %d wrong", m.name, cn, right, wrong);
        CHECK(wrong == 0, "%s at %.0f dB: %d wrong markers", m.name, cn, wrong);
    }
    // the tone of one marker with the keying of another: tone only
    { KeyedAm k; k.fs = fs; k.elems = innerMarker(); k.toneHz = 400; k.noise = noiseFor(30, 12000, fs); k.stopSec = 8; auto t = go(k, fs, 12.0);
      CHECK(rows(t, "Marker").empty() && rows(t, "Tone only").size() == 1, "400 Hz with dots: %zu marker %zu tone-only", rows(t, "Marker").size(), rows(t, "Tone only").size()); }
    // two tones at once: no marker
    { KeyedAm k; k.fs = fs; k.elems = outerMarker(); k.toneHz = 400; k.tone2Hz = 3000; k.tone2Depth = 0.6; k.depth = 0.6; k.noise = noiseFor(30, 12000, fs); k.stopSec = 8; auto t = go(k, fs, 12.0);
      CHECK(rows(t, "Marker").empty(), "two tones: %zu markers", rows(t, "Marker").size()); }
    // an identification tone at 1020 Hz (Morse-like): no marker
    { KeyedAm k; k.fs = fs; k.elems = {{0.1, 0.1}, {0.3, 0.1}, {0.1, 0.1}, {0.3, 0.5}}; k.toneHz = 1020; k.noise = noiseFor(30, 12000, fs); k.stopSec = 10; auto t = go(k, fs, 12.0);
      CHECK(rows(t, "Marker").empty(), "1020 Hz ident: %zu markers", rows(t, "Marker").size()); }
    // noise alone and a plain carrier
    { KeyedAm k; k.fs = fs; k.elems = {}; k.noise = 1.0; k.startSec = 1e9; auto t = go(k, fs, 60.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    { KeyedAm k; k.fs = fs; k.elems = {}; k.lo = 1200; k.noise = noiseFor(30, 12000, fs); k.startSec = 1e9; k.fadeDepth = 0.3; auto t = go(k, fs, 60.0); CHECK(t.rows.empty(), "carrier: %zu rows", t.rows.size()); }
    printf(fails ? "x40 marker: %d FAILED\n" : "x40 marker: ok\n", fails);
    return fails ? 1 : 0;
}
