// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "session_bundle.h"
#include <cctype>
#include <cstdio>
#include <ctime>

namespace sessionbundle {
namespace bk = dect2::backup;

static std::string utc(int64_t t, bool compact) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[40];
    if (compact) strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm); else strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return b;
}

std::string fileName(int64_t epochSec) { return "onair-session-" + utc(epochSec, true) + ".zip"; }

std::string recordingsCsv(const std::vector<Recording>& r) {
    resexp::Table t;
    t.cols = {"name", "bytes", "modified_utc"};
    for (const Recording& x : r) t.add({x.name, std::to_string(x.bytes), x.mtime > 0 ? utc(x.mtime, false) : ""});
    return resexp::toCsv(t);
}

static bool lowerHas(const std::string& key, const char* part) {
    std::string k;
    for (char c : key) k += (char)std::tolower((unsigned char)c);
    return k.find(part) != std::string::npos;
}

std::string settingsText(const std::vector<bk::PrefItem>& prefs) {
    std::vector<bk::PrefItem> plain, secret, keep;
    bk::splitSecretPrefs(prefs, plain, secret);
    for (const bk::PrefItem& p : plain) {
        if (p.type == 'C') continue;   // scan channels are not part of a session
        const std::string& k = p.key;
        if (k == "adsbLat" || k == "adsbLon" || k == "setupLoc" || k == "mcLat" || k == "mcLon" || lowerHas(k, "latitude") || lowerHas(k, "longitude")) continue;
        if (lowerHas(k, "dir") && p.type == 'S') continue;     // folders carry the user's account name
        if (lowerHas(k, "path") && p.type == 'S') continue;
        keep.push_back(p);
    }
    return bk::serializePrefs(keep);
}

static std::string radioText(const Input& in) {
    char b[160];
    std::string o = "OnAir " + in.version + "\n";
    o += "mode: " + in.mode + "\n";
    snprintf(b, sizeof b, "frequency_mhz: %s\n", resexp::num(in.freqHz / 1e6, 6).c_str()); o += b;
    snprintf(b, sizeof b, "sample_rate_msps: %s\n", resexp::num(in.sampleRateHz / 1e6, 6).c_str()); o += b;
    o += "device: " + in.device + "\n";
    return o;
}

static std::string locationText(const Input& in) {
    std::string o;
    if (!in.hasLocation) return "(not set)\n";
    if (in.lat != 0 || in.lon != 0) o += "lat: " + resexp::num(in.lat, 5) + "\nlon: " + resexp::num(in.lon, 5) + "\n";
    if (!in.grid.empty()) o += "grid: " + in.grid + "\n";
    return o.empty() ? "(not set)\n" : o;
}

std::string summaryText(const Input& in) {
    std::string o = "OnAir session export\n";
    o += "version: " + in.version + "\n";
    if (in.t0 > 0) o += "from: " + utc((int64_t)in.t0, false) + "\n";
    if (in.t1 > 0) o += "to: " + utc((int64_t)in.t1, false) + "\n";
    o += "mode: " + in.mode + "\n";
    o += "frequency_mhz: " + resexp::num(in.freqHz / 1e6, 6) + "\n";
    o += std::to_string(in.log.rows.size()) + " rows in log.csv\n";
    if (in.withRecordings) o += std::to_string(in.recordings.size()) + " recordings listed in recordings.csv (the files themselves are not included)\n";
    o += "\nFiles: log.csv";
    if (resexp::toKml(in.log).find("<Placemark>") != std::string::npos) o += ", log.kml, log.gpx";
    if (in.withRecordings) o += ", recordings.csv";
    if (in.withSettings) o += ", settings.txt (credentials, folders and position left out)";
    o += ", radio.txt";
    if (in.withLocation) o += ", location.txt";
    o += "\n";
    return o;
}

std::vector<bk::Entry> entries(const Input& in) {
    std::vector<bk::Entry> e;
    e.push_back({"summary.txt", summaryText(in)});
    e.push_back({"log.csv", resexp::toCsv(in.log)});
    const std::string kml = resexp::toKml(in.log);
    if (kml.find("<Placemark>") != std::string::npos) {
        e.push_back({"log.kml", kml});
        e.push_back({"log.gpx", resexp::toGpx(in.log)});
    }
    if (in.withRecordings) e.push_back({"recordings.csv", recordingsCsv(in.recordings)});
    if (in.withSettings) e.push_back({"settings.txt", settingsText(in.prefs)});
    e.push_back({"radio.txt", radioText(in)});
    if (in.withLocation) e.push_back({"location.txt", locationText(in)});
    return e;
}

std::string build(const Input& in) { return bk::zipPack(entries(in)); }

}  // namespace sessionbundle
