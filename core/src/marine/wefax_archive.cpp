// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/wefax_archive.h"
#include "dect2/common/diag.h"
#include "dect2/marine/marine_wefax_stations.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {
// days since 1970-01-01 for a civil date (UTC), and back; avoids timegm/gmtime portability differences
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
// value text after "key": (string unescaped, number/bool raw)
bool jsonGet(const std::string& j, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p])) p++;
    if (p >= j.size()) return false;
    out.clear();
    if (j[p] == '"') {
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) {
                p++;
                if (j[p] == 'n') out += '\n';
                else if (j[p] == 'u' && p + 4 < j.size()) { out += (char)std::strtol(j.substr(p + 1, 4).c_str(), nullptr, 16); p += 4; }
                else out += j[p];
            } else out += j[p];
        }
        return true;
    }
    while (p < j.size() && j[p] != ',' && j[p] != '}' && !std::isspace((unsigned char)j[p])) out += j[p++];
    return !out.empty();
}
std::string utcIso(long long t) {
    long long y; unsigned m, d;
    const long long days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    const long long sod = t - days * 86400;
    civilFromDays(days, y, m, d);
    char b[40];
    snprintf(b, sizeof b, "%04lld-%02u-%02uT%02d:%02d:%02dZ", y, m, d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
    return b;
}
bool parseIso(const std::string& s, long long& t) {
    int Y, M, D, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &Y, &M, &D, &h, &mi, &se) != 6) {
        DIAG_DEBUG(412679, "The WeFAX archive time %s is not in the form year-month-dayThour:minute:secondZ, so it is ignored.", s.c_str());
        return false;
    }
    t = daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}
} // namespace

std::string wefaxArchiveBase(long long startUtc, double dialKhz) {
    const std::string iso = utcIso(startUtc);   // 2026-10-22T12:30:45Z
    char b[96];
    snprintf(b, sizeof b, "wefax_%.4s%.2s%.2sT%.2s%.2s%.2sZ_%.1fkHz", iso.c_str(), iso.c_str() + 5, iso.c_str() + 8,
             iso.c_str() + 11, iso.c_str() + 14, iso.c_str() + 17, dialKhz);
    return b;
}

std::string wefaxArchiveUniqueBase(const std::string& base, const std::vector<std::string>& taken) {
    auto isTaken = [&](const std::string& s) { return std::find(taken.begin(), taken.end(), s) != taken.end(); };
    if (!isTaken(base)) return base;
    for (int i = 2; i < 10000; i++) {
        const std::string c = base + "_" + std::to_string(i);
        if (!isTaken(c)) return c;
    }
    return base + "_x";
}

bool wefaxArchiveParseBase(const std::string& base, long long& startUtc, double& dialKhz) {
    int Y, M, D, h, mi, se; double k;
    if (sscanf(base.c_str(), "wefax_%4d%2d%2dT%2d%2d%2dZ_%lfkHz", &Y, &M, &D, &h, &mi, &se, &k) != 7) {
        DIAG_DEBUG(222342, "The archive file name %s does not follow the WeFAX picture naming, so it is skipped.", base.c_str());
        return false;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || se > 60) {
        DIAG_DEBUG(366294, "The archive file name %s holds a date or time that does not exist, so it is skipped.", base.c_str());
        return false;
    }
    startUtc = daysFromCivil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + se;
    dialKhz = k;
    return true;
}

std::string wefaxMetaToJson(const WefaxArchiveMeta& m) {
    char b[1536];
    snprintf(b, sizeof b,
        "{\n  \"app\": \"OnAir\",\n  \"mode\": \"wefax\",\n  \"dial_khz\": %.3f,\n  \"ioc\": %d,\n  \"lpm\": %d,\n"
        "  \"start_utc\": \"%s\",\n  \"stop_utc\": \"%s\",\n  \"start_unix\": %lld,\n  \"stop_unix\": %lld,\n  \"width\": %d,\n  \"height\": %d,\n"
        "  \"station\": \"%s\",\n  \"station_id\": \"%s\",\n  \"snr_db\": %.1f,\n  \"phasing_lines\": %d,\n  \"end_reason\": \"%s\",\n  \"starred\": %s\n}\n",
        m.dialKhz, m.ioc, m.lpm, utcIso(m.startUtc).c_str(), utcIso(m.stopUtc).c_str(), m.startUtc, m.stopUtc, m.width, m.height,
        jsonEsc(m.station).c_str(), jsonEsc(m.stationId).c_str(), m.snrDb, m.phasingLines, jsonEsc(m.endReason).c_str(), m.starred ? "true" : "false");
    return b;
}

bool wefaxMetaFromJson(const std::string& j, WefaxArchiveMeta& m) {
    std::string v;
    if (!jsonGet(j, "mode", v) || v != "wefax") {
        DIAG_DEBUG(738426, "The archive details file of %zu bytes does not say mode wefax, so it is not read as WeFAX picture details.", j.size());
        return false;
    }
    WefaxArchiveMeta r;
    if (jsonGet(j, "dial_khz", v)) r.dialKhz = std::atof(v.c_str());
    if (jsonGet(j, "ioc", v)) r.ioc = std::atoi(v.c_str());
    if (jsonGet(j, "lpm", v)) r.lpm = std::atoi(v.c_str());
    if (jsonGet(j, "start_unix", v)) r.startUtc = std::atoll(v.c_str());
    else if (jsonGet(j, "start_utc", v)) parseIso(v, r.startUtc);
    if (jsonGet(j, "stop_unix", v)) r.stopUtc = std::atoll(v.c_str());
    else if (jsonGet(j, "stop_utc", v)) parseIso(v, r.stopUtc);
    if (jsonGet(j, "width", v)) r.width = std::atoi(v.c_str());
    if (jsonGet(j, "height", v)) r.height = std::atoi(v.c_str());
    if (jsonGet(j, "station", v)) r.station = v;
    if (jsonGet(j, "station_id", v)) r.stationId = v;
    if (jsonGet(j, "snr_db", v)) r.snrDb = std::atof(v.c_str());
    if (jsonGet(j, "phasing_lines", v)) r.phasingLines = std::atoi(v.c_str());
    if (jsonGet(j, "end_reason", v)) r.endReason = v;
    if (jsonGet(j, "starred", v)) r.starred = (v == "true");
    m = r;
    return true;
}

const WefaxStation* wefaxStationForDial(double dialKhz, double tolKhz) {
    const WefaxStation* best = nullptr; double bd = tolKhz + 1;
    for (const WefaxStation& st : wefaxStations())
        for (double k : st.carrierKhz) {
            const double d = std::min(std::fabs(dialKhz - wefaxDialKhz(k)), std::fabs(dialKhz - k));
            if (d <= tolKhz && d < bd) { bd = d; best = &st; }
        }
    return best;
}

std::vector<size_t> wefaxCleanupPlan(const std::vector<WefaxArchiveEntry>& e, long long budgetBytes) {
    std::vector<size_t> out;
    if (budgetBytes <= 0) return out;
    long long total = 0;
    for (const auto& x : e) total += x.bytes;
    std::vector<size_t> idx(e.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return e[a].startUtc < e[b].startUtc; });
    for (size_t i : idx) {
        if (total <= budgetBytes) break;
        if (e[i].starred) continue;
        out.push_back(i);
        total -= e[i].bytes;
    }
    return out;
}

std::vector<size_t> wefaxGalleryList(const std::vector<WefaxArchiveEntry>& e, WefaxSort sort, const std::string& query, bool starredOnly) {
    const std::string q = lower(query);
    std::vector<size_t> out;
    for (size_t i = 0; i < e.size(); i++) {
        if (starredOnly && !e[i].starred) continue;
        if (!q.empty()) {
            char kb[32]; snprintf(kb, sizeof kb, "%.1f", e[i].dialKhz);
            const std::string hay = lower(e[i].station + " " + e[i].base + " " + kb);
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        switch (sort) {
        case WefaxSort::TimeOldest: return e[a].startUtc < e[b].startUtc;
        case WefaxSort::Station: {
            const std::string sa = lower(e[a].station), sb = lower(e[b].station);
            if (sa.empty() != sb.empty()) return sb.empty();     // unknown stations last
            if (sa != sb) return sa < sb;
            return e[a].startUtc > e[b].startUtc;
        }
        case WefaxSort::Frequency:
            if (e[a].dialKhz != e[b].dialKhz) return e[a].dialKhz < e[b].dialKhz;
            return e[a].startUtc > e[b].startUtc;
        default: return e[a].startUtc > e[b].startUtc;
        }
    });
    return out;
}

void wefaxThumbSize(int w, int h, int maxDim, int& tw, int& th) {
    tw = std::max(1, w); th = std::max(1, h);
    if (maxDim < 1 || w <= 0 || h <= 0) return;
    const double s = std::min(1.0, (double)maxDim / std::max(w, h));
    tw = std::max(1, (int)std::lround(w * s));
    th = std::max(1, (int)std::lround(h * s));
}

void wefaxThumbnail(const uint8_t* pix, int w, int h, int tw, int th, std::vector<uint8_t>& out) {
    out.assign((size_t)std::max(0, tw) * (size_t)std::max(0, th), 255);
    if (!pix || w <= 0 || h <= 0 || tw <= 0 || th <= 0) return;
    for (int y = 0; y < th; y++) {
        const int y0 = (int)((long long)y * h / th), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * h / th));
        for (int x = 0; x < tw; x++) {
            const int x0 = (int)((long long)x * w / tw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * w / tw));
            long long sum = 0; int n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) { sum += pix[(size_t)yy * w + xx]; n++; }
            out[(size_t)y * tw + x] = n ? (uint8_t)((sum + n / 2) / n) : 255;
        }
    }
}

} // namespace dect2
