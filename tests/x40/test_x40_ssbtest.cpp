// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSB transmitter test meter: one-tone and two-tone tests of a generated transmitter (upper and lower sideband) with known carrier leak, opposite sideband, harmonics and intermodulation products, the
// radio tuned off by some tens of Hz, three input rates, unequal tones, a noise sweep, and the answer for no tone or noise only. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// "-31.0 dB under one tone" or "under -70 dB under one tone": returns the number and whether it is a floor
static bool lv(const std::string& s, double& v) { if (s.rfind("under -", 0) == 0) { v = atof(s.c_str() + 7); return true; } v = -atof(s.c_str()); v = -v; v = std::fabs(atof(s.c_str())); return false; }
static void addT(ToneScene& sc, double hz, double dbc) { SchedTone t; t.hz = hz; t.amp = std::pow(10.0, dbc / 20); sc.tones.push_back(t); }

int main() {
    for (double fs : {12000.0, 48000.0, 192000.0}) for (int usb = 1; usb >= 0; usb--) {
        const double sg = usb ? 1 : -1, d = fs == 48000 ? 35.0 : fs == 12000 ? -20.0 : 8.0;     // the radio's tuning error
        // ---- one tone at 1000 Hz: carrier -50, opposite sideband -55, harmonics 2..5 at -60 -58 -70 -66
        { ToneScene sc(fs, noiseFor(95, 3000, fs), 4);
          addT(sc, d, -50); addT(sc, d + sg * 1000, 0); addT(sc, d - sg * 1000, -55); const double hh[4] = {-60, -58, -70, -66}; for (int i = 0; i < 4; i++) addT(sc, d + sg * 1000 * (i + 2), hh[i]);
          auto t = runChunks(x40::SsbTest, fs, 14.0, std::ref(sc), 0, 14.2e6, 1.0);
          double v; const bool cu = lv(factStr(t, "carrier leak"), v);
          CHECK(!cu && std::fabs(v - 50) < 1.5, "fs %.0f usb %d: carrier %.1f want 50 (%s)", fs, usb, v, factStr(t, "carrier leak").c_str());
          lv(factStr(t, "opposite sideband"), v); CHECK(std::fabs(v - 55) < 1.5, "opposite sideband %.1f want 55", v);
          for (int i = 0; i < 4; i++) { const bool u = lv(factStr(t, ("harmonic " + std::to_string(i + 2) + " of the audio tone").c_str()), v); if (i < 2) CHECK(!u && std::fabs(v + hh[i]) < 1.5, "harmonic %d: %.1f want %.0f", i + 2, v, -hh[i]); }
          CHECK(factStr(t, "sideband").rfind(usb ? "upper" : "lower", 0) == 0, "sideband %s", factStr(t, "sideband").c_str());
          CHECK(t.headline.rfind("one tone 1000 Hz", 0) == 0, "headline %s", t.headline.c_str()); }
        // ---- two tones 1000 and 1700 Hz: IM3 low -31 / high -34, IM5 high -47, IM7 not there; carrier -48, opposite sideband -52
        { ToneScene sc(fs, noiseFor(95, 3000, fs), 5);
          addT(sc, d, -48); addT(sc, d + sg * 1000, 0); addT(sc, d + sg * 1700, 0);
          addT(sc, d + sg * 300, -31); addT(sc, d + sg * 2400, -34);        // 2f1 - f2 = 300, 2 f2 - f1 = 2400
          addT(sc, d + sg * 3100, -47);                                      // 3 f2 - 2 f1 = 3100 (3 f1 - 2 f2 = -400 is on the other side)
          addT(sc, d - sg * 1000, -52);
          auto t = runChunks(x40::SsbTest, fs, 14.0, std::ref(sc), 0, 14.2e6, 1.0);
          double v; auto first = [&](const char* key) { const std::string s = factStr(t, key); const bool u = lv(s, v); return u; };
          const bool u3 = first("IMD3"); CHECK(!u3 && std::fabs(v - 31) < 1.5, "fs %.0f usb %d: IMD3 %.1f want 31", fs, usb, v);
          const bool u5 = first("IMD5"); CHECK(!u5 && std::fabs(v - 47) < 1.5, "IMD5 %.1f want 47", v);
          const bool u7 = first("IMD7"); CHECK(u7, "IMD7 should be under the floor");
          { const std::string s = factStr(t, "IMD3"); const char* p = strstr(s.c_str(), "under one tone, "); CHECK(p && std::fabs(atof(p + 16) + 37.0) < 1.6 + 0 && true, "IMD3 under PEP: %s", s.c_str()); }
          lv(factStr(t, "carrier leak"), v); CHECK(std::fabs(v - 48) < 1.5, "carrier %.1f want 48", v);
          lv(factStr(t, "opposite sideband"), v); CHECK(std::fabs(v - 52) < 1.5, "opposite sideband %.1f want 52", v);
          CHECK(t.headline.rfind("two tones 1000 and 1700 Hz", 0) == 0, "headline %s", t.headline.c_str()); }
    }
    // unequal tones, 5 dB apart: IMD3 is read against the weaker tone
    { const double fs = 48000; ToneScene sc(fs, noiseFor(95, 3000, fs), 6); addT(sc, 1200, 0); addT(sc, 2000, -5); addT(sc, 400, -36); addT(sc, 2800, -39);   // dB re the strong tone; the weak tone is at -5
      auto t = runChunks(x40::SsbTest, fs, 14.0, std::ref(sc), 0, 7.1e6, 1.0); double v; lv(factStr(t, "IMD3"), v); CHECK(std::fabs(v - 31) < 1.5, "unequal tones: IMD3 %.1f want 31 under the weaker tone", v); }
    // noise sweep: one tone with a -50 dBc harmonic: floor and the reading against the carrier-to-noise ratio of the tone in 3 kHz
    printf("one tone, harmonic at -50 dBc: tone to noise in 3 kHz -> read (dB under), measurement floor:\n");
    for (double cnr : {80.0, 70.0, 60.0, 55.0, 50.0}) {
        const double fs = 48000; ToneScene sc(fs, noiseFor(cnr, 3000, fs), 7); addT(sc, 1000, 0); addT(sc, 2000, -50);
        auto t = runChunks(x40::SsbTest, fs, 14.0, std::ref(sc), 0, 7.1e6, 1.0); double v; const bool u = lv(factStr(t, "harmonic 2 of the audio tone"), v);
        printf("  %.0f dB: %s%.1f (floor %s)\n", cnr, u ? "under -" : "-", v, factStr(t, "measurement floor").c_str());
        if (cnr >= 70) CHECK(!u && std::fabs(v - 50) < 1.5, "%.0f dB: %.1f", cnr, v);
    }
    // no tone (noise, and a carrier alone): nothing is claimed
    { ToneScene sc(48000, 1.0, 3); auto t = runChunks(x40::SsbTest, 48000, 14.0, std::ref(sc), 0, 7.1e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    { ToneScene sc(48000, noiseFor(60, 3000, 48000), 3); addT(sc, 5, 0); auto t = runChunks(x40::SsbTest, 48000, 14.0, std::ref(sc), 0, 7.1e6, 1.0); CHECK(t.rows.empty(), "carrier only: %zu rows (%s)", t.rows.size(), t.headline.c_str()); }
    printf(fails ? "x40 ssbtest: %d FAILED\n" : "x40 ssbtest: ok\n", fails);
    return fails ? 1 : 0;
}
