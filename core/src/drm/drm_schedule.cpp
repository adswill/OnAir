// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DRM broadcast schedule page: its JSON (short wave) and its medium wave table, and what is on air at a given time. See drm_schedule.h.
#include "dect2/drm/drm_schedule.h"
#include "dect2/mcp/mcp.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace dect2::drmsched {

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// the text of an HTML fragment: tags dropped (a closing span or cell is a space), the few entities the page uses decoded, spaces folded
std::string text(const std::string& h) {
    std::string o;
    for (size_t i = 0; i < h.size(); i++) {
        if (h[i] == '<') { size_t e = h.find('>', i); if (e == std::string::npos) break; o += ' '; i = e; continue; }
        if (h[i] == '&') {
            static const struct { const char* e; const char* t; } kE[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#039;", "'"}, {"&nbsp;", " "}, {"&ndash;", "-"}};
            bool hit = false;
            for (const auto& x : kE) if (!h.compare(i, std::strlen(x.e), x.e)) { o += x.t; i += std::strlen(x.e) - 1; hit = true; break; }
            if (hit) continue;
        }
        o += h[i];
    }
    std::string f;
    bool sp = false;
    for (char c : o) { if (std::isspace((unsigned char)c)) { sp = !f.empty(); continue; } if (sp) f += ' '; sp = false; f += c; }
    return f;
}

std::string str(const mcp::Json& a, size_t i) { return i < a.arr.size() && a.arr[i].isStr() ? trim(a.arr[i].str) : std::string(); }

void parseJsonPart(const std::string& html, Schedule& out) {
    const size_t k = html.find("id=\"drm-sw-data\"");
    if (k == std::string::npos) return;
    const size_t b = html.find('>', k), e = html.find("</script>", k);
    if (b == std::string::npos || e == std::string::npos || e <= b) return;
    mcp::Json j;
    std::string err;
    if (!mcp::parseJson(html.substr(b + 1, e - b - 1), j, err) || !j.isObj()) return;
    out.season = j.strOr("season");
    out.validFrom = j.strOr("validFrom");
    out.validTo = j.strOr("validTo");
    out.updatedAt = j.strOr("updatedAt");
    const mcp::Json* rs = j.get("records");
    if (!rs || !rs->isArr()) return;
    for (const mcp::Json& r : rs->arr) {
        const mcp::Json* v = r.get("values");
        if (!r.isObj() || !v || !v->isArr() || v->arr.size() < 3) continue;
        Broadcast x;
        x.id = r.strOr("id");
        x.region = r.strOr("region");
        x.timeText = str(*v, 0); x.station = str(*v, 1); x.freqText = str(*v, 2); x.days = str(*v, 3); x.language = str(*v, 4);
        x.target = str(*v, 5); x.site = str(*v, 6); x.power = str(*v, 7); x.azimuth = str(*v, 8);
        x.confidence = r.strOr("confidence"); x.label = r.strOr("label"); x.note = r.strOr("note");
        x.khz = parseKhz(x.freqText);
        parseTimes(x.timeText, x.windows);
        if (x.station.empty() || x.khz.empty()) continue;
        out.items.push_back(std::move(x));
    }
}

void parseMediumWave(const std::string& html, Schedule& out) {
    const size_t t = html.find("id=\"drm-india-schedule\"");
    size_t p = html.find("<tbody", t == std::string::npos ? 0 : t);
    if (t == std::string::npos || p == std::string::npos) return;
    const size_t end = html.find("</tbody>", p);
    while (true) {
        const size_t r = html.find("<tr", p);
        if (r == std::string::npos || r > end) break;
        const size_t re = html.find("</tr>", r);
        if (re == std::string::npos) break;
        std::vector<std::string> cells;
        size_t c = r;
        while (true) {
            const size_t td = html.find("<td", c);
            if (td == std::string::npos || td > re) break;
            const size_t cs = html.find('>', td), ce = html.find("</td>", td);
            if (cs == std::string::npos || ce == std::string::npos || ce > re) break;
            cells.push_back(text(html.substr(cs + 1, ce - cs - 1)));
            c = ce + 5;
        }
        p = re + 5;
        if (cells.size() < 2) continue;
        Broadcast x;
        x.mediumWave = true;
        x.region = "region_3";
        x.station = "All India Radio";
        x.site = cells[0];
        x.target = "India";
        x.freqText = cells[1];
        x.power = cells.size() > 2 ? cells[2] : std::string();
        x.days = "Not listed";
        x.label = "Medium wave transmitter (times not listed)";
        x.khz = parseKhz(x.freqText);
        if (!x.khz.empty()) out.items.push_back(std::move(x));
    }
}

}  // namespace

bool parseTimes(const std::string& s, std::vector<Window>& out) {
    out.clear();
    for (size_t i = 0; i + 9 <= s.size(); ) {
        // HHMM-HHMM
        auto dig = [&](size_t k) { return k < s.size() && std::isdigit((unsigned char)s[k]); };
        if (dig(i) && dig(i + 1) && dig(i + 2) && dig(i + 3) && i + 4 < s.size() && (s[i + 4] == '-' || s[i + 4] == '/') && dig(i + 5) && dig(i + 6) && dig(i + 7) && dig(i + 8) &&
            !dig(i + 9) && (i == 0 || !dig(i - 1))) {
            const int a = std::atoi(s.substr(i, 2).c_str()) * 60 + std::atoi(s.substr(i + 2, 2).c_str());
            const int b = std::atoi(s.substr(i + 5, 2).c_str()) * 60 + std::atoi(s.substr(i + 7, 2).c_str());
            if (a <= 24 * 60 && b <= 24 * 60) out.push_back({a % (24 * 60), b == 24 * 60 ? 24 * 60 : b});
            i += 9;
        } else i++;
    }
    return !out.empty();
}

std::vector<double> parseKhz(const std::string& s) {
    std::vector<double> v;
    for (size_t i = 0; i < s.size(); ) {
        if (!std::isdigit((unsigned char)s[i])) { i++; continue; }
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) j++;
        const double f = std::atof(s.substr(i, j - i).c_str());
        if (f >= 100 && f <= 30000 && std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);   // (dates such as "30 Aug" are below 100)
        i = j;
    }
    return v;
}

int dayMatches(const std::string& daysIn, int wd) {
    const std::string d = lower(daysIn);
    if (d.find("daily") != std::string::npos || d.find("every day") != std::string::npos) return 1;
    static const char* kD[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    // the day names in order of appearance, and whether a '-' joins two of them (a range)
    std::vector<std::pair<int, size_t>> found;
    for (size_t i = 0; i + 3 <= d.size(); i++) {
        if (i > 0 && std::isalpha((unsigned char)d[i - 1])) continue;
        for (int k = 0; k < 7; k++) if (!d.compare(i, 3, kD[k])) { found.push_back({k, i}); break; }
    }
    if (found.empty()) return -1;
    std::vector<bool> on(7, false);
    for (size_t n = 0; n < found.size(); n++) {
        on[(size_t)found[n].first] = true;
        if (n + 1 < found.size()) {
            const std::string between = d.substr(found[n].second, found[n + 1].second - found[n].second);
            if (between.find('-') != std::string::npos || between.find(" to ") != std::string::npos) {
                for (int k = found[n].first; k != found[n + 1].first; k = (k + 1) % 7) on[(size_t)k] = true;
            }
        }
    }
    return on[(size_t)(wd % 7)] ? 1 : 0;
}

OnAir onAirAt(const Broadcast& b, std::time_t utc) {
    if (b.windows.empty()) return OnAir::Unknown;
    std::tm t{};
#ifdef _WIN32
    gmtime_s(&t, &utc);
#else
    gmtime_r(&utc, &t);
#endif
    const int m = t.tm_hour * 60 + t.tm_min;
    for (const Window& w : b.windows) {
        bool in = false;
        int wd = t.tm_wday;
        if (w.endMin > w.startMin) in = m >= w.startMin && m < w.endMin;
        else {   // past midnight: a minute after midnight belongs to the window that started the day before
            if (m >= w.startMin) in = true;
            else if (m < w.endMin) { in = true; wd = (wd + 6) % 7; }
        }
        if (!in) continue;
        const int dm = dayMatches(b.days, wd);
        if (dm == 1) return OnAir::Yes;
        if (dm == -1) return OnAir::Maybe;
    }
    return OnAir::No;
}

int minutesToNext(const Broadcast& b, std::time_t utc) {
    if (b.windows.empty()) return -1;
    const OnAir o = onAirAt(b, utc);
    if (o == OnAir::Yes || o == OnAir::Maybe) return 0;
    std::tm t{};
#ifdef _WIN32
    gmtime_s(&t, &utc);
#else
    gmtime_r(&utc, &t);
#endif
    const int now = t.tm_hour * 60 + t.tm_min;
    int best = -1;
    for (int day = 0; day <= 7; day++) {   // the starts of the next eight days, on the days the entry runs (or may run)
        const int wd = (t.tm_wday + day) % 7;
        if (dayMatches(b.days, wd) == 0) continue;
        for (const Window& w : b.windows) {
            const int k = day * 24 * 60 + w.startMin - now;
            if (k > 0 && (best < 0 || k < best)) best = k;
        }
    }
    return best;
}

const char* regionName(const std::string& r) {
    if (r == "region_1") return "Europe, Middle East, Africa";
    if (r == "region_2") return "Asia";
    if (r == "region_3") return "India (medium wave)";
    if (r == "region_4") return "Pacific";
    if (r == "region_5") return "Spain and Latin America";
    return "Other";
}

bool parsePage(const std::string& html, Schedule& out, std::string& err) {
    out = Schedule();
    parseJsonPart(html, out);
    parseMediumWave(html, out);
    if (out.items.empty()) { DIAG(497861, "The DRM Association broadcast schedule page holds no entries OnAir can read (its layout may have changed), so the schedule list stays empty."); err = "the page has no schedule OnAir can read (its layout may have changed)"; return false; }
    return true;
}

}  // namespace dect2::drmsched
