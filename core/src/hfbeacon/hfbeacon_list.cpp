// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::hfb {

const char* const kNcdxfCall[kNcdxfBeacons] = {"4U1UN", "VE8AT", "W6WX", "KH6RS", "ZL6B", "VK6RBP", "JA2IGY", "RR9O", "VR2B",
                                               "4S7B", "ZS6DN", "5Z4B", "4X6TU", "OH2B", "CS3B", "LU4AA", "OA4B", "YV5B"};
const double kNcdxfBandKhz[kNcdxfBands] = {14100, 18110, 21150, 24930, 28200};
// grid squares of the IBP table; the beacon is placed at the centre of its square
static const char* const kNcdxfGrid[kNcdxfBeacons] = {"FN30as", "CP38", "CM87", "BL10", "RE78", "OF87", "PM84", "NO14", "OL72",
                                                      "MJ96", "KG44", "KI88", "KM72", "KP20", "IM12", "GF05", "FH17", "FJ69"};
static const char* const kNcdxfPlace[kNcdxfBeacons] = {"United Nations", "Inuvik, Canada", "California, USA", "Hawaii, USA", "New Zealand", "Western Australia",
                                                       "Japan", "Novosibirsk, Russia", "Hong Kong", "Sri Lanka", "South Africa", "Kenya", "Israel",
                                                       "Finland", "Madeira", "Argentina", "Peru", "Venezuela"};

int ncdxfSlotAt(double utcSec) {
    double m = std::fmod(utcSec, kNcdxfCycleSec);
    if (m < 0) m += kNcdxfCycleSec;
    return std::min(kNcdxfBeacons - 1, (int)(m / kNcdxfSlotSec));
}
// beacon k is on band b in slot (k + b) mod 18: it steps to the next higher band when the next beacon starts on the one below
int ncdxfBeaconAt(int band, double utcSec) { return ((ncdxfSlotAt(utcSec) - band) % kNcdxfBeacons + kNcdxfBeacons) % kNcdxfBeacons; }
double ncdxfNextStart(int idx, int band, double utcSec) {
    const int slot = (idx + band) % kNcdxfBeacons;
    const double cyc = std::floor(utcSec / kNcdxfCycleSec) * kNcdxfCycleSec;
    double t = cyc + slot * kNcdxfSlotSec;
    if (t < utcSec) t += kNcdxfCycleSec;
    return t;
}

double searchOf(const Beacon& b) {
    if (b.searchHz > 0) return b.searchHz;
    return b.kind == Letter ? 50 : b.kind == Dashes ? 1000 : 150;
}
const char* kindName(int k) { return k == Ncdxf ? "NCDXF/IARU" : k == Letter ? "single letter" : k == Dashes ? "long dashes" : "carrier"; }

static const char* const kBandNames[7] = {"10 m", "12 m", "15 m", "17 m", "20 m", "other below 10 MHz", "other above 10 MHz"};
int bandIndexOf(double khz) {
    if (khz >= 28000 && khz <= 29700) return 0;
    if (khz >= 24890 && khz <= 24990) return 1;
    if (khz >= 21000 && khz <= 21450) return 2;
    if (khz >= 18068 && khz <= 18168) return 3;
    if (khz >= 14000 && khz <= 14350) return 4;
    return khz < 10000 ? 5 : 6;
}
std::string bandName(double khz) { return kBandNames[bandIndexOf(khz)]; }

std::vector<Beacon> defaultBeacons() {
    std::vector<Beacon> v;
    const char* ibp = "NCDXF/IARU International Beacon Project, ncdxf.org/beacon (schedule); grid square from the IBP table, position = square centre";
    for (int k = 0; k < kNcdxfBeacons; k++)
        for (int b = 0; b < kNcdxfBands; b++) {
            Beacon x;
            x.name = std::string(kNcdxfCall[k]) + " " + kNcdxfPlace[k];
            x.khz = kNcdxfBandKhz[b];
            x.kind = Ncdxf; x.ncdxfIdx = k; x.ncdxfBand = b;
            fromMaidenhead(kNcdxfGrid[k], x.lat, x.lon);
            x.mode = "CW + 4 dashes (100 W, 10 W, 1 W, 0.1 W)";
            char s[96]; snprintf(s, sizeof s, "10 s slot %d of 18 in each 3 minute cycle (UTC), 00:00 start", (k + b) % kNcdxfBeacons);
            x.schedule = s;
            x.source = ibp;
            v.push_back(x);
        }
    // Long Dashes (sigidwiki): descending order, 3.0 s per frequency, 29996 kHz twice in a row; origin central Australia, speculated
    static const double ld[8] = {29996, 27928, 21899, 16174, 12494, 11999, 8999, 6496};
    for (int i = 0; i < 8; i++) {
        Beacon x;
        char n[48]; snprintf(n, sizeof n, "Long Dashes %.0f kHz", ld[i]);
        x.name = n; x.khz = ld[i]; x.kind = Dashes; x.dashIdx = i == 0 ? 0 : i + 1;
        x.lat = -25; x.lon = 134; x.uncertain = true;
        x.mode = "USB OOK, 2.9 s dash with a 0.2 s multitone start";
        x.schedule = "27 s cycle, descending from 29996 kHz; on air only some hours a day";
        x.source = "sigidwiki.com/wiki/Long_Dashes; location speculated (central Australia, KiwiSDR TDoA), frequency 'around' the value, dial or carrier not stated";
        v.push_back(x);
    }
    // Russian naval single-letter beacons (priyom): eight markers 100 Hz apart, D P S C A F K M ascending from the lowest of each cluster
    static const char mk[8] = {'D', 'P', 'S', 'C', 'A', 'F', 'K', 'M'};
    static const char* const city[8] = {"Sevastopol", "Kaliningrad", "Severomorsk", "Moscow", "Astrakhan", "Vladivostok", "Petropavlovsk-Kamchatsky", "Magadan"};
    static const double clat[8] = {44.62, 54.71, 69.07, 55.75, 46.35, 43.12, 53.04, 59.56}, clon[8] = {33.52, 20.51, 33.42, 37.62, 48.04, 131.89, 158.65, 150.81};
    static const double cl[9] = {3594.7, 4557.7, 5153.7, 7508.7, 8494.7, 10871.7, 13527.7, 16331.7, 20047.7};
    for (int c = 0; c < 9; c++)
        for (int i = 0; i < 8; i++) {
            Beacon x;
            x.khz = cl[c] + 0.1 * i;
            // the page lists 7508.7 - 7509.1 for the 7.5 MHz cluster (five) and 7039.2 - 7039.4 apart: taken as the markers F, K and M, literally
            const bool odd = c == 3 && i >= 5;
            if (odd) x.khz = 7039.2 + 0.1 * (i - 5);
            char n[96]; snprintf(n, sizeof n, "%c %s %.1f", mk[i], city[i], x.khz);
            x.name = n; x.kind = Letter; x.letter = mk[i];
            x.lat = clat[i]; x.lon = clon[i];
            x.mode = std::string("single letter '") + mk[i] + "' in Morse";
            x.schedule = "not all channels are active at a given time";
            x.source = "priyom.org/military-stations/russia/single-letter-beacons; position = the base city the page names";
            if (odd) { x.uncertain = true; x.source += "; the page lists 7039.2 - 7039.4 after the five 7508.7 - 7509.1 values (possibly 7509.2 - 7509.4)"; }
            v.push_back(x);
        }
    return v;
}

// ---- JSON (the subset the list uses: one array of flat objects of strings, numbers and booleans)

static void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    o += '"';
}

std::string toJson(const std::vector<Beacon>& b) {
    std::string o = "{\n \"version\": 1,\n \"beacons\": [\n";
    char t[160];
    for (size_t i = 0; i < b.size(); i++) {
        const Beacon& x = b[i];
        o += "  {\"name\": "; esc(o, x.name);
        snprintf(t, sizeof t, ", \"khz\": %.3f, \"pattern\": \"%s\"", x.khz, x.kind == Ncdxf ? "ncdxf" : x.kind == Letter ? "letter" : x.kind == Dashes ? "dashes" : "carrier");
        o += t;
        if (x.kind == Letter) { o += ", \"letter\": \""; o += x.letter ? x.letter : '?'; o += "\""; }
        if (x.kind == Ncdxf) { snprintf(t, sizeof t, ", \"ncdxf_index\": %d, \"ncdxf_band\": %d", x.ncdxfIdx, x.ncdxfBand); o += t; }
        if (x.kind == Dashes) { snprintf(t, sizeof t, ", \"dash_index\": %d", x.dashIdx); o += t; }
        snprintf(t, sizeof t, ", \"lat\": %.4f, \"lon\": %.4f", x.lat, x.lon); o += t;
        o += ", \"mode\": "; esc(o, x.mode);
        o += ", \"schedule\": "; esc(o, x.schedule);
        o += ", \"source\": "; esc(o, x.source);
        o += x.uncertain ? ", \"uncertain\": true" : ", \"uncertain\": false";
        if (x.searchHz > 0) { snprintf(t, sizeof t, ", \"search_hz\": %.0f", x.searchHz); o += t; }
        o += i + 1 < b.size() ? "},\n" : "}\n";
    }
    o += " ]\n}\n";
    return o;
}

namespace {
struct P {
    const std::string& s; size_t i = 0; std::string err;
    explicit P(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool fail(const char* m) { if (err.empty()) { char b[96]; snprintf(b, sizeof b, "%s at byte %zu", m, i); err = b; } return false; }
    bool lit(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    bool str(std::string& o) {
        ws();
        if (i >= s.size() || s[i] != '"') return fail("string expected");
        i++; o.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                const char c = s[i];
                if (c == 'n') o += '\n'; else if (c == 't') o += ' ';
                else if (c == 'u' && i + 4 < s.size()) { o += '?'; i += 4; }
                else o += c;
            } else o += s[i];
            i++;
        }
        if (i >= s.size()) return fail("unterminated string");
        i++;
        return true;
    }
    // a value: string, number or bool, kept as text with its type
    bool value(std::string& o, int& type) {
        ws();
        if (i >= s.size()) return fail("value expected");
        if (s[i] == '"') { type = 0; return str(o); }
        size_t j = i;
        while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '-' || s[j] == '+')) j++;
        if (j == i) return fail("value expected");
        o = s.substr(i, j - i); i = j;
        type = (o == "true" || o == "false") ? 2 : 1;
        return true;
    }
    bool skip() {   // skip any value of an unknown key
        ws();
        if (i < s.size() && (s[i] == '[' || s[i] == '{')) {
            int d = 0; bool q = false;
            for (; i < s.size(); i++) {
                const char c = s[i];
                if (q) { if (c == '\\') i++; else if (c == '"') q = false; }
                else if (c == '"') q = true;
                else if (c == '[' || c == '{') d++;
                else if (c == ']' || c == '}') { if (--d == 0) { i++; return true; } }
            }
            return fail("unterminated value");
        }
        std::string o; int t; return value(o, t);
    }
};
}

bool fromJson(const std::string& text, std::vector<Beacon>& out, std::string* err) {
    P p(text);
    std::vector<Beacon> v;
    auto bad = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!p.lit('{')) return bad("not a JSON object");
    bool found = false;
    while (!p.lit('}')) {
        std::string key;
        if (!p.str(key) || !p.lit(':')) return bad(p.err.empty() ? "key expected" : p.err);
        if (key != "beacons") { if (!p.skip()) return bad(p.err); p.lit(','); continue; }
        found = true;
        if (!p.lit('[')) return bad("beacons must be an array");
        while (!p.lit(']')) {
            if (!p.lit('{')) return bad("a beacon must be an object");
            Beacon b;
            std::string pattern;
            bool haveFreq = false;
            while (!p.lit('}')) {
                std::string k, val; int ty = 0;
                if (!p.str(k) || !p.lit(':') || !p.value(val, ty)) return bad(p.err);
                const double num = std::atof(val.c_str());
                if (k == "name") b.name = val;
                else if (k == "khz") { b.khz = num; haveFreq = true; }
                else if (k == "mhz") { b.khz = num * 1000; haveFreq = true; }
                else if (k == "pattern") pattern = val;
                else if (k == "letter") b.letter = val.empty() ? 0 : (char)std::toupper((unsigned char)val[0]);
                else if (k == "ncdxf_index") b.ncdxfIdx = (int)num;
                else if (k == "ncdxf_band") b.ncdxfBand = (int)num;
                else if (k == "dash_index") b.dashIdx = (int)num;
                else if (k == "lat") b.lat = num;
                else if (k == "lon") b.lon = num;
                else if (k == "mode") b.mode = val;
                else if (k == "schedule") b.schedule = val;
                else if (k == "source") b.source = val;
                else if (k == "uncertain") b.uncertain = val == "true";
                else if (k == "search_hz") b.searchHz = num;
                p.lit(',');
            }
            if (!haveFreq || !(b.khz > 0 && b.khz < 1e6)) return bad("beacon '" + b.name + "' has no frequency");
            if (b.name.empty()) { char n[32]; snprintf(n, sizeof n, "%.1f kHz", b.khz); b.name = n; }
            if (pattern == "ncdxf") b.kind = Ncdxf; else if (pattern == "letter") b.kind = Letter; else if (pattern == "dashes") b.kind = Dashes; else b.kind = Carrier;
            if (b.kind == Ncdxf && (b.ncdxfIdx < 0 || b.ncdxfIdx >= kNcdxfBeacons || b.ncdxfBand < 0 || b.ncdxfBand >= kNcdxfBands)) return bad("beacon '" + b.name + "': ncdxf_index 0-17 and ncdxf_band 0-4 needed");
            if (b.kind == Letter && !std::strchr("DPSCAFKM", b.letter ? b.letter : '?')) return bad("beacon '" + b.name + "': letter must be one of D P S C A F K M");
            if (b.kind == Dashes && b.dashIdx < 0) b.dashIdx = 0;
            if (b.lat < -90 || b.lat > 90 || b.lon < -180 || b.lon > 360) return bad("beacon '" + b.name + "': position out of range");
            v.push_back(std::move(b));
            p.lit(',');
        }
        p.lit(',');
    }
    if (!found) return bad("no \"beacons\" array");
    out = std::move(v);
    return true;
}

std::vector<Centre> planCentres(const std::vector<Beacon>& b, double rateHz, int onlyKind) {
    std::vector<int> idx;
    for (int i = 0; i < (int)b.size(); i++) if (onlyKind < 0 || b[(size_t)i].kind == onlyKind) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](int x, int y) { return b[(size_t)x].khz < b[(size_t)y].khz; });
    const double span = 0.8 * rateHz - 8000;   // the usable span, less a margin each side for the Long Dashes' search
    std::vector<Centre> out;
    size_t i = 0;
    while (i < idx.size()) {
        const double lo = b[(size_t)idx[i]].khz * 1e3;
        Centre c;
        size_t j = i;
        while (j < idx.size() && b[(size_t)idx[j]].khz * 1e3 - lo <= span) { c.beacons.push_back(idx[j]); j++; }
        if (c.beacons.empty()) { c.beacons.push_back(idx[i]); j = i + 1; }
        const double hi = b[(size_t)c.beacons.back()].khz * 1e3;
        c.centerHz = std::max(100e3, (lo + hi) / 2);
        // not near a beacon: the radio's centre spike (and the detector's own) hides a signal there; search nearby centres that still cover the stop
        const double half = 0.4 * rateHz - 3000;
        auto fits = [&](double cc, bool spike) {
            for (int bi : c.beacons) {
                const double d = std::fabs(b[(size_t)bi].khz * 1e3 - cc);
                if (d > half || (spike && d < 20e3)) return false;
            }
            return true;
        };
        if (!fits(c.centerHz, true))
            for (double sh = 25e3; sh < half; sh += 5e3) {
                if (fits(c.centerHz + sh, true)) { c.centerHz += sh; break; }
                if (fits(c.centerHz - sh, true) && c.centerHz - sh >= 100e3) { c.centerHz -= sh; break; }
            }
        out.push_back(std::move(c));
        i = j;
    }
    return out;
}

} // namespace dect2::hfb
