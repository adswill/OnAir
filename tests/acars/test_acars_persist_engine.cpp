// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Decoded results through the engine, as the user meets them: ACARS messages decoded from the test signal stay through a switch to
// another mode (QO-100) and back, a retune and a stop / start of the receiver; they go into the results folder, a new engine (the next
// run of OnAir) shows them again before its radio starts, and only the mode's Clear empties them.
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;
#include "../common/test_util.h"

// the newest report of the given active standard, waited for up to secs
static bool reportOf(Engine& e, int act, double secs, RxTelemetry& rx) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t seq = 0;
    bool got = false;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        RxTelemetry t;
        if (e.latestRx(t, seq)) { seq = t.seq; if (t.standard == act) { rx = std::move(t); got = true; } }
        if (got && secs < 0.6) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return got;
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("onair-results-test-" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string d = dir.string();
    size_t kept = 0;
    {
        Engine e;
        DeviceInfo dev;                     // the synthetic source
        TuneSettings t;
        t.centerHz = 131.725e6;
        t.bandwidthMhz = 2;
        t.sampleRate = 2e6;
        t.synth.mode = 18;
        t.synth.snrDb = 30;
        t.synth.modeVal[0] = 3;             // a block every 5 s per aircraft
        FileOptions fo;
        e.setStandard(18);
        CHECK(e.start(dev, t, fo), "engine start");
        RxTelemetry rx;
        CHECK(reportOf(e, 17, 5, rx), "no ACARS report");
        const size_t n0 = rx.acars.messages.size(), a0 = rx.acars.aircraft.size();
        printf("  decoded: %zu messages, %zu aircraft\n", n0, a0);
        CHECK(n0 > 5 && a0 > 0, "%zu messages, %zu aircraft decoded", n0, a0);
        const uint64_t newest = n0 ? rx.acars.messages.front().serial : 0;
        auto has = [](const AcarsTelemetry& a, uint64_t serial) { for (const auto& m : a.messages) if (m.serial == serial) return true; return false; };

        // another mode, then back, as the interface switches modes (stop, the new standard, start)
        e.stop();
        e.setStandard(55);                  // QO-100
        TuneSettings tq = t;
        tq.centerHz = 744e6; tq.synth.mode = 55;
        CHECK(e.start(dev, tq, fo), "engine start in QO-100");
        RxTelemetry q;
        CHECK(reportOf(e, 54, 3, q), "no QO-100 report after the switch");
        e.stop();
        e.setStandard(18);
        CHECK(e.start(dev, t, fo), "engine start in ACARS again");
        CHECK(reportOf(e, 17, 2, rx), "no ACARS report after switching back");
        CHECK(rx.acars.messages.size() >= n0, "%zu messages after QO-100 and back, %zu before", rx.acars.messages.size(), n0);
        CHECK(has(rx.acars, newest), "the newest message before the switch is gone");
        CHECK(rx.acars.aircraft.size() >= a0, "%zu aircraft after the switch, %zu before", rx.acars.aircraft.size(), a0);

        // a retune that restarts the receiver
        TuneSettings t2 = t;
        t2.centerHz = 131.55e6;
        e.retuneReset(t2);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(reportOf(e, 17, 2, rx), "no ACARS report after the retune");
        CHECK(rx.acars.messages.size() >= n0 && has(rx.acars, newest), "%zu messages after a retune, %zu before", rx.acars.messages.size(), n0);
        e.retuneReset(t);

        // stop and start the receiver
        e.stop();
        CHECK(e.start(dev, t, fo), "engine start again");
        CHECK(reportOf(e, 17, 2, rx), "no ACARS report after the restart");
        CHECK(rx.acars.messages.size() >= n0 && has(rx.acars, newest), "%zu messages after stop / start, %zu before", rx.acars.messages.size(), n0);
        e.stop();
        e.refreshIdle();                    // the lists as they are now (a message may have come after the last report)
        CHECK(reportOf(e, 17, 0.5, rx), "no report with the radio stopped");
        kept = rx.acars.messages.size();

        // into the results folder (the second save writes nothing: nothing changed)
        CHECK(e.saveResults(d) >= 1, "nothing saved");
        CHECK(std::filesystem::exists(dir / "acars.bin"), "no acars.bin in %s", d.c_str());
        CHECK(e.saveResults(d) == 0, "an unchanged list was written again");
        {   // what the file holds (a message may have come after the last report)
            std::ifstream f(dir / "acars.bin", std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            AcarsReceiver r;
            AcarsTelemetry at;
            CHECK(r.loadResults(ss.str()) && r.telemetry(at, 0), "acars.bin does not load");
            CHECK(at.messages.size() >= kept, "the file holds %zu messages, the last report %zu", at.messages.size(), kept);
            kept = at.messages.size();
        }
    }
    {   // the next run: the lists are back before the radio starts
        Engine e;
        e.setStandard(18);
        CHECK(e.loadResults(d) >= 1, "nothing loaded");
        RxTelemetry rx;
        CHECK(reportOf(e, 17, 0.5, rx), "no report of the loaded lists while stopped");
        CHECK(rx.acars.messages.size() == kept, "%zu messages loaded, %zu saved", rx.acars.messages.size(), kept);
        // a file of another build: discarded
        {
            FILE* f = fopen((dir / "ais.bin").string().c_str(), "wb");
            if (f) { fputs("OARS not a results file", f); fclose(f); }
        }
        e.loadResults(d);
        CHECK(!std::filesystem::exists(dir / "ais.bin"), "a damaged file was kept");
        // Clear
        e.clearResults(18);
        CHECK(reportOf(e, 17, 0.5, rx) && rx.acars.messages.empty() && rx.acars.aircraft.empty(), "%zu messages after Clear", rx.acars.messages.size());
        CHECK(e.saveResults(d) >= 1, "the cleared list was not saved");
    }
    {   // and the cleared list stays cleared
        Engine e;
        e.setStandard(18);
        e.loadResults(d);
        RxTelemetry rx;
        reportOf(e, 17, 0.5, rx);
        CHECK(rx.acars.messages.empty(), "%zu messages came back after Clear", rx.acars.messages.size());
    }
    {   // the saver thread and its final save (setResultsDir / flushResults, as the interface uses them)
        Engine e;
        e.setResultsDir(d);
        e.flushResults();
        CHECK(std::filesystem::exists(dir / "ais.bin"), "flushResults wrote no files");
    }
    std::filesystem::remove_all(dir, ec);
    printf(fails ? "acars persist engine: %d FAILED\n" : "acars persist engine: all passed\n", fails);
    return fails ? 1 : 0;
}
