// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/d40/radio_e40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::mt19937 rng(11);
    // aliases
    TgAlias al; const int n = al.importCsv("Decimal,Hex,Mode,Alpha Tag,Description,Tag,Category\r\n101,65,D,\"Dispatch, North\",Police Dispatch,Law Dispatch,Police\r\n205,CD,DE,Fire Ops,Fire,Fire Dispatch,Fire\r\nbad,line\r\n300,Ambulance\r\n");
    CHECK(n == 3 && al.size() == 3); CHECK(al.name(101) == "Dispatch, North" && al.name(300) == "Ambulance" && al.name(999) == "TG 999");
    al.setPriority(205, 5);
    TgFollower f(al, 3.0);
    CHECK(f.onCall(101, 0, false) && f.current() == 101);
    CHECK(!f.onCall(300, 1, false));                       // equal priority waits
    CHECK(f.onCall(205, 1.5, false) && f.current() == 205);  // higher priority pre-empts
    CHECK(!f.onCall(101, 2, false));
    CHECK(f.onCall(101, 6, false));                        // hold over
    CHECK(!f.onCall(300, 100, true));                      // encrypted is skipped
    TgFollower g(al, 3.0, true); g.listen(300); CHECK(!g.onCall(101, 0, false) && g.onCall(300, 1, false));
    // control channel
    std::vector<CcHit> hits = {{851.0125e6, 3, 40, -60, 0.1}, {852.3e6, 90, 6, -70, 0.9}, {853.1e6, 40, 30, -50, 0.5}, {854e6, 2, 0, -40, 0}};
    CHECK(pickControlChannel(hits) == 1);
    for (auto& h : hits) { h.validFrames = 1; h.badFrames = 50; } CHECK(pickControlChannel(hits) == -1);
    // range map: contacts only in the east half-plane out to 300 km, west up to 60 km
    RangeMap rm; std::uniform_real_distribution<double> u(0, 1);
    for (int i = 0; i < 4000; i++) { const double b = u(rng) * 360, r = (b < 180 ? 300 : 60) * std::sqrt(u(rng)); const double lat = 25 + r * std::cos(b * M_PI / 180) / 111.0, lon = 55 + r * std::sin(b * M_PI / 180) / (111.0 * std::cos(25 * M_PI / 180)); rm.add(25, 55, lat, lon); }
    CHECK(rm.p95Km(9) > 250 && rm.p95Km(27) < 80 && rm.maxKm(9) < 310);
    auto bl = rm.blindSectors(); CHECK(!bl.empty()); for (int s : bl) CHECK(s >= 18); CHECK(std::find(bl.begin(), bl.end(), 9) == bl.end());
    CHECK(RangeMap().blindSectors().empty());
    // LoRa: all SF / BW combinations with 3 % clock error
    int ok = 0, tot = 0;
    for (double bw : {125e3, 250e3, 500e3, 62.5e3}) for (int sf = 6; sf <= 12; sf++) { tot++; auto gs = loraGuess(std::pow(2.0, sf) / bw * 1.03, bw * 0.95); if (gs.sf == sf && gs.bwHz == bw && gs.confidence > 0.5) ok++; }
    CHECK(ok == tot);
    CHECK(loraGuess(0.0001, 125e3).sf == 0);                              // SF 3 does not exist
    CHECK(loraSyncWord(0x18, 0x20, 8) == 0x34 && loraSyncWord(0x08, 0x10, 7) == 0x12 && loraSyncWord(0x10 + 1, 0x58 - 1, 9) == 0x2B);
    CHECK(loraSyncWord(5, 0, 8) == -1);                                   // not a multiple of 8
    CHECK(std::string(loraSyncName(0x2B)) == "Meshtastic" && std::string(loraSyncName(0x99)) == "unknown");
    // packet stats
    PacketStats ps; for (int i = 0; i < 60; i++) ps.addMeshPort(i % 3 == 0 ? 3 : i % 3 == 1 ? 1 : 4, i); ps.addMeshPort(77, 61);
    CHECK(ps.count("POSITION") == 20 && ps.count("PORT_77") == 1); CHECK(std::fabs(ps.share("TEXT_MESSAGE") - 20.0 / 61) < 1e-9);
    CHECK(std::fabs(ps.perMinute("POSITION") - 20.0) < 0.5); CHECK(ps.top(2).size() == 2 && ps.top(1)[0].second == 20);
    // pings: 5 pings (4 underdense decays, 1 overdense plateau) on a noise floor, 100 Hz
    std::normal_distribution<float> nd(0, 0.8f); const double fs = 100;
    std::vector<float> e(12000); for (auto& x : e) x = -100 + nd(rng);
    auto addPing = [&](double t0, double peak, double tau, bool plateau) {
        for (int i = 0; i < 700; i++) { const size_t k = (size_t)(t0 * fs) + i; if (k >= e.size()) break;
            const double t = i / fs; double pdb = plateau && t < 2.0 ? peak + 0.0 : peak - 4.343 * (plateau ? t - 2.0 : t) / tau; e[k] = std::max(e[k], (float)(-100 + pdb) + nd(rng) * 0.5f); }
    };
    addPing(10, 25, 0.25, false); addPing(30, 18, 0.4, false); addPing(60, 30, 0.15, false); addPing(80, 22, 0.3, false); addPing(100, 30, 0.5, true);
    auto pg = findPings(e, fs); CHECK(pg.size() == 5);
    for (size_t i = 0; i < pg.size() && i < 4; i++) CHECK(pg[i].underdense);
    if (pg.size() == 5) { CHECK(!pg[4].underdense); CHECK(pg[4].durSec > pg[0].durSec); CHECK(std::fabs(pg[0].startSec - 10) < 0.2); CHECK(std::fabs(pg[0].decayTauSec - 0.25) < 0.08); }
    // noise only: no false pings in 200 runs of 120 s
    int fp = 0; for (int r = 0; r < 200; r++) { std::vector<float> z(12000); for (auto& x : z) x = nd(rng); fp += (int)findPings(z, fs).size(); }
    CHECK(fp == 0);
    // GSM-R channels
    double d, u2; CHECK(gsmrArfcnToHz(955, &d, &u2) && std::fabs(d - 921.2e6) < 1 && std::fabs(u2 - 876.2e6) < 1);
    CHECK(gsmrArfcnToHz(973, &d, &u2) && std::fabs(d - 924.8e6) < 1); CHECK(gsmrArfcnToHz(975, &d) && std::fabs(d - 925.2e6) < 1);
    CHECK(gsmrArfcnToHz(62, &d) && std::fabs(d - 947.4e6) < 1); CHECK(!gsmrArfcnToHz(500, &d)); CHECK(isGsmrBand(960) && !isGsmrBand(62));
    if (!fails) std::printf("radio_e40 ok\n"); return fails ? 1 : 0;
}
