// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio x mode glue: what each mode asks of a radio fits together, and a radio too slow for a mode or a scan channel it cannot tune is said
// instead of decoding nothing in silence. The built-in source stands in for the radio (synth.mode 99 sends silence at the rate asked for).
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/scanner.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

// the engine's log after a start at this rate and standard
static std::string startLog(int stdMode, double rate, double bwMhz) {
    Engine e;
    e.setStandard(stdMode);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
    TuneSettings t;
    t.synth.mode = 99; t.sampleRate = rate; t.bandwidthMhz = bwMhz;
    FileOptions fo;
    e.start(dev, t, fo);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    e.stop();
    size_t total = 0;
    std::string s;
    for (const auto& l : e.logSnapshot(total)) s += l + "\n";
    return s;
}

static bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

int main() {
    // every mode: the rate it asks for is one its receiver takes, the filter fits in that rate, the default is in its range, and the channel
    // moved off the DC spike still fits inside the slowest rate the receiver accepts
    for (int sm = 8; sm <= 22; sm++) {
        const ModeTuning* m = modeTuning(sm);
        CHECK(m, "standard %d has a tuning", sm);
        if (!m) continue;
        CHECK(m->sampleRate >= m->minSampleRate, "%s asks for %.0f, below its own minimum %.0f", m->name, m->sampleRate, m->minSampleRate);
        CHECK(m->basebandHz <= m->sampleRate, "%s: filter %.0f wider than its rate %.0f", m->name, m->basebandHz, m->sampleRate);
        CHECK(m->defMhz >= m->minMhz && m->defMhz <= m->maxMhz, "%s: default %.3f outside %.3f-%.3f", m->name, m->defMhz, m->minMhz, m->maxMhz);
        if (m->tuneOffsetHz != 0)
            CHECK(std::fabs(m->tuneOffsetHz) + m->bandwidthMhz * 0.5e6 < 0.45 * m->minSampleRate, "%s: offset %.0f Hz + half of %.0f Hz does not fit in %.0f sps", m->name, m->tuneOffsetHz, m->bandwidthMhz * 1e6, m->minSampleRate);
        CHECK(minSampleRateFor(sm, 8) == m->minSampleRate, "%s: minSampleRateFor follows the tuning", m->name);
    }
    CHECK(std::fabs(minSampleRateFor(0, 8) - 7.9e6) < 1 && std::fabs(minSampleRateFor(1, 1.7) - 7.9e6 * 1.7 / 8) < 1, "DVB-T2 needs 7.9 Msps per 8 MHz");
    CHECK(minSampleRateFor(3, 8) == 6.5e6 && minSampleRateFor(6, 6) == 6.0e6 && minSampleRateFor(6, 8) == 8.0e6 && minSampleRateFor(7, 8) == 500e3 && minSampleRateFor(4, 8) > 1.536e6, "ATSC, ISDB-T (6 Msps per 6 MHz), FM, DAB limits");

    // the engine says it when the radio is too slow, for the standards that had no word for it (DVB-T2 / DVB-T, ATSC, DAB)
    std::string l = startLog(0, 2.56e6, 8);
    CHECK(has(l, "too low for the 8 MHz DVB-T2 / DVB-T channel"), "DVB at 2.56 Msps (an RTL-SDR) is reported:\n%s", l.c_str());
    l = startLog(1, 2.56e6, 1.7);
    CHECK(!has(l, "too low"), "T2-Lite 1.7 MHz at 2.56 Msps is fine:\n%s", l.c_str());
    l = startLog(3, 2.56e6, 6);
    CHECK(has(l, "too low for an ATSC channel"), "ATSC at 2.56 Msps is reported:\n%s", l.c_str());
    l = startLog(3, 8e6, 6);
    CHECK(!has(l, "too low"), "ATSC at 8 Msps is fine:\n%s", l.c_str());
    l = startLog(4, 912e3, 1.7);
    CHECK(has(l, "too low for a DAB ensemble"), "DAB at 912 kHz (an Airspy HF+) is reported:\n%s", l.c_str());
    l = startLog(4, 2.048e6, 1.7);
    CHECK(!has(l, "too low"), "DAB at 2.048 Msps is fine:\n%s", l.c_str());
    l = startLog(9, 2.56e6, 8);
    CHECK(has(l, "too low for DTMB"), "DTMB at 2.56 Msps is reported:\n%s", l.c_str());

    // the scanner: a range outside what the radio is specified for is NOT refused: every channel is asked of the driver
    DeviceInfo pluto; pluto.kind = DeviceInfo::Native; pluto.board = "pluto"; pluto.name = "PlutoSDR"; pluto.maxRateHz = 61.44e6; pluto.minFreqHz = 325e6; pluto.maxFreqHz = 3800e6;
    ScanConfig vhf; vhf.startMHz = 177.5; vhf.stopMHz = 226.5; vhf.stepMHz = 7; vhf.bwMhz = 7;
    std::string err;
    CHECK(Scanner::check(pluto, vhf, err), "a VHF scan on a stock PlutoSDR is accepted (the radio is the limit): %s", err.c_str());
    ScanConfig uhf;
    CHECK(Scanner::check(pluto, uhf, err), "a UHF scan on it is fine");

    // a radio that runs slower than the channels need (it listed more, or listed nothing): the scan says so instead of "no DVB-T2" everywhere
    {
        ScanConfig c; c.startMHz = 474; c.stopMHz = 482; c.stepMHz = 8; c.bwMhz = 8; c.autoBandwidth = false; c.identifyServices = false;
        c.testTune = [](double, TuneSettings& t) { t.synth.mode = 99; t.sampleRate = 4e6; };
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
        Scanner s;
        CHECK(s.start(dev, c, err), "the scan starts");
        for (int i = 0; i < 100 && (s.progress().running || i < 3); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const ScanProgress pr = s.progress();
        printf("  slow radio: %s\n", pr.phase.c_str());
        CHECK(!pr.running && has(pr.phase, "too low for 8 MHz channels") && s.results().empty(), "a 4 Msps radio stops the 8 MHz scan with the reason");
    }
    // channels outside what the radio is specified for are measured like any other: none is skipped, the radio opens on the first channel
    {
        ScanConfig c; c.startMHz = 466; c.stopMHz = 482; c.stepMHz = 8; c.bwMhz = 8; c.autoBandwidth = false; c.identifyServices = false;
        double firstTune = 0;
        c.testTune = [&firstTune](double f, TuneSettings& t) { if (firstTune == 0) firstTune = f; t.synth.mode = 99; t.sampleRate = 10e6; };
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal"; dev.minFreqHz = 470e6; dev.maxFreqHz = 478e6;
        Scanner s;
        CHECK(s.start(dev, c, err), "the scan starts");
        for (int i = 0; i < 400 && (s.progress().running || i < 3); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto res = s.results();
        printf("  partly outside: %s, %zu channels\n", s.progress().phase.c_str(), res.size());
        for (const auto& r : res) printf("    %.1f MHz  %s\n", r.freqMHz, r.note.c_str());
        CHECK(res.size() == 3, "three channels");
        if (res.size() == 3) {
            CHECK(res[0].note != "outside the radio's range" && res[2].note != "outside the radio's range", "466 and 482 MHz are not skipped");
            CHECK(res[0].note == "empty" && res[1].note == "empty" && res[2].note == "empty", "all three are measured");
        }
        CHECK(std::fabs(firstTune - 466) < 0.01, "the radio is opened on the first channel, 466 MHz (%.1f)", firstTune);
    }
    // ---- any frequency: no mode refuses or moves a frequency outside its usual band. The tuning table is open (any value from 1 kHz up), and an
    // engine with a generated signal starts, retunes and runs at 9 MHz and at 6 GHz in every mode, with no refusal, clamp or "out of range" in its log
    for (int sm = 8; sm <= 80; sm++) {
        const ModeTuning* m = modeTuning(sm);
        if (!m) continue;
        CHECK(m->minMhz <= 0.001 && m->maxMhz >= kMaxTuneMhz, "%s: tuning range %.3f-%.3g MHz is not open", m->name, m->minMhz, m->maxMhz);
    }
    {
        struct M { int std, synth; double rate, bw; const char* name; };
        // the standard, the generated signal (99: silence at the rate asked), its rate and channel width
        const M modes[] = {{0, 99, 10e6, 8, "DVB-T/T2"}, {7, 99, 1.2e6, 0.2, "FM"}, {8, 99, 10e6, 8, "DVB-S/S2"}, {13, 13, 4e6, 2, "ADS-B"}, {3, 99, 8e6, 6, "ATSC"}, {4, 99, 2.048e6, 1.7, "DAB"}};
        for (const auto& mm : modes) {
            for (double hz : {9e6, 6e9}) {
                Engine e;
                e.setStandard(mm.std);
                DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
                TuneSettings t;
                t.synth.mode = mm.synth; t.sampleRate = mm.rate; t.bandwidthMhz = mm.bw; t.centerHz = hz;
                FileOptions fo;
                const bool started = e.start(dev, t, fo);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                TuneSettings t2 = t; t2.centerHz = hz == 9e6 ? 6e9 : 9e6;
                const bool re = e.retune(t2);
                t2.centerHz = 1.2345e12;   // a terahertz: still not refused or cut (the radio would be the limit)
                const bool re2 = e.retune(t2);
                e.stop();
                size_t total = 0;
                std::string lg;
                for (const auto& x : e.logSnapshot(total)) lg += x + "\n";
                CHECK(started && re && re2, "%s at %.0f Hz: start %d, retune %d / %d\n%s", mm.name, hz, (int)started, (int)re, (int)re2, lg.c_str());
                CHECK(!has(lg, "out of range") && !has(lg, "outside the") && !has(lg, "refused") && !has(lg, "could not tune") && !has(lg, "clamp"), "%s at %.0f Hz: the log refuses or clamps:\n%s", mm.name, hz, lg.c_str());
            }
        }
    }
    printf(fails ? "radio mode glue: FAILED\n" : "radio mode glue: ok\n");
    return fails ? 1 : 0;
}
