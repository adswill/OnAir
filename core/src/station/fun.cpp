// Surprise me, rare catches, dossier and plain-English messages: see fun.h.
#include "dect2/station/fun.h"
#include "dect2/adsb/adsb_reg.h"
#include "dect2/marine/marine_mmsi.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace dect2::station {

using marine::MmsiInfo; using marine::MmsiKind; using marine::mmsiInfo; using marine::mmsiKindName;

namespace {

// a "key=value" line of Record::extra
std::string extraField(const std::string& extra, const std::string& key) {
    const std::string k = key + "=";
    size_t p = 0;
    while (p < extra.size()) {
        size_t e = extra.find('\n', p);
        if (e == std::string::npos) e = extra.size();
        if (extra.compare(p, k.size(), k) == 0) return extra.substr(p + k.size(), e - p - k.size());
        p = e + 1;
    }
    return {};
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

const char* squawkMeaning(int sq) {
    switch (sq) {
    case 7500: return "hijacking";
    case 7600: return "radio failure";
    case 7700: return "general emergency";
    default: return nullptr;
    }
}

// ITU-R M.1371 navigational status
const char* navStatusText(int s) {
    switch (s) {
    case 0: return "under way using its engine";
    case 1: return "at anchor";
    case 2: return "not under command";
    case 3: return "restricted in its ability to manoeuvre";
    case 4: return "constrained by its draught";
    case 5: return "moored";
    case 6: return "aground";
    case 7: return "engaged in fishing";
    case 8: return "under way sailing";
    case 14: return "sending an AIS-SART, MOB or EPIRB distress signal";
    default: return nullptr;
    }
}

bool isShipDistress(MmsiKind k) { return k == MmsiKind::AisSart || k == MmsiKind::Mob || k == MmsiKind::EpirbAis; }

int squawkOf(const msgdb::Record& r) {
    const std::string s = extraField(r.extra, "squawk");
    return s.empty() ? -1 : std::atoi(s.c_str());
}

}  // namespace

int surprise(const std::vector<SurpriseMode>& modes, uint32_t seed) {
    uint64_t total = 0;
    for (const SurpriseMode& m : modes) if (m.ready) total += m.heard ? 1 : 3;
    if (!total) return -1;
    // a few rounds of an LCG, so that neighbouring seeds (the clock) give unrelated picks
    uint32_t x = seed;
    for (int i = 0; i < 4; i++) x = x * 1664525u + 1013904223u;
    uint64_t pick = (uint64_t)(x >> 8) % total;
    for (size_t i = 0; i < modes.size(); i++) {
        if (!modes[i].ready) continue;
        const uint64_t w = modes[i].heard ? 1 : 3;
        if (pick < w) return (int)i;
        pick -= w;
    }
    return -1;
}

void RareWatcher::setKnownModes(const std::vector<std::string>& modes) {
    for (const std::string& m : modes) known_.insert(lower(m));
}

bool RareWatcher::check(const msgdb::Record& r, RareCatch& out) {
    out = RareCatch();
    out.ident = r.ident; out.mode = r.mode; out.time = r.time;
    const int sq = squawkOf(r);
    const char* meaning = squawkMeaning(sq);
    if (meaning && said_.insert("sq|" + lower(r.ident)).second) {
        out.kind = RareKind::EmergencySquawk;
        out.text = "Aircraft " + r.ident + " is squawking " + std::to_string(sq) + " (" + meaning + ")";
        known_.insert(lower(r.mode));
        return true;
    }
    if (r.ident.size() == 9 && std::all_of(r.ident.begin(), r.ident.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const MmsiInfo i = mmsiInfo(r.ident);
        if (isShipDistress(i.kind) && said_.insert("sart|" + r.ident).second) {
            out.kind = RareKind::SartBeacon;
            out.text = std::string(mmsiKindName(i.kind)) + " beacon heard: " + r.ident;
            known_.insert(lower(r.mode));
            return true;
        }
    }
    if (!r.mode.empty() && known_.insert(lower(r.mode)).second) {
        out.kind = RareKind::FirstOfMode;
        out.text = "First " + r.mode + " message ever heard here";
        return true;
    }
    return false;
}

Dossier dossier(const msgdb::Db& db, const std::string& ident) {
    Dossier d;
    d.ident = ident;
    d.kind = identKind(ident);
    if (d.kind == QueryKind::Icao) {
        const uint32_t icao = (uint32_t)std::strtoul(ident.c_str(), nullptr, 16);
        const AdsbCountry c = adsbCountry(icao);
        if (c.iso && *c.iso) d.country = c.name;
        d.registration = adsbAlgorithmicRegistration(icao);
    } else if (d.kind == QueryKind::Mmsi) {
        const MmsiInfo i = mmsiInfo(ident);
        d.country = i.country;
        if (i.kind != MmsiKind::Unknown) d.what = mmsiKindName(i.kind);
    }
    msgdb::Filter f; f.ident = ident; f.limit = 20000;
    std::map<std::string, int> modes;
    std::vector<double> freqs;
    double posT = -1;
    for (msgdb::Record& r : db.query(f)) {
        if (lower(r.ident) != lower(ident)) continue;   // the filter is a substring match
        if (!d.messages) d.first = d.last = r.time;
        d.messages++;
        d.first = std::min(d.first, r.time);
        d.last = std::max(d.last, r.time);
        modes[r.mode]++;
        if (r.freqHz > 0 && std::find(freqs.begin(), freqs.end(), r.freqHz) == freqs.end()) freqs.push_back(r.freqHz);
        if (r.hasPos && r.time > posT) { posT = r.time; d.hasPos = true; d.lat = r.lat; d.lon = r.lon; }
        if (d.recent.size() < 20) d.recent.push_back(std::move(r));
    }
    for (const auto& kv : modes) d.modes.push_back(kv.first);
    std::sort(freqs.begin(), freqs.end());
    d.freqsHz = std::move(freqs);
    return d;
}

std::string plainEnglish(const msgdb::Record& r) {
    const int sq = squawkOf(r);
    if (const char* m = squawkMeaning(sq)) return "This aircraft is reporting a " + std::string(m) + " (squawk " + std::to_string(sq) + ").";
    const std::string nav = extraField(r.extra, "nav_status");
    if (!nav.empty() && r.ident.size() == 9) {
        if (const char* m = navStatusText(std::atoi(nav.c_str()))) return "This ship says it is " + std::string(m) + ".";
    }
    const QueryKind k = identKind(r.ident);
    if (k == QueryKind::Mmsi) {
        const MmsiInfo i = mmsiInfo(r.ident);
        if (i.kind != MmsiKind::Unknown) return std::string(mmsiKindName(i.kind)) + (i.country.empty() ? std::string(".") : " registered in " + i.country + ".");
    }
    if (k == QueryKind::Icao) {
        const AdsbCountry c = adsbCountry((uint32_t)std::strtoul(r.ident.c_str(), nullptr, 16));
        if (c.iso && *c.iso) return std::string("An aircraft registered in ") + c.name + ".";
    }
    return {};
}

} // namespace dect2::station
