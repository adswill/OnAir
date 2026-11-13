// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MeshCore radio presets (api.meshcore.nz): the committed sample document parses to the same table the generator wrote, a preset maps to the
// right LoRa parameters, and the saved-file loader refuses malformed files (and falls back to the built-in table).
#include "dect2/mesh/mesh_presets.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {}); }
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }

int main(int argc, char** argv) {
    const char* env = getenv("ONAIR_TEST_DATA");
    const std::string dir = argc > 1 ? argv[1] : env ? env : "tests/data";
    const std::string sample = slurp(dir + "/meshcore_config_sample.json");
    CHECK(!sample.empty(), "sample missing");

    // the sample parses and equals the built-in table (generated from it)
    std::vector<MeshCorePresetEntry> v;
    CHECK(parseMeshCoreConfig(sample, v), "sample parse");
    const auto b = meshCoreBuiltinPresets();
    CHECK(v.size() == b.size() && v.size() >= 20, "count %zu vs built-in %zu", v.size(), b.size());
    for (size_t i = 0; i < v.size() && i < b.size(); i++)
        CHECK(v[i].title == b[i].title && v[i].region == b[i].region && v[i].note == b[i].note && std::fabs(v[i].mhz - b[i].mhz) < 1e-6 &&
              std::fabs(v[i].bwKhz - b[i].bwKhz) < 1e-6 && v[i].sf == b[i].sf && v[i].cr == b[i].cr, "row %zu differs from the built-in table", i);

    // mapping
    for (const auto& e : v) if (e.title == "EU/UK (Narrow)") {
        CHECK(e.region == "Europe" && std::fabs(e.mhz - 869.618) < 1e-9 && e.bwKhz == 62.5 && e.sf == 8 && e.cr == 8, "EU narrow fields");
        const MeshLoraSettings s = meshCorePresetToLora(e);
        CHECK(std::fabs(s.freqHz - 869.618e6) < 1 && s.bwHz == 62500 && s.sf == 8 && s.cr == 8 && s.syncWord == 0x12 && s.preamble == 32 && !s.ldro, "EU narrow lora");
        CHECK(meshCorePresetSummary(e) == "869.618 MHz / 62.5 kHz / SF8 / CR 4/8", "summary '%s'", meshCorePresetSummary(e).c_str());
    }
    for (const auto& e : v) if (e.title == "EU/UK (Deprecated)") {
        const MeshLoraSettings s = meshCorePresetToLora(e);
        CHECK(s.sf == 11 && s.bwHz == 250000 && s.preamble == 16 && !s.ldro, "SF11/250 lora (symbol 8 ms)");
    }
    for (const auto& e : v) if (e.title == "Czech Republic (Narrow)") CHECK(e.note == "2B", "note '%s'", e.note.c_str());
    for (const auto& e : v) CHECK(e.region == meshCoreRegionOf(e.title), "region rule differs for %s", e.title.c_str());
    const auto regs = meshCoreRegions(v);
    CHECK(regs.size() == 5 && regs[0] == "Oceania", "regions %zu", regs.size());

    // the saved file: valid, then malformed variants
    const std::string f = dir + "/meshcore_presets_tmp.json";
    put(f, sample);
    std::vector<MeshCorePresetEntry> o;
    CHECK(loadMeshCorePresetFile(f, o) && o.size() == v.size(), "valid file loads");
    const char* bad[] = {"", "not json", "{\"config\":{}}", "{\"config\":{\"suggested_radio_settings\":{\"entries\":[]}}}",
                         "{\"config\":{\"suggested_radio_settings\":{\"entries\":[{\"title\":\"X\",\"frequency\":\"869.6\"",   // truncated
                         "{\"config\":{\"suggested_radio_settings\":{\"entries\":[{\"title\":\"X\",\"frequency\":\"abc\",\"bandwidth\":\"62.5\",\"spreading_factor\":\"8\",\"coding_rate\":\"5\"}]}}}",
                         "{\"config\":{\"suggested_radio_settings\":{\"entries\":[{\"title\":\"X\",\"frequency\":\"869.6\",\"bandwidth\":\"62.5\",\"spreading_factor\":\"99\",\"coding_rate\":\"5\"}]}}}",
                         "{\"config\":{\"suggested_radio_settings\":{\"entries\":[[1,2],3]}}}"};
    for (const char* s : bad) {
        put(f, s);
        std::vector<MeshCorePresetEntry> keep(1);
        CHECK(!loadMeshCorePresetFile(f, keep) && keep.size() == 1, "malformed file accepted: %.40s", s);
    }
    put(f, sample.substr(0, sample.size() / 2));
    CHECK(!loadMeshCorePresetFile(f, o), "half a file accepted");
    CHECK(!loadMeshCorePresetFile(dir + "/no_such_meshcore_file.json", o), "missing file");
    // a malformed file never replaces the built-in table; a valid newer one does
    put(f, "garbage");
    bool fromFile = true;
    CHECK(meshCorePresets(f, &fromFile).size() == b.size() && !fromFile, "malformed -> built-in");
    put(f, sample);
    CHECK(meshCorePresets(f, &fromFile).size() == v.size() && fromFile, "newer valid file used");
    std::remove(f.c_str());
    CHECK(!meshCorePresets("", &fromFile).empty() && !fromFile, "no file -> built-in");

    printf(fails ? "FAILED\n" : "OK (%zu presets)\n", v.size());
    return fails ? 1 : 0;
}
