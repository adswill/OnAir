// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The codec library names and the search order are what docs/CODECS.md says: mbelib (AMBE/IMBE: DMR, P25, NXDN, dPMR, D-STAR) and codec2
// (FreeDV, M17). The codecs folders come first, then the program's folder, then the system's places.
#include "dect2/hfdig/hfdig_freedv.h"
#include <algorithm>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"
static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static size_t at(const std::vector<std::string>& v, const std::string& s) { return (size_t)(std::find(v.begin(), v.end(), s) - v.begin()); }
int main() {
    const auto mbe = codecFileNameList(false), c2 = codecFileNameList(true);
#if defined(_WIN32)
    for (const char* n : {"mbelib.dll", "libmbe.dll", "libmbe-1.dll"}) CHECK(has(mbe, n), "mbelib name %s", n);
    for (const char* n : {"codec2.dll", "libcodec2.dll"}) CHECK(has(c2, n), "codec2 name %s", n);
#elif defined(__APPLE__)
    CHECK(has(mbe, "libmbe.dylib"), "mbelib libmbe.dylib");
    CHECK(has(c2, "libcodec2.dylib"), "codec2 libcodec2.dylib");
#else
    CHECK(has(mbe, "libmbe.so") && has(mbe, "libmbe.so.1"), "mbelib libmbe.so, libmbe.so.1");
    CHECK(has(c2, "libcodec2.so") && has(c2, "libcodec2.so.1.2"), "codec2 libcodec2.so, .so.1.2");
#endif
    CHECK(!has(mbe, "codec2.dll") && !has(mbe, "libcodec2.so") && !has(mbe, "libcodec2.dylib"), "mbelib list has no codec2 names");
    CHECK(!has(c2, "libmbe.so") && !has(c2, "libmbe.dll") && !has(c2, "libmbe.dylib"), "codec2 list has no mbelib names");
    // order: every name in the first codecs folder, then the next folder ..., then the system's places
    const auto folders = codecFolderList(), places = codecSearchPlaces(), cand = codecCandidatePaths(false);
    CHECK(!folders.empty(), "codecs folders exist");
    for (size_t i = 0; i < folders.size(); i++) CHECK(i < places.size() && places[i] == folders[i], "place %zu is codecs folder %zu", i, i);
    CHECK(places.size() > folders.size(), "places after the codecs folders");
    for (const auto& f : folders) for (const auto& n : mbe) CHECK(has(cand, f + n), "candidate %s%s", f.c_str(), n.c_str());
    if (!folders.empty() && !mbe.empty()) CHECK(at(cand, folders[0] + mbe[0]) == 0, "first candidate is the first codecs folder + first name");
    if (folders.size() > 1) CHECK(at(cand, folders[0] + mbe.back()) < at(cand, folders[1] + mbe[0]), "first folder before the second");
    // the codecs folders come before any system place
    size_t lastCodecs = 0;
    for (size_t i = 0; i < cand.size(); i++) for (const auto& f : folders) if (cand[i].compare(0, f.size(), f) == 0) lastCodecs = i;
    CHECK(lastCodecs + 1 < cand.size(), "system places follow the codecs folders");
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
