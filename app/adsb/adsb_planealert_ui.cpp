// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// plane-alert-db in the ADS-B table: the category and tags of an aircraft from the imported file (<data folder>/plane-alert-db.csv).
#include "adsb_internal.h"
#include "dect2/watchlist/plane_alert.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace adsbui {

namespace {
std::map<uint32_t, dect2::watchlist::PlaneAlertEntry> gIdx;
double gLoaded = -1e9;
std::filesystem::file_time_type gStamp{};
}

const dect2::watchlist::PlaneAlertEntry* planeAlertFor(uint32_t icao) {
    const double t = ImGui::GetTime();
    if (t - gLoaded > 10) {   // read again when the file was imported anew
        gLoaded = t;
        const std::filesystem::path p(reinterpret_cast<const char8_t*>((plat::dataDir() + "/plane-alert-db.csv").c_str()));
        std::error_code ec;
        const auto st = std::filesystem::last_write_time(p, ec);
        if (!ec && st != gStamp) {
            gStamp = st;
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            std::vector<dect2::watchlist::PlaneAlertEntry> v;
            std::string err;
            gIdx = dect2::watchlist::parsePlaneAlert(ss.str(), v, err) ? dect2::watchlist::planeAlertIndex(v) : decltype(gIdx)();
        }
    }
    auto it = gIdx.find(icao & 0xFFFFFF);
    return it == gIdx.end() ? nullptr : &it->second;
}

}  // namespace adsbui
