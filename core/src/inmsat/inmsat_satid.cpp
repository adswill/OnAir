// Satellite identification (inmsat_satid.h).
#include "dect2/inmsat/inmsat_satid.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::inmsat {

const std::vector<SatDef>& satTable() {
    static const std::vector<SatDef> t = {
        // name,                                   lon,   region, 600/1200 P MHz,        10500 P MHz
        {"Alphasat 25E",                            25.0, -1, 1545.115, 1545.130, 1545.315, 1545.3275},    // 10500 bit/s P pair observed 2026-10-30, its system table naming 25.5E
        {"Inmarsat-4 F3 98W",                      -98.0, -1, 1545.020, 1545.205, 1546.005, 1546.0775},
        {"Inmarsat-3 F5 54W (AOR-W)",              -54.0,  0, 1545.025, 1545.040, 1546.055, 1546.070},
        {"Inmarsat-6 F1 83.5E",                     83.5, -1, 1545.160, 1545.225, 1546.0425, 1546.1225},
        {"Inmarsat-4 F1 143.5E",                   143.5, -1, 0, 0, 0, 0},       // no published channel list found
        {"Inmarsat 15.5W (AOR-E)",                 -15.5,  1, 0, 0, 0, 0},       // nominal ocean region slots, no channel list
        {"Inmarsat 64.5E (IOR)",                    64.5,  3, 0, 0, 0, 0},
        {"Inmarsat 178E (POR)",                    178.0,  2, 0, 0, 0, 0},
    };
    return t;
}

void geoLook(double latDeg, double lonDeg, double lonSatDeg, double& azDeg, double& elDeg) {
    const double d2r = 3.14159265358979323846 / 180.0, Re = 6378.137, Rs = 42164.17;
    const double la = latDeg * d2r, lo = lonDeg * d2r, ls = lonSatDeg * d2r;
    const double px = Re * std::cos(la) * std::cos(lo), py = Re * std::cos(la) * std::sin(lo), pz = Re * std::sin(la);
    const double dx = Rs * std::cos(ls) - px, dy = Rs * std::sin(ls) - py, dz = -pz;
    const double e = -std::sin(lo) * dx + std::cos(lo) * dy;
    const double n = -std::sin(la) * std::cos(lo) * dx - std::sin(la) * std::sin(lo) * dy + std::cos(la) * dz;
    const double u = std::cos(la) * std::cos(lo) * dx + std::cos(la) * std::sin(lo) * dy + std::sin(la) * dz;
    const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
    elDeg = std::asin(u / r) / d2r;
    azDeg = std::atan2(e, n) / d2r;
    if (azDeg < 0) azDeg += 360;
}

namespace {
double lonDiff(double a, double b) { double d = std::fmod(a - b + 540.0, 360.0) - 180.0; return std::fabs(d); }

bool inList(const SatDef& s, double mhz, int bitRate, double tolMHz) {
    const bool hi = bitRate == 10500 || (bitRate == 0 && mhz > 1545.95);
    const double lo = hi ? s.p10500Lo : s.p600Lo, up = hi ? s.p10500Hi : s.p600Hi;
    if (lo == 0) return false;
    return mhz >= lo - tolMHz && mhz <= up + tolMHz;
}
bool hasList(const SatDef& s) { return s.p600Lo != 0 || s.p10500Lo != 0; }
std::string beamOf(const SatInput& in) {
    bool any = false, spot = false;
    for (const auto& f : in.sys) if (f.kind == 2 || f.kind == 3) { any = true; spot |= f.spotBeam; }
    return !any ? "" : spot ? "spot beam" : "global beam";
}
std::string lonText(double lon) { char b[24]; std::snprintf(b, sizeof b, "%.1f%c", std::fabs(lon), lon < 0 ? 'W' : 'E'); return b; }
}

SatResult identifySatellite(const SatInput& in) {
    const auto& tab = satTable();
    SatResult res;
    std::vector<SatCandidate> cands;
    // facts that name a longitude outright (content): system table satellite ids, and the ocean region of Inmarsat-C
    for (size_t i = 0; i < tab.size(); i++) {
        const SatDef& s = tab[i];
        SatCandidate c;
        c.index = (int)i;
        // ---- content ----
        SatEvidence ec; ec.kind = "content";
        double best = -1; std::string why;
        for (const auto& f : in.sys) {
            if (f.kind != 2) continue;
            const double d = lonDiff(f.lonDeg, s.lonDeg);
            const double sc = d <= 1.6 ? 1.0 : d <= 3.1 ? 0.6 : 0.0;       // the table's longitude has a 1.5 degree step
            if (sc > best) { best = sc; char b[120]; std::snprintf(b, sizeof b, "P channel system table says satellite %d at %s (this one is at %s)", f.satId, lonText(f.lonDeg).c_str(), lonText(s.lonDeg).c_str()); why = b; }
        }
        for (int r : in.inmcRegions) {
            if (r < 0) continue;
            double sc = (s.oceanRegion == r) ? 1.0 : 0.0;
            if (s.oceanRegion < 0) {      // a satellite with no region code: judged by the region's nominal slot
                for (const auto& o : tab) if (o.oceanRegion == r) sc = lonDiff(o.lonDeg, s.lonDeg) <= 3.1 ? 0.8 : 0.0;
            }
            if (sc > best) { best = sc; static const char* nm[4] = {"AOR-W", "AOR-E", "POR", "IOR"}; why = std::string("Inmarsat-C bulletin board names ocean region ") + (r >= 0 && r < 4 ? nm[r] : "?"); }
        }
        if (best >= 0) { ec.available = true; ec.score = best; ec.text = why; }
        else ec.text = "no system table or bulletin board decoded yet";
        c.evidence.push_back(ec);
        // ---- frequency ----
        SatEvidence ef; ef.kind = "frequency";
        int n = 0, hit = 0;
        for (const auto& k : in.carriers) {
            if (!k.aero || k.freqHz <= 0) continue;
            const double mhz = k.freqHz / 1e6;
            if (mhz < 1545.0 || mhz > 1546.2) continue;
            n++;
            hit += inList(s, mhz, k.bitRate, 0.004);
        }
        if (n > 0 && hasList(s)) {
            ef.available = true; ef.score = (double)hit / n;
            char b[120]; std::snprintf(b, sizeof b, "%d of %d P carriers are in this satellite's channel list", hit, n);
            ef.text = b;
        } else if (n > 0) {
            ef.text = "no published channel list for this satellite";     // it cannot be confirmed or ruled out by frequency
        } else {
            ef.text = in.carriers.empty() ? "no P carriers yet" : "carrier frequencies unknown (the capture centre is not known)";
        }
        c.evidence.push_back(ef);
        // ---- geometry ----
        SatEvidence eg; eg.kind = "geometry";
        if (in.haveLocation) {
            geoLook(in.latDeg, in.lonDeg, s.lonDeg, c.azimuthDeg, c.elevationDeg);
            c.visible = c.elevationDeg > 2.0;
            eg.available = true;
            eg.score = c.elevationDeg <= 2.0 ? 0.0 : std::min(1.0, 0.2 + (c.elevationDeg - 2.0) / 30.0);
            char b[120]; std::snprintf(b, sizeof b, "azimuth %.0f, elevation %.0f degrees from your location%s", c.azimuthDeg, c.elevationDeg, c.visible ? "" : " (below the horizon)");
            eg.text = b;
        } else eg.text = "your location is not set";
        c.evidence.push_back(eg);
        cands.push_back(c);
    }
    // combine: weighted mean of the evidence that exists; the number of agreeing sources limits the confidence; a satellite below the
    // horizon is not receivable, whatever else is said (unless the content names it: then both are shown and the result is ambiguous)
    // geometry alone never names a satellite (a dozen slots are above the horizon): it is a plausibility factor on what content and frequency say
    const double w[2] = {0.5, 0.3};
    for (auto& c : cands) {
        double sw = 0, sum = 0;
        int base = 0, agree = 0;
        for (size_t k = 0; k < 2; k++) {
            const auto& e = c.evidence[k];
            if (!e.available) continue;
            sw += w[k]; sum += w[k] * e.score; base++;
            agree += e.score >= 0.6;
        }
        if (base == 0) { c.confidence = 0; continue; }
        const bool geo = c.evidence[2].available;
        double conf = sum / sw;
        const double cap = (base >= 2 ? 0.85 : 0.6) + (geo && c.visible ? 0.15 : 0.0);
        conf = std::min(conf, cap) * (0.5 + 0.5 * (double)agree / base);
        if (geo && !c.visible) conf *= 0.25;
        else if (geo && c.elevationDeg < 10) conf *= 0.8;
        c.confidence = conf;
    }
    std::sort(cands.begin(), cands.end(), [](const SatCandidate& a, const SatCandidate& b) { return a.confidence > b.confidence; });
    if (cands.size() > 6) cands.resize(6);
    res.ranked = cands;
    if (!in.manual.empty()) {
        for (auto& c : res.ranked) if (tab[(size_t)c.index].name == in.manual) { res.valid = true; res.manual = true; res.name = in.manual; res.lonDeg = tab[(size_t)c.index].lonDeg; res.confidence = 1.0; res.summary = in.manual + " (chosen by you)"; break; }
        if (!res.valid) for (size_t i = 0; i < tab.size(); i++) if (in.manual == tab[i].name) { res.valid = true; res.manual = true; res.name = in.manual; res.lonDeg = tab[i].lonDeg; res.confidence = 1.0; res.summary = in.manual + " (chosen by you)"; }
        if (res.valid) {
            res.beam = beamOf(in);
            return res;
        }
    }
    if (!res.ranked.empty() && res.ranked[0].confidence >= 0.2) {
        const SatDef& s = tab[(size_t)res.ranked[0].index];
        res.valid = true; res.name = s.name; res.lonDeg = s.lonDeg; res.confidence = res.ranked[0].confidence;
        res.ambiguous = res.ranked.size() > 1 && res.ranked[1].confidence > res.confidence - 0.15 && res.ranked[1].confidence >= 0.2;
        res.beam = beamOf(in);
        char b[200]; std::snprintf(b, sizeof b, "%s, %.0f%% sure%s%s", s.name, res.confidence * 100, res.ambiguous ? ", close to " : "", res.ambiguous ? tab[(size_t)res.ranked[1].index].name : "");
        res.summary = b;
    } else {
        res.summary = "satellite not identified yet";
        // a longitude from the system table with no matching slot is still worth showing
        for (const auto& f : in.sys) if (f.kind == 2) { res.summary = "a satellite at " + lonText(f.lonDeg) + " (not in the table)"; break; }
    }
    return res;
}

std::string satNameForCarrier(const SatResult& r, double freqHz, int bitRate) {
    if (!r.valid) return "";
    if (r.manual) return r.name;
    const auto& tab = satTable();
    if (freqHz > 0 && !r.ranked.empty()) {
        for (const auto& c : r.ranked) if (c.confidence >= 0.2 && inList(tab[(size_t)c.index], freqHz / 1e6, bitRate, 0.004) && c.confidence >= r.confidence - 0.15) return tab[(size_t)c.index].name;
    }
    return r.confidence >= 0.5 ? r.name : "";
}

} // namespace dect2::inmsat
