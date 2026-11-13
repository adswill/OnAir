// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// mbelib and mbelib-neo are told apart and each frame function is called with its own arguments (source/mbe_dyn.h). mbelib-neo kept the
// names of mbelib 1.3 but changed the arguments; called the old way it wrote over the caller's stack (Discord report 2026-10-23).
// argv[1]: the stand-in library (tests/codec/fake_mbelib.c), argv[2]: "neo" or "classic".
#include "../../core/src/source/mbe_dyn.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using FrameFn = void (DECT2_CALL*)(short* aout, int* errs, int* errs2, char* errStr, char fr[4][24], char d[49], void* cur, void* prev, void* prevEnh, int uvq);

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: test_mbe_abi <library> neo|classic\n"); return 2; }
#ifdef _WIN32
    _putenv_s("DECT2_MBELIB_PATH", argv[1]);
#else
    setenv("DECT2_MBELIB_PATH", argv[1], 1);
#endif
    const bool neo = std::string(argv[2]) == "neo";
    static dect2::mbe::Loader<FrameFn> loader("mbe_processAmbe3600x2450Frame");
    const auto& a = loader.api();
    CHECK(a.ok, "the stand-in loads");
    CHECK(a.neo == neo, "told apart: neo=%d, expected %d", (int)a.neo, (int)neo);
    auto st = std::make_shared<dect2::mbe::State>();
    CHECK(dect2::mbe::initState(a, st), "init through the guard");
    char fr[4][24] = {};
    fr[0][0] = 1; fr[3][23] = 1;
    int16_t pcm[160] = {};
    CHECK(dect2::mbe::decodeFrame(a, st, &fr[0][0], sizeof fr, pcm), "a frame decodes");
    CHECK(pcm[0] == (neo ? 2000 : 1000) && pcm[159] == pcm[0], "called with its own arguments (got %d)", pcm[0]);
    std::printf(fails ? "FAILED\n" : "mbe_abi %s ok\n", argv[2]);
    return fails ? 1 : 0;
}
