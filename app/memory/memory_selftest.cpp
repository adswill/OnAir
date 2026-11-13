// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// --memory-selftest: applies memories to the receiver of a hidden run and checks the state (mode, sub-mode, frequency, options, gains), so the
// recall is tested through the real App without drawing anything or touching a radio. tests/app/run_memory_recall.cmake runs it from a
// renamed copy of the program with its own data and settings folders. Prints one line per check; the exit code is the number of failures.
#include "memory_options.h"
#include <cmath>
#include <cstdio>

namespace {
int fails = 0;
void check(bool ok, const char* what) { if (!ok) { fails++; printf("FAIL: %s\n", what); } }

const ModeDef* modeOfFamily(int family, const char* subId = nullptr) {
    for (const auto& m : allModes()) if (m.family == family && (!subId || (m.subId && !strcmp(m.subId, subId)))) return &m;
    return nullptr;
}
dect2::Memory mem(const ModeDef& md, uint64_t hz) {
    dect2::Memory m;
    m.name = md.name; m.freqHz = hz; m.mode = memoryModeKey(md); m.family = md.family; m.subId = md.subId ? md.subId : "";
    return m;
}
}  // namespace

int memorySelfTest(App& a) {
    fails = 0;
    std::string note;
    check(!a.engine.running(), "the self test runs without a radio");
    const ModeDef* fm = modeOfFamily(5);
    const ModeDef* gen = modeOfFamily(55);
    const ModeDef* dtmb = modeOfFamily(7);
    const ModeDef* pager = modeOfFamily(23);
    check(fm && gen && dtmb && pager, "the modes the test needs are in this build");
    if (!(fm && gen && dtmb && pager)) return fails;

    // FM: frequency, options, gains
    {
        const int dtmbBefore = a.dtmbBwMhz;   // the settings of an earlier run may have set it
        dect2::Memory m = mem(*fm, 100100000);
        m.options["fmDeemph"] = "75"; m.options["fmMono"] = "1"; m.options["fmChanKhz"] = "180"; m.options["dtmbBwMhz"] = dtmbBefore == 6 ? "8" : "6";   // the last one belongs to another mode: ignored
        m.hasGain = true; m.lna = 8; m.vga = 16; m.amp = false; m.gainDb = 12;
        check(memoryRecallApp(a, m, true, note), "recall FM");
        check(a.family == 5 && a.fmMode, "FM: the mode is selected");
        check(std::fabs(a.freqMhz - 100.1) < 1e-9 && std::llround(a.tune.centerHz) == 100100000, "FM: frequency");
        check(a.fmDeemph == 75 && a.fmMono && a.fmChanKhz == 180, "FM: options");
        check(a.tune.lnaDb == 8 && a.tune.vgaDb == 16 && !a.tune.ampOn && a.tune.gainDb == 12, "FM: gains");
        check(a.dtmbBwMhz == dtmbBefore, "an option of another mode is not applied");
        const dect2::Memory c = memoryCapture(a);
        check(c.freqHz == 100100000 && c.mode == m.mode && c.options.at("fmDeemph") == "75" && c.options.count("dtmbBwMhz") == 0, "capture gives back what was applied");
    }
    // a frequency only keeps the mode and the options
    {
        dect2::Memory m = mem(*gen, 145500000);
        m.options["fmDeemph"] = "50";
        check(memoryRecallApp(a, m, false, note), "recall frequency only");
        check(a.family == 5 && std::fabs(a.freqMhz - 145.5) < 1e-9 && a.fmDeemph == 75, "frequency only: mode and options stay");
    }
    // the general receiver with its own state
    {
        dect2::Memory m = mem(*gen, 121500000);
        m.options["genDemod"] = "2"; m.options["genSquelch"] = "level"; m.options["genSquelchDb"] = "-72";
        check(memoryRecallApp(a, m, true, note), "recall general receiver");
        check(a.family == 55 && std::fabs(a.freqMhz - 121.5) < 1e-9, "general: mode and frequency");
        const dect2::Memory c = memoryCapture(a);
        check(c.freqHz == 121500000 && c.options.at("genDemod") == "2" && c.options.at("genSquelch") == "level" && c.options.at("genSquelchDb") == "-72", "general: options read back");
    }
    // DTMB 6 MHz, and a sub-mode
    {
        dect2::Memory m = mem(*dtmb, 474000000);
        m.options["dtmbBwMhz"] = "6";
        check(memoryRecallApp(a, m, true, note) && a.family == 7 && a.dtmbBwMhz == 6, "DTMB: width");
        dect2::Memory p = mem(*pager, 153350000);
        p.options["pagerNarrow"] = "1";
        check(memoryRecallApp(a, p, true, note) && a.family == 23 && a.pagerNarrow, "pager: mode and option");
        check(a.sub == subIndexOf(pager->subId) && std::fabs(a.freqMhz - 153.35) < 1e-9, "pager: sub-mode and frequency");
        check(memoryCapture(a).subId == (pager->subId ? pager->subId : ""), "capture keeps the sub-mode id");
    }
    // a mode this build lacks changes nothing
    {
        const int fam = a.family; const double f = a.freqMhz;
        dect2::Memory m; m.name = "x"; m.freqHz = 50000000; m.mode = "no-such-mode";
        check(!memoryRecallApp(a, m, true, note) && !note.empty() && a.family == fam && a.freqMhz == f, "unknown mode: refused, nothing changed");
    }
    // the bank on its own file (the data folder is the test's)
    {
        dect2::MemoryBank& b = memoryBank();
        b.clear();
        b.add(mem(*fm, 93900000));
        memoryChanged();
        dect2::MemoryBank again;
        check(again.load(memoryFilePath()).loaded == 1 && memoryFilePath().find("memories.json") != std::string::npos, "the bank is saved as memories.json");
    }
    printf(fails ? "memory selftest: %d failed\n" : "memory selftest: all passed\n", fails);
    return fails;
}
