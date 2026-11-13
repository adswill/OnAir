// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The result lists the screens keep themselves (the receivers keep theirs: Engine::setResultsDir): the DAB and FM band scans, the
// TV channel scan (DVB / ATSC / ISDB-T / DTMB), the experimental scanner's hit log, the VOR fix radials and the last weather fax picture. Saved beside the engine's files in <data folder>/results
// (app-<name>.bin, results_io.h format: a file of another build is discarded) every 30 s and at exit, loaded at start; the interface
// thread only. Each list is emptied by its own Clear.
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/results/results_io.h"
#include "dect2/engine/scanner.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

std::string scannerResultsSave();
bool scannerResultsLoad(const std::string& blob);
std::string vorResultsSave();
bool vorResultsLoad(const std::string& blob);
std::string marineFaxSave();
bool marineFaxLoad(const std::string& blob);

namespace {

struct Slot {
    const char* name;
    std::function<std::string(App&)> save;
    std::function<bool(App&, const std::string&)> load;
};

std::vector<Slot> slots() {
    return {
        {"dab-scan", [](App& a) { return dect2::results::pack(a.dabScan.results); },
                     [](App& a, const std::string& b) { return !a.dabScan.running && dect2::results::unpack(b, a.dabScan.results); }},
        {"fm-scan", [](App& a) { return dect2::results::pack(a.fmScan.results); },
                    [](App& a, const std::string& b) { return !a.fmScan.running && dect2::results::unpack(b, a.fmScan.results); }},
        {"tv-scan", [](App& a) { return dect2::results::pack(a.scanner.results()); },
                    [](App& a, const std::string& b) {
                        if (a.scanner.progress().running) return false;
                        std::vector<dect2::ScanResult> r;
                        if (!dect2::results::unpack(b, r)) return false;
                        a.scanner.setResults(std::move(r));
                        return true;
                    }},
        {"scanner", [](App&) { return scannerResultsSave(); }, [](App&, const std::string& b) { return scannerResultsLoad(b); }},
        {"vor-fix", [](App&) { return vorResultsSave(); }, [](App&, const std::string& b) { return vorResultsLoad(b); }},
        {"marine-fax", [](App&) { return marineFaxSave(); }, [](App&, const std::string& b) { return marineFaxLoad(b); }},
    };
}

std::map<std::string, uint64_t> gHash;   // what each file holds now
double gLastSave = 0;

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string resultsDir() { return plat::dataDir() + "/results"; }

} // namespace

void appResultsLoad(App& a) {
    for (const Slot& s : slots()) {
        const std::filesystem::path p = u8path(resultsDir()) / (std::string("app-") + s.name + ".bin");
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string blob = ss.str();
        if (s.load(a, blob)) { gHash[s.name] = dect2::results::detail::fnv1a(blob); continue; }
        f.close();
        std::error_code ec;
        std::filesystem::remove(p, ec);
        a.engine.log(std::string("results: the saved ") + s.name + " list is from another version and was discarded");
    }
}

void appResultsSave(App& a, bool now) {
    const double t = ImGui::GetTime();
    if (!now && t - gLastSave < 30) return;
    gLastSave = t;
    std::error_code ec;
    std::filesystem::create_directories(u8path(resultsDir()), ec);
    for (const Slot& s : slots()) {
        const std::string blob = s.save(a);
        const uint64_t h = dect2::results::detail::fnv1a(blob);
        auto it = gHash.find(s.name);
        if (it != gHash.end() && it->second == h) continue;
        const std::filesystem::path p = u8path(resultsDir()) / (std::string("app-") + s.name + ".bin");
        std::filesystem::path tmp = p;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) continue;
            f.write(blob.data(), (std::streamsize)blob.size());
            if (!f) continue;
        }
        std::filesystem::rename(tmp, p, ec);
        if (ec) { std::filesystem::remove(tmp, ec); continue; }
        gHash[s.name] = h;
    }
}

bool resultsClearButton(const char* id) {
    char b[64];
    snprintf(b, sizeof b, "Clear##res%s", id);
    const bool pressed = ImGui::SmallButton(b);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty the decoded lists of this mode. They are kept until then: through changes of mode,\nretunes and restarts of the receiver, and from one run of OnAir to the next.");
    return pressed;
}
