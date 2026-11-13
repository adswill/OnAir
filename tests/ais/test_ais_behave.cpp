// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS behaviour flags and anchor watch (ais_behave.h): simulated vessel reports with GNSS noise and lost reports.
#include "dect2/ais/ais_behave.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static int count(const std::vector<AisBehaveFlag>& v, const char* k) { int n = 0; for (auto& f : v) n += f.kind == k; return n; }
static const double kNmLat = 1.0 / 60;

int main() {
    for (double loss : {0.0, 0.4}) for (double noiseM : {3.0, 12.0}) {
        std::mt19937 r(9); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
        auto jit = [&](double& lat, double& lon) { lat += noiseM * g(r) / 111320; lon += noiseM * g(r) / 64000; };
        AisBehaviour b; std::vector<AisBehaveFlag> out;
        // A and B stop side by side 0.1 nm apart at sea for 50 min (a transfer); C passes by at 12 kn
        for (int t = 0; t < 3000; t += 10) {
            for (int k = 0; k < 3; k++) {
                if (u(r) < loss) continue;
                AisBehaveReport p; p.t = t; p.mmsi = 230000001 + k; p.navStatus = 0;
                if (k < 2) { p.lat = 57.5 + k * 0.1 * kNmLat; p.lon = 20.0; p.sog = 0.3 + 0.2 * u(r); }
                else { p.lat = 57.45 + t * 12.0 / 3600 * kNmLat; p.lon = 20.0; p.sog = 12; }
                jit(p.lat, p.lon); b.add(p, out);
            }
        }
        CHECK(count(out, "rendezvous") == 1, "loss %.1f noise %.0f: one rendezvous %d", loss, noiseM, count(out, "rendezvous"));
        for (auto& f : out) if (f.kind == "rendezvous") CHECK((f.mmsi == 230000001 && f.other == 230000002) || (f.mmsi == 230000002 && f.other == 230000001), "the pair");
        CHECK(count(out, "status_stopped") == 0 || loss > 0, "under 60 min stopped, no status flag");
        // the two stopped "under way using engine" for over an hour: status flag for each
        out.clear();
        for (int t = 3000; t < 6800; t += 10) for (int k = 0; k < 2; k++) { if (u(r) < loss) continue; AisBehaveReport p; p.t = t; p.mmsi = 230000001 + k; p.navStatus = 0; p.lat = 57.5 + k * 0.1 * kNmLat; p.lon = 20; p.sog = 0.1; b.add(p, out); }
        CHECK(count(out, "status_stopped") == 2, "status stopped %d", count(out, "status_stopped"));

        // D loiters: 4 kn in a 0.4 nm circle for 2.5 h; E steams straight at 4 kn
        AisBehaviour b2; out.clear();
        for (int t = 0; t < 9000; t += 30) {
            if (u(r) < loss) continue;
            const double a = t * 4.0 / 3600 / (2 * M_PI * 0.4) * 2 * M_PI;
            AisBehaveReport d; d.t = t; d.mmsi = 230000010; d.navStatus = 0; d.sog = 4; d.lat = 57.6 + 0.4 * kNmLat * std::sin(a); d.lon = 20.2 + 0.4 * kNmLat / std::cos(57.6 * M_PI / 180) * std::cos(a); jit(d.lat, d.lon); b2.add(d, out);
            AisBehaveReport e; e.t = t; e.mmsi = 230000011; e.navStatus = 0; e.sog = 4; e.lat = 57.0 + t * 4.0 / 3600 * kNmLat; e.lon = 21; b2.add(e, out);
        }
        int lo = 0; for (auto& f : out) if (f.kind == "loiter") { lo++; CHECK(f.mmsi == 230000010, "the loiterer"); }
        CHECK(lo == 1, "loiter %d", lo);

        // F at anchor: swings on a 40 m circle for 1 h, then drags 0.6 kn to the south-east
        AisBehaviour b3; out.clear();
        double alat = 57.7, alon = 20.5;
        for (int t = 0; t < 7200; t += 20) {
            if (u(r) < loss) continue;
            AisBehaveReport f; f.t = t; f.mmsi = 230000020; f.navStatus = 1; f.sog = 0.2;
            const double sw = t * 2 * M_PI / 1800;
            f.lat = alat + 40 * std::cos(sw) / 111320; f.lon = alon + 40 * std::sin(sw) / 64000;
            if (t > 3600) { const double m = (t - 3600) * 0.6 * 1852 / 3600; f.lat -= m * 0.707 / 111320; f.lon += m * 0.707 / 64000; f.sog = 0.6; }
            jit(f.lat, f.lon); b3.add(f, out);
        }
        int dr = 0; for (auto& f : out) if (f.kind == "anchor_drag") { dr++; CHECK(f.t > 3600 && f.t < 4800 && f.text.find("towards 1") != std::string::npos, "drag at %.0f: %s", f.t, f.text.c_str()); }
        CHECK(dr == 1, "loss %.1f noise %.0f: one drag alert %d", loss, noiseM, dr);
        // G at anchor that only swings (and a gust moves it 30 m) is never dragging
        AisBehaviour b4; out.clear();
        for (int t = 0; t < 7200; t += 20) { AisBehaveReport f; f.t = t; f.mmsi = 230000021; f.navStatus = 1; f.sog = 0.2; const double sw = t * 2 * M_PI / 1500;
            f.lat = 57.8 + (40 + (t > 5000 ? 30 : 0)) * std::cos(sw) / 111320; f.lon = 20.5 + 40 * std::sin(sw) / 64000; jit(f.lat, f.lon); b4.add(f, out); }
        CHECK(count(out, "anchor_drag") == 0, "swinging only %d", count(out, "anchor_drag"));
    }
    {   // status, draught and destination changes; a moored rendezvous in port is ordinary
        AisBehaviour b; std::vector<AisBehaveFlag> out;
        AisBehaveReport p; p.mmsi = 244000001; p.lat = 54; p.lon = 21; p.navStatus = 5; p.sog = 4.5; p.draughtM = 6.0; p.destination = "KLAIPEDA";
        for (int i = 0; i < 4; i++) { p.t = i * 10; b.add(p, out); }
        CHECK(count(out, "status_moving") == 1, "moored but moving %d", count(out, "status_moving"));
        p.sog = 0; p.t = 100; p.draughtM = 9.2; p.destination = "Klaipeda"; b.add(p, out);
        CHECK(count(out, "draught") == 1 && count(out, "destination") == 0, "draught; same destination in other case");
        p.t = 200; p.destination = "GDANSK"; b.add(p, out);
        CHECK(count(out, "destination") == 1, "destination change");
        // two ships moored at that berth: not a rendezvous; also aids are ignored
        out.clear();
        for (int t = 300; t < 3000; t += 30) for (int k = 0; k < 2; k++) { AisBehaveReport q; q.t = t; q.mmsi = 244000010 + k; q.navStatus = 0; q.sog = 0.2; q.lat = 54 + k * 0.001; q.lon = 21; b.add(q, out); }
        AisBehaveReport aid; aid.isShip = false; aid.mmsi = 992441234; b.add(aid, out);
        CHECK(count(out, "rendezvous") == 0, "in port %d", count(out, "rendezvous"));
    }
    return testSummary("ais_behave");
}
