// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The runtime loader of the user-installed mbelib (P25 IMBE, D-STAR / NXDN AMBE): with no library found the decoders say so; pointed at a fake
// library (tests/p25/fake_mbelib.c, no codec) through DECT2_MBELIB_PATH they load it, and decoding fills 160 samples per frame.
#include "dect2/p25/p25_imbe.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

int main() {
    setEnv("DECT2_NATIVE_LIBDIR", "/nonexistent");   // no installed library may be picked up
    setEnv("DECT2_MBELIB_PATH", "");
    std::string p1, p2, p3;
    CHECK(!p25ImbeAvailable(&p1) && !dstarAmbeAvailable(&p2) && !nxdnAmbeAvailable(&p3), "a vocoder was found with no library installed");
    {   P25Imbe i0; DstarAmbe d0; NxdnAmbe n0; CHECK(!i0.ok() && !d0.ok() && !n0.ok(), "decoders report ok with no library"); }

    {   // test hook of the loader: the library is taken from DECT2_NATIVE_LIBDIR, by the file name in DECT2_MBELIB_PATH
        std::string full = FAKE_MBELIB_PATH;
        setEnv("DECT2_NATIVE_LIBDIR", full.substr(0, full.find_last_of("/\\")).c_str());
        setEnv("DECT2_MBELIB_PATH", full.c_str());
    }
    const bool a = p25ImbeRescan(), b = dstarAmbeRescan(), c = nxdnAmbeRescan();
    CHECK(a && b && c, "rescan: imbe %d dstar %d nxdn %d", a, b, c);
    p25ImbeAvailable(&p1); dstarAmbeAvailable(&p2); nxdnAmbeAvailable(&p3);
    CHECK(!p1.empty() && p1.find("fake_mbelib") != std::string::npos && p2 == p1 && p3 == p1, "path: %s / %s / %s", p1.c_str(), p2.c_str(), p3.c_str());

    P25Imbe imbe; DstarAmbe dstar; NxdnAmbe nxdn;
    CHECK(imbe.ok() && dstar.ok() && nxdn.ok(), "decoders");
    int16_t pcm[160];
    char fr[8][23] = {};
    uint8_t bits[72] = {};
    int nonzero = 0;
    for (int n = 0; n < 10; n++) {
        memset(pcm, 0x55, sizeof pcm); imbe.decode(fr, pcm);
        for (int16_t s : pcm) nonzero += s != 0x5555;
        memset(pcm, 0x55, sizeof pcm); dstar.decode(bits, pcm);
        for (int16_t s : pcm) nonzero += s != 0x5555;
        memset(pcm, 0x55, sizeof pcm); nxdn.decode(bits, pcm);
        for (int16_t s : pcm) nonzero += s != 0x5555;
    }
    CHECK(nonzero == 10 * 3 * 160, "every decode fills its 160 samples (%d of %d)", nonzero, 10 * 3 * 160);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
