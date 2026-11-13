// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR sound through the user-installed DRA+ add-on (cdr_dra.h), tested with fake libraries (tests/cdr/fake_onair_dra.c): no codec, no sound device.
//  1  the loader: no file, a library without the functions, one whose decoder cannot be made (all refused with a reason), a good one, "Look again"
//  2  CdrAudio without the add-on: nothing counted, nothing played, no crash
//  3  the add-on is installed after the audio object was made: sound starts after "Look again"
//  4  CdrAudio with it: 48 real units with extension bytes give one frame each (only the frames themselves are sent in), values reach the output
#include "../../core/src/cdr/cdr_audio.h"
#include "dect2/cdr/cdr_dra.h"
#include "cdr_dra_ext_units.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void setEnv(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

static uint64_t feed(CdrAudio& a, CdrTelemetry& t, size_t unitCount) {
    for (size_t i = 0; i < unitCount; i++) {
        std::vector<cdr::AudioUnit> v(1);
        v[0].data.assign(kDraExtUnits[i], kDraExtUnits[i] + sizeof kDraExtUnits[i]);
        a.play(1, 1, v, 1.f, true, t);
    }
    return t.draPcmFrames;
}

int main() {
    std::string path, prob;
    // ---------------------------------------------------------------- 1 the loader
    setEnv("DECT2_DRA_PATH", "/nonexistent/libonair_dra_missing");
    CHECK(!cdr::draRescan(), "a missing file was accepted");
    CHECK(!cdr::draAvailable(&path, &prob) && prob.empty() && cdr::draApi() == nullptr, "missing: the problem stays empty ('%s')", prob.c_str());
    CHECK(!cdr::draFileNames().empty() && !cdr::draCandidatePaths().empty(), "file names and search paths are listed");
    for (const std::string& n : cdr::draFileNames()) CHECK(n.find("onair_dra") != std::string::npos, "file name %s", n.c_str());
    setEnv("DECT2_DRA_PATH", FAKE_DRA_NOFUNC);
    CHECK(!cdr::draRescan() && !cdr::draAvailable(&path, &prob) && prob.find("DRADecRecvFrame") != std::string::npos && path == FAKE_DRA_NOFUNC, "no functions: '%s'", prob.c_str());
    setEnv("DECT2_DRA_PATH", FAKE_DRA_NOCREATE);
    CHECK(!cdr::draRescan() && !cdr::draAvailable(&path, &prob) && prob.find("no decoder") != std::string::npos, "no decoder: '%s'", prob.c_str());

    // ---------------------------------------------------------------- 2 not installed
    setEnv("DECT2_DRA_PATH", "/nonexistent/libonair_dra_missing");
    cdr::draRescan();
    {
        CdrAudio a(true);
        CdrTelemetry t;
        CHECK(feed(a, t, 48) == 0 && t.draUnitsIn == 48 && t.draUnitsBad == 0, "without the add-on the units are only counted (%llu)", (unsigned long long)t.draPcmFrames);
        a.resetAll();
    }

    // ---------------------------------------------------------------- 3 installed while the program runs ("Look again")
    {
        CdrAudio a(true);
        CdrTelemetry t;
        CHECK(feed(a, t, 4) == 0, "no add-on, no frames");
        setEnv("DECT2_DRA_PATH", FAKE_DRA_GOOD);
        CHECK(cdr::draRescan(), "look again finds it");
        CHECK(feed(a, t, 4) == 4 * 1024, "sound starts after Look again (%llu)", (unsigned long long)t.draPcmFrames);
    }
    // ---------------------------------------------------------------- 4 installed: what the receiver gets
    CHECK(cdr::draAvailable(&path, &prob) && prob.empty() && !path.empty() && cdr::draApi(), "good library: '%s'", prob.c_str());
    {
        CdrAudio a(true);
        CdrTelemetry t;
        const uint64_t n = feed(a, t, 48);
        printf("48 units with extension bytes: %llu samples\n", (unsigned long long)n);
        CHECK(n == 48 * 1024 && t.draUnitsIn == 48 && t.draUnitsBad == 0, "each unit gives one frame (%llu samples, %llu bad)", (unsigned long long)n, (unsigned long long)t.draUnitsBad);
        CHECK(t.draRateHz == 48000, "rate %d", t.draRateHz);
        // junk: refused by the fake, counted bad, no sound
        CdrTelemetry t2;
        std::vector<cdr::AudioUnit> v(1);
        v[0].data.assign(100, 0x55);
        a.play(1, 1, v, 1.f, true, t2);
        CHECK(t2.draUnitsBad == 1 && t2.draPcmFrames == 0, "junk unit: bad %llu", (unsigned long long)t2.draUnitsBad);
    }
    return testSummary("cdr_dra_addon");
}
