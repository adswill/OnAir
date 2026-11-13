// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DSC bands, the marine band table, the scanner, parallel monitoring in one capture, the ship table (AIS learning, import, export) and KML / GPX.
#include "../data/marine/rf_util.h"
#include "dect2/marine/marine_bands.h"
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_dsc_geo.h"
#include "dect2/marine/marine_mmsi.h"
#include <cstdio>
#include <set>
using namespace dect2;
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // ---- the frequency lists
    {
        const auto& f = dscFrequencies();
        std::set<long> seen; int distress = 0;
        for (const auto& e : f) { CHECK(seen.insert((long)(e.hz * 10)).second, "duplicate %.1f", e.hz); if (e.group == DscGroup::Distress) distress++; }
        CHECK(distress == 6, "six MF/HF distress channels, got %d", distress);
        // 6 MF/HF distress + VHF ch70 + 9 worldwide ship + 9 coast + 8+8 series A + 8+8 series B + 2 on 455 kHz = 59
        CHECK(f.size() == 59, "all calling channels: %zu", f.size());
        bool has2177 = false, has4208 = false, has25208 = false, has4209 = false;
        for (const auto& e : f) { if (e.hz == 2177e3) has2177 = true; if (e.hz == 4208e3) has4208 = true; if (e.hz == 25208.5e3) has25208 = true; if (e.hz == 4209e3) has4209 = true; }
        CHECK(has2177 && has4208 && has25208 && has4209, "routine channels listed");
        int n = 0;
        for (const auto& b : marineBands()) { CHECK(b.name && b.protocols && b.band, "band row text"); if (b.family == 15 && b.sub && std::string(b.sub) == "dsc") n++; }
        CHECK(n >= 15, "DSC rows in the band table: %d", n);
    }
    // ---- the scanner
    {
        DscScan sc;
        sc.setList({{2187.5e3, 2}, {8414.5e3, 1}, {12577e3, 1}});
        sc.setDwell(4, 20);
        CHECK(sc.slots() == 4, "weighted slots %zu", sc.slots());
        double hz = 0, t = 0;
        std::vector<double> order;
        CHECK(sc.update(t, false, 0, hz), "first slot");
        order.push_back(hz);
        for (int i = 0; i < 40; i++) { t += 1; if (sc.update(t, false, 0, hz)) order.push_back(hz); }
        CHECK(order.size() >= 9 && order[0] == 2187.5e3 && order[1] == 8414.5e3 && order[2] == 12577e3 && order[3] == 2187.5e3 && order[4] == 2187.5e3, "order of channels: %zu", order.size());
        // a locked decoder keeps the radio; a call keeps it for the hold time
        DscScan s2; s2.setList({{1e6, 1}, {2e6, 1}}); s2.setDwell(4, 20);
        t = 0; s2.update(t, false, 0, hz);
        bool moved = false;
        for (int i = 0; i < 30; i++) { t += 1; if (s2.update(t, true, 0, hz)) moved = true; }
        CHECK(!moved, "locked: stays");
        t += 1; s2.update(t, false, t, hz);
        moved = false;
        for (int i = 0; i < 15; i++) { t += 1; if (s2.update(t, false, t - i - 1 < 0 ? 0 : t - i - 1, hz)) moved = true; }
        CHECK(!moved || s2.current() > 0, "hold after a call");
        std::vector<double> cap = dscInCapture({4207.5e3, 8414.5e3, 6312e3, 4208e3}, 4208e3, 100e3);
        CHECK(cap.size() == 2, "channels inside the capture: %zu", cap.size());
    }
    // ---- parallel monitoring: two DSC transmitters 45 kHz apart in one 250 kHz capture
    {
        SynthConfig cfg; cfg.mode = 17; cfg.snrDb = 30; cfg.modeOpt[0] = 2;
        const double rate = 250000;
        auto a = makeMarineSynthAt(cfg, rate, -marineTuning().tuneOffsetHz);
        auto b = makeMarineSynthAt(cfg, rate, -marineTuning().tuneOffsetHz + 45000);
        MarineReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(-marineTuning().tuneOffsetHz);
        rx.setService(0);
        rx.setFrequencyHz(8414.5e3);
        rx.setParallelDsc({8414.5e3, 8414.5e3 + 45000, 8414.5e3 + 400e3});
        const size_t total = (size_t)(24 * rate);
        std::vector<cf32> x(65536), y(65536);
        uint64_t last = 0; MarineTelemetry t, tl;
        for (size_t done = 0; done < total; done += x.size()) {
            const size_t n = std::min(x.size(), total - done);
            a->generate(x.data(), n); b->generate(y.data(), n);
            for (size_t i = 0; i < n; i++) x[i] += y[i];
            rx.feed(x.data(), n);
            while (rx.telemetry(t, last)) { last = t.seq; tl = t; }
        }
        int main_ = 0, par = 0;
        for (const auto& c : tl.dsc) if (c.eccOk) { if (std::fabs(c.freqHz - 8414.5e3) < 1) main_++; else if (std::fabs(c.freqHz - (8414.5e3 + 45000)) < 1) par++; }
        printf("parallel: %d calls on the dial, %d on +45 kHz, %zu channels reported\n", main_, par, tl.parallel.size());
        CHECK(main_ >= 1 && par >= 1, "both channels decoded: %d %d", main_, par);
        CHECK(tl.parallel.size() == 1, "the channel outside the capture is dropped: %zu", tl.parallel.size());
    }
    // ---- ship table
    {
        ShipDb db;
        db.learn("232123456", "MV EXAMPLE", "MABC1", "Cargo");
        ShipRec r;
        CHECK(db.find("232123456", r) && r.name == "MV EXAMPLE" && r.callsign == "MABC1" && r.country == "United Kingdom" && r.source == 0, "learned");
        CHECK(db.takeDirty() && !db.takeDirty(), "dirty once");
        const std::string csv = "MMSI,Name,Callsign,Type,Country,Station,Location,Lat,Lon\n232123456,\"Imported, Name\",MXYZ,Tanker,GB,,,,\n002320064,,,Coast station,GB,Solent Coastguard,Lee-on-Solent,50.80,-1.20\n";
        CHECK(db.importText(csv) == 2, "import csv");
        CHECK(db.find("232123456", r) && r.name == "Imported, Name" && r.callsign == "MXYZ" && r.source == 1, "import wins");
        db.learn("232123456", "AIS NAME", "AIS", "Cargo");
        CHECK(db.name("232123456") == "Imported, Name", "AIS never replaces an import");
        CHECK(db.name("002320064") == "Solent Coastguard, Lee-on-Solent" && db.positioned().size() == 1, "coast station");
        ShipDb db2;
        CHECK(db2.importText(db.exportTsv()) == 2 && db2.name("002320064") == db.name("002320064"), "export round trip");
        ShipDb db3;
        CHECK(db3.importText("232123456 \n") == 0, "garbage");
        CHECK(db3.importText("232123456;Ship;CALL;Tug;;;;;\n") == 1 && db3.name("232123456") == "Ship", "semicolons, no header");
        // positional file without header, tabs, a legacy two-column list
        CHECK(db3.importText("219000123\tNordic Star\n") == 1, "two columns");
    }
    // ---- geo export
    {
        std::vector<DscCall> calls;
        for (int i = 0; i < 3; i++) {
            DscCall c; c.eccOk = true; c.hasPos = true; c.lat = 50.8 + i * 0.1; c.lon = -1.3; c.fromMmsi = "232123456"; c.rxTime = 1700000000 + i * 600; c.kind = "Distress alert"; c.distress = true; c.natureName = "Fire & <smoke>";
            calls.push_back(c);
        }
        DscCall bad = calls[0]; bad.eccOk = false; bad.rxTime += 5000; calls.push_back(bad);
        auto fx = dscFixes(calls);
        CHECK(fx.size() == 3, "fixes %zu", fx.size());
        CHECK(dscAddFixes(fx, calls) == 0, "no duplicates");
        ShipDb db; db.learn("232123456", "MV EXAMPLE", "", "");
        db.importText("002320064\tSolent Coastguard\t\tCoast\t\t\t\t50.8\t-1.2\n");
        const std::string k = dscKml(fx, db), g = dscGpx(fx, db);
        CHECK(k.find("<LineString>") != std::string::npos && k.find("MV EXAMPLE") != std::string::npos && k.find("Fire &amp; &lt;smoke&gt;") != std::string::npos && k.find("Solent Coastguard") != std::string::npos, "kml");
        CHECK(g.find("<trk>") != std::string::npos && g.find("<wpt lat=\"50.800000\"") != std::string::npos && g.find("2023-11-14T22:13:20Z") != std::string::npos, "gpx");
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
