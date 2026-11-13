// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The channel scan taking a radio over from the receiver, through the fake SoapySDR radio (tests/source/soapy_testdriver.cpp) set to behave as some
// real radios do: a stream started the moment the previous one ended never sends a sample. The scan tab stops the receiver and starts the
// scan straight away, so the scan's first open hit that, and the first scan went through every channel seeing nothing (a second scan, with
// the radio idle for a while, worked). The scan must notice that the radio is silent, open it again, and measure and lock on the first run.
#ifdef _WIN32
#include <stdlib.h>
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#include "dect2/engine/scanner.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char** argv) {
    if (argc > 1) setenv("SOAPY_SDR_PLUGIN_PATH", argv[1], 1);
    setenv("ONAIR_TEST_PACE", "0.5", 1);              // half speed: independent of how fast the machine is
    setenv("ONAIR_TEST_DEAD_REOPEN_MS", "300", 1);    // a stream started within 0.3 s of the last one ending stays silent
    std::string err;
    DeviceInfo radio;
    bool found = false;
    for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest") { radio = d; found = true; }
    CHECK(found, "fake radio not listed (%s)", err.c_str());
    if (!found) { printf("scan soapy: FAILED\n"); return 1; }

    Engine rx;   // the receiver, running on the radio when the scan starts
    rx.setStandard(1);
    TuneSettings t; t.bandwidthMhz = 8; t.sampleRate = 10e6; t.centerHz = 522e6; t.gainDb = 30;
    FileOptions fo;
    CHECK(rx.start(radio, t, fo), "the receiver starts");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    ScanConfig cfg;
    cfg.startMHz = 474; cfg.stopMHz = 482; cfg.stepMHz = 8; cfg.bwMhz = 8;   // the fake radio sends its DVB-T2 signal on every channel
    cfg.identifyServices = false;
    cfg.lockTimeoutSec = 15;   // the signal runs at half speed
    cfg.tune = t;
    // the scan tab's order: the receiver lets go of the radio, and the scan opens it at once
    rx.stop();
    Scanner s;
    CHECK(s.start(radio, cfg, err), "the scan starts (%s)", err.c_str());
    for (int i = 0; i < 1200 && (s.progress().running || i < 5); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const ScanProgress pr = s.progress();
    const auto res = s.results();
    printf("  scan: %s (silent streams the radio started: %s)\n", pr.phase.c_str(), getenv("ONAIR_TEST_DEAD_STREAMS") ? getenv("ONAIR_TEST_DEAD_STREAMS") : "0");
    CHECK(getenv("ONAIR_TEST_DEAD_STREAMS"), "the radio never started a silent stream: the test does not test the hand-over");
    CHECK(!pr.running && pr.phase == "finished", "the scan finished");
    CHECK(res.size() == 2, "two channels were scanned (%zu)", res.size());
    for (const auto& r : res) {
        printf("    %.1f MHz  level %.1f dBFS  occupied %d locked %d seen %d  %s\n", r.freqMHz, r.levelDbfs, (int)r.occupied, (int)r.t2, (int)r.seen, r.note.c_str());
        CHECK(r.t2 && r.seen, "%.1f MHz was not locked on the first scan", r.freqMHz);
    }
    s.stop();
    // and the receiver gets the radio back afterwards
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(rx.start(radio, t, fo), "the receiver starts again after the scan");
    rx.stop();
    printf(fails ? "scan soapy: FAILED\n" : "scan soapy: ok\n");
    return fails ? 1 : 0;
}
