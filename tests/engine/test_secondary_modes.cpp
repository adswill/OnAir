// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Modes that may run as the second decoder (Engine::setSecondary): for each of the narrow-channel modes added after the first allow-list, the mode's
// own test signal is written as a recording; it must decode (a) as the first mode alone and (b) as the second mode beside VDL Mode 2 (which only sees
// noise in it), through latestRx2(), with the same kind of result. Modes that stay refused must say why. SEC_MODES="65 67" limits the run.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static std::string writeCapture(int sm, double rate, double secs) {
    SynthConfig cfg;
    auto g = makeModeSynth(sm, cfg, rate);
    if (!g) return "";
    const std::string path = (std::filesystem::temp_directory_path() / ("onair_sec_" + std::to_string(sm) + ".cs8")).string();
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const size_t blk = 1 << 16;
    std::vector<cf32> v(blk);
    std::vector<int8_t> q(2 * blk);
    for (size_t done = 0; done < (size_t)(rate * secs); done += blk) {
        g->generate(v.data(), blk);
        for (size_t i = 0; i < blk; i++) {
            q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].real() * 127)));
            q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[i].imag() * 127)));
        }
        fwrite(q.data(), 1, q.size(), f);
    }
    fclose(f);
    return path;
}

// a mode that counts decoded blocks or says its data is valid; the others (tables of sensors, devices, alerts) are judged by the reports they keep
// delivering with the rate ok, and their one-line summary is printed
static bool decoded(const RxTelemetry& t) { return t.dataValid || t.blocksOk > 0; }

// runs the engine on the capture; asSecond: the mode is the second beside VDL Mode 2; true when it decoded within limitSecs
static bool run(const std::string& path, int sm, double rate, bool asSecond, double limitSecs, int& reports, int& wrong, std::string& summary) {
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = true;
    const ModeTuning* mt = modeTuning(sm);
    TuneSettings t; t.centerHz = 100e6; t.sampleRate = rate; t.bandwidthMhz = mt ? mt->bandwidthMhz : 1;
    if (asSecond) {
        e.setStandard(29);
        if (!e.setSecondaryHz(sm, 100e6)) { printf("  refused: %s\n", e.secondaryNote().c_str()); return false; }
    } else e.setStandard(sm);
    if (!e.start(dev, t, fo)) { CHECK(false, "start"); return false; }
    if (asSecond && !e.secondaryRunning()) { printf("  not running: %s\n", e.secondaryNote().c_str()); e.stop(); return false; }
    RxTelemetry r;
    uint64_t seq = 0;
    reports = wrong = 0;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < limitSecs && !ok) {
        if (asSecond ? e.latestRx2(r, seq) : e.latestRx(r, seq)) {
            seq = r.seq; reports++;
            if (r.standard != sm - 1) wrong++;
            else if (decoded(r)) { ok = true; summary = modeSummary(r); }
            else if (reports >= 40 && r.rateOk) { ok = true; summary = "(reports only) " + modeSummary(r); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (asSecond) CHECK(e.secondaryDropped() == 0, "second mode dropped %llu samples", (unsigned long long)e.secondaryDropped());
    e.stop();
    return ok;
}

int main() {
    std::vector<int> modes = {65, 67, 68, 71, 72, 73, 74, 76, 77, 78, 79, 80};
    if (const char* s = getenv("SEC_MODES")) { modes.clear(); std::istringstream is(s); int m; while (is >> m) modes.push_back(m); }
    // the ones that stay refused, with the reason
    for (int sm : {1, 2, 7, 8, 9, 10, 12, 14, 34, 44, 55, 66, 69, 70}) {
        CHECK(!Engine::secondaryAllowed(sm), "mode %d must stay refused", sm);
        CHECK(Engine::secondaryCheck(29, sm, 100e6, 20e6, 100e6).find("cannot run as a second mode") != std::string::npos, "mode %d: no reason", sm);
    }
    for (int sm : modes) {
        const ModeTuning* mt = modeTuning(sm);
        if (!mt) { CHECK(false, "no tuning for mode %d", sm); continue; }
        // the capture's rate: the mode's own, doubled when the channel would not lie inside the part of the capture the filters keep (secondaryCheck)
        double rate = mt->sampleRate;
        if (!Engine::secondaryCheck(29, sm, 100e6, rate, 100e6).empty()) rate *= 2;
        printf("%s (mode %d), %.2f Msps, allowed as second: %s\n", mt->name, sm, rate / 1e6, Engine::secondaryAllowed(sm) ? "yes" : "no");
        // a refused mode's alone run has no check: by default it is left out (ONAIR_TEST_LONG=1 runs it, as before)
        if (!testLong() && !Engine::secondaryAllowed(sm)) continue;
        const std::string path = writeCapture(sm, rate, 8.0);
        if (path.empty()) { CHECK(false, "no test signal for mode %d", sm); continue; }
        int rep = 0, wrong = 0;
        std::string sum;
        const bool alone = run(path, sm, rate, false, 40, rep, wrong, sum);
        printf("  alone:  %s (%d reports) %s\n", alone ? "decodes" : "no result", rep, sum.c_str());
        if (Engine::secondaryAllowed(sm)) {
            const bool second = run(path, sm, rate, true, 60, rep, wrong, sum);
            printf("  second: %s (%d reports, %d of the wrong standard) %s\n", second ? "decodes" : "no result", rep, wrong, sum.c_str());
            CHECK(alone, "allowed as second but its own signal does not decode alone");
            CHECK(second, "mode %d decodes alone but not as the second mode", sm);
            CHECK(wrong == 0, "%d reports of the wrong standard", wrong);
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    printf(fails ? "secondary modes: %d FAILED\n" : "secondary modes: all passed\n", fails);
    return fails != 0;
}
