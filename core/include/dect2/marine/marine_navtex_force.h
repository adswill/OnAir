// NAVTEX "warnings in force" (3.9, docs/SMART5-field-3.9.md item 5): the set of warnings each station has out now, kept from the
// messages this receiver decoded. A warning (subject A navigational, B meteorological, D search and rescue / piracy, J GNSS, K other
// navaids, L further navigational) enters the set; it leaves it when
//  - another message cancels it: "CANCEL NAV WARN 123/24", "CANCEL NAVAREA I 045/24", "CANCEL WZ 1093", "CANCEL EA42",
//    "CANCELS 117/24 AND 118/24" (references are the series numbers in the warnings' first lines, the UK "WZ nnnn" numbers or the
//    NAVTEX header B1 B2 B3 B4),
//  - its own text says "CANCEL THIS MSG 241800 UTC NOV 24" and that time has passed,
//  - the station sends a list of the warnings in force ("... IN FORCE ... 2024 SERIES: 081 085 112") that does not name it,
//  - it is older than the assumed lifetime of its subject (B 24 h, D 72 h, others 60 days): marked "assumed", since NAVTEX itself gives
//    no expiry for those.
// A repeat of the same message (same header and text) only updates the time. Damaged references ('*') are not used. Kept between runs
// (save()/load()). Pure logic, no clock: times are Unix seconds (UTC).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

struct NavtexWarning {
    char station = '?', subject = '?';
    std::string header;                  // "EA42"
    std::vector<std::string> refs;       // "123/24", "WZ1093", "EA42"
    std::string title;                   // the first meaningful line
    std::string text;
    int64_t firstRx = 0, lastRx = 0;
    int64_t expires = 0;                 // from "CANCEL THIS MSG", 0 none
};

struct NavtexChange {
    std::string kind;                    // "new", "cancelled", "expired", "dropped" (not in the in-force list), "aged" (assumed lifetime)
    NavtexWarning w;
    std::string by;                      // the cancelling header, when cancelled
    int64_t t = 0;
};

class NavtexInForce {
public:
    double lifeMetSec = 24 * 3600, lifeSarSec = 72 * 3600, lifeOtherSec = 60 * 86400.0;

    static bool isWarning(char subject) { return subject == 'A' || subject == 'B' || subject == 'D' || subject == 'J' || subject == 'K' || subject == 'L'; }

    // one decoded message (station B1, subject B2, header "B1B2B3B4", text); changes are appended to `out`
    void add(char station, char subject, const std::string& header, const std::string& textIn, int64_t rx, std::vector<NavtexChange>& out) {
        const std::string text = upper(textIn);
        if (station < 'A' || station > 'Z') return;
        // 1. cancellations of other warnings
        const auto cancels = cancelRefs(text);
        if (!cancels.empty()) {
            auto& set = st_[station];
            for (auto it = set.begin(); it != set.end();) {
                bool hit = false;
                for (const auto& r : it->refs) if (cancels.count(r)) { hit = true; break; }
                if (hit && it->header != header) { out.push_back({"cancelled", *it, header, rx}); it = set.erase(it); } else ++it;
            }
        }
        // 2. an in-force list
        if (text.find("IN FORCE") != std::string::npos && applyInForce(station, text, rx, out)) return;   // the list itself is no warning
        if (!isWarning(subject) || header.find('*') != std::string::npos) return;
        // 3. the warning itself
        NavtexWarning w;
        w.station = station; w.subject = subject; w.header = header; w.text = textIn; w.firstRx = w.lastRx = rx;
        w.refs = ownRefs(text);
        if (header.size() == 4 && header.substr(2) != "00") w.refs.push_back(header);
        w.title = titleOf(textIn);
        w.expires = selfExpiry(text, rx);
        auto& set = st_[station];
        for (auto& x : set) {
            if (x.header == header && header.substr(2) != "00") {
                if (x.text == textIn || similar(x.text, textIn)) { x.lastRx = rx; return; }   // a repeat
                out.push_back({"replaced", x, header, rx});
                x = w;
                out.push_back({"new", w, "", rx});
                return;
            }
        }
        if (w.expires && w.expires <= rx) return;   // heard after its own end
        set.push_back(w);
        out.push_back({"new", w, "", rx});
    }

    // expiries and assumed lifetimes
    void tick(int64_t now, std::vector<NavtexChange>& out) {
        for (auto& [s, set] : st_) {
            for (auto it = set.begin(); it != set.end();) {
                const double life = it->subject == 'B' ? lifeMetSec : it->subject == 'D' ? lifeSarSec : lifeOtherSec;
                if (it->expires && now >= it->expires) { out.push_back({"expired", *it, "", now}); it = set.erase(it); }
                else if (!it->expires && now - it->lastRx > life) { out.push_back({"aged", *it, "", now}); it = set.erase(it); }
                else ++it;
            }
        }
    }

    const std::vector<NavtexWarning>& inForce(char station) const { static const std::vector<NavtexWarning> none; auto it = st_.find(station); return it == st_.end() ? none : it->second; }
    std::vector<char> stations() const { std::vector<char> v; for (const auto& [s, set] : st_) if (!set.empty()) v.push_back(s); return v; }
    size_t total() const { size_t n = 0; for (const auto& [s, set] : st_) n += set.size(); return n; }

    // ---- parsing helpers (public for the tests)
    static std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
    // "123/24" style numbers and "WZ 1093" in the first three lines: what other messages use to name this one
    static std::vector<std::string> ownRefs(const std::string& text) {
        std::vector<std::string> v;
        std::istringstream in(text);
        std::string l; int lines = 0;
        while (lines < 3 && std::getline(in, l)) {
            if (l.find_first_not_of(" \r\t") == std::string::npos) continue;
            lines++;
            if (l.find("CANCEL") != std::string::npos) continue;
            for (const auto& r : numbers(l)) if (std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
        }
        return v;
    }
    // what a message cancels: the references after each "CANCEL" up to the end of that sentence, except "CANCEL THIS ..."
    static std::set<std::string> cancelRefs(const std::string& text) {
        std::set<std::string> s;
        size_t p = 0;
        while ((p = text.find("CANCEL", p)) != std::string::npos) {
            size_t q = p + 6;
            while (q < text.size() && std::isalpha((unsigned char)text[q])) q++;   // CANCELS, CANCELLED
            size_t e = text.find_first_of(".\n", q);
            if (e == std::string::npos) e = text.size();
            std::string part = text.substr(q, e - q);
            const size_t ns = part.find_first_not_of(' ');
            if (ns != std::string::npos && part.compare(ns, 4, "THIS") == 0) { p = e; continue; }
            for (const auto& r : numbers(part)) s.insert(r);
            for (const auto& h : headers(part)) s.insert(h);
            p = e;
        }
        return s;
    }
    // the time in "CANCEL THIS MSG 241800 UTC NOV 24" (DDHHMM, month, year), 0 when there is none
    static int64_t selfExpiry(const std::string& text, int64_t rx) {
        const size_t p = text.find("CANCEL THIS");
        if (p == std::string::npos) return 0;
        std::istringstream in(text.substr(p + 11, 80));
        std::string w, dtg, mon, yr;
        while (in >> w) {
            std::string d; for (char c : w) if (std::isdigit((unsigned char)c)) d += c;
            if (dtg.empty() && d.size() == 6 && (w.size() == 6 || w.back() == 'Z' || w.back() == 'z' || w.find("UTC") != std::string::npos)) { dtg = d; continue; }
            if (!dtg.empty() && mon.empty() && monthOf(w) >= 0) { mon = w; continue; }
            if (!mon.empty() && yr.empty() && (d.size() == 2 || d.size() == 4) && d.size() == w.size()) { yr = d; break; }
        }
        if (dtg.empty()) return 0;
        tm g{};
        const time_t rt = (time_t)rx;
#ifdef _WIN32
        gmtime_s(&g, &rt);
#else
        gmtime_r(&rt, &g);
#endif
        int year = g.tm_year + 1900, month = g.tm_mon;
        if (!mon.empty()) month = monthOf(mon);
        if (!yr.empty()) year = yr.size() == 2 ? 2000 + std::stoi(yr) : std::stoi(yr);
        const int day = std::stoi(dtg.substr(0, 2)), hh = std::stoi(dtg.substr(2, 2)), mm = std::stoi(dtg.substr(4, 2));
        if (day < 1 || day > 31 || hh > 23 || mm > 59) return 0;
        int64_t t = utcOf(year, month, day, hh, mm);
        if (mon.empty() && t < rx - 86400) t = utcOf(month == 11 ? year + 1 : year, (month + 1) % 12, day, hh, mm);   // no month: the next such day
        return t;
    }

    std::string save() const {
        std::ostringstream o;
        o << "navtexforce 1\n";
        for (const auto& [s, set] : st_)
            for (const auto& w : set) {
                std::string t = w.text; for (auto& c : t) if (c == '\n') c = '\x1f';
                o << "W\t" << w.header << "\t" << w.station << w.subject << "\t" << w.firstRx << "\t" << w.lastRx << "\t" << w.expires << "\t" << t << "\n";
            }
        return o.str();
    }
    bool load(const std::string& text) {
        std::istringstream in(text);
        std::string l;
        if (!std::getline(in, l) || l.rfind("navtexforce 1", 0) != 0) return false;
        while (std::getline(in, l)) {
            std::vector<std::string> f; std::string x; std::istringstream ls(l);
            while (std::getline(ls, x, '\t')) f.push_back(x);
            if (f.size() < 7 || f[0] != "W" || f[2].size() != 2) continue;
            NavtexWarning w;
            w.header = f[1]; w.station = f[2][0]; w.subject = f[2][1];
            w.firstRx = std::atoll(f[3].c_str()); w.lastRx = std::atoll(f[4].c_str()); w.expires = std::atoll(f[5].c_str());
            w.text = f[6]; for (auto& c : w.text) if (c == '\x1f') c = '\n';
            const std::string u = upper(w.text);
            w.refs = ownRefs(u);
            if (w.header.size() == 4 && w.header.substr(2) != "00") w.refs.push_back(w.header);
            w.title = titleOf(w.text);
            st_[w.station].push_back(w);
        }
        return true;
    }

private:
    std::map<char, std::vector<NavtexWarning>> st_;

    static int monthOf(const std::string& w) {
        static const char* const m[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        for (int i = 0; i < 12; i++) if (w.compare(0, 3, m[i]) == 0) return i;
        return -1;
    }
    static int64_t utcOf(int y, int mon, int d, int h, int mi) {   // days from civil (Howard Hinnant)
        y -= mon < 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400), mp = (unsigned)((mon + 10) % 12);
        const unsigned doy = (153 * mp + 2) / 5 + (unsigned)d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
        return days * 86400 + h * 3600 + mi * 60;
    }
    // "123/24" -> "123/24" (leading zeros dropped: "081/24" = "81/24"), "WZ 1093" -> "WZ1093"
    static std::vector<std::string> numbers(const std::string& l) {
        std::vector<std::string> v;
        for (size_t i = 0; i < l.size(); i++) {
            if (!std::isdigit((unsigned char)l[i]) || (i > 0 && (std::isalnum((unsigned char)l[i - 1]) || l[i - 1] == '/'))) continue;
            size_t j = i; while (j < l.size() && std::isdigit((unsigned char)l[j])) j++;
            if (j < l.size() && l[j] == '/' && j - i <= 4) {
                size_t k = j + 1; while (k < l.size() && std::isdigit((unsigned char)l[k])) k++;
                if ((k - j - 1 == 2 || k - j - 1 == 4) && (k == l.size() || !std::isalnum((unsigned char)l[k]))) {
                    std::string yy = l.substr(j + 1, k - j - 1); if (yy.size() == 4) yy = yy.substr(2);
                    v.push_back(std::to_string(std::stoi(l.substr(i, j - i))) + "/" + yy);
                }
                i = k; continue;
            }
            i = j;
        }
        for (size_t p = 0; (p = l.find("WZ ", p)) != std::string::npos; p += 3) {
            if (p > 0 && std::isalnum((unsigned char)l[p - 1])) continue;
            size_t j = p + 3; std::string d; while (j < l.size() && std::isdigit((unsigned char)l[j])) d += l[j++];
            if (d.size() >= 2 && d.size() <= 4) v.push_back("WZ" + std::to_string(std::stoi(d)));
        }
        return v;
    }
    // NAVTEX headers named in a cancellation: two letters and two digits ("EA42"), the subject one of the warning letters
    static std::vector<std::string> headers(const std::string& l) {
        std::vector<std::string> v;
        for (size_t i = 0; i + 4 <= l.size(); i++) {
            if (i > 0 && std::isalnum((unsigned char)l[i - 1])) continue;
            if (i + 4 < l.size() && std::isalnum((unsigned char)l[i + 4])) continue;
            if (std::isupper((unsigned char)l[i]) && isWarning(l[i + 1]) && std::isdigit((unsigned char)l[i + 2]) && std::isdigit((unsigned char)l[i + 3]))
                v.push_back(l.substr(i, 4));
        }
        return v;
    }
    static std::string titleOf(const std::string& text) {
        std::istringstream in(text);
        std::string l, first;
        while (std::getline(in, l)) {
            const size_t a = l.find_first_not_of(" \r\t");
            if (a == std::string::npos) continue;
            l = l.substr(a);
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (first.empty()) first = l;
            // skip the date-time group line ("111200 UTC NOV 24") and the bare series line
            bool dtg = l.size() >= 6 && std::isdigit((unsigned char)l[0]) && l.find("UTC") != std::string::npos && l.size() < 24;
            if (!dtg) return l.size() > 90 ? l.substr(0, 90) : l;
        }
        return first;
    }
    static bool similar(const std::string& a, const std::string& b) {   // the same message with a few damaged characters
        if (a.size() < 20 || b.size() < 20) return false;
        const size_t n = std::min(a.size(), b.size());
        if (std::max(a.size(), b.size()) - n > n / 20) return false;
        size_t diff = 0; for (size_t i = 0; i < n; i++) if (a[i] != b[i] && a[i] != '*' && b[i] != '*') diff++;
        return diff <= n / 50;
    }
    // "... IN FORCE ..." lists: the series numbers named; warnings of this station whose references use a listed year but are not named leave
    bool applyInForce(char station, const std::string& text, int64_t rx, std::vector<NavtexChange>& out) {
        std::set<std::string> named;
        std::set<std::string> years;
        std::istringstream in(text);
        std::string l, year;
        bool after = false;
        while (std::getline(in, l)) {
            if (l.find("IN FORCE") != std::string::npos) after = true;
            if (!after) continue;
            for (const auto& r : numbers(l)) { named.insert(r); years.insert(r.substr(r.find('/') + 1)); }
            const size_t sp = l.find("SERIES");
            std::istringstream ws(l);
            std::string w;
            while (ws >> w) {
                std::string d; for (char c : w) if (std::isdigit((unsigned char)c)) d += c;
                if (d.size() == 4 && d.size() + (w.back() == ':' ? 1 : 0) >= w.size() && sp != std::string::npos && l.find(w) < sp) { year = d.substr(2); years.insert(year); continue; }
                if (!year.empty() && !d.empty() && d.size() <= 4 && d.size() + 1 >= w.size()) named.insert(std::to_string(std::stoi(d)) + "/" + year);
            }
        }
        if (named.empty()) return false;
        auto& set = st_[station];
        for (auto it = set.begin(); it != set.end();) {
            bool listedYear = false, inList = false;
            for (const auto& r : it->refs) {
                const size_t sl = r.find('/');
                if (sl == std::string::npos) continue;
                if (years.count(r.substr(sl + 1))) listedYear = true;
                if (named.count(r)) inList = true;
            }
            if (listedYear && !inList && it->lastRx < rx) { out.push_back({"dropped", *it, "", rx}); it = set.erase(it); } else ++it;
        }
        return true;
    }
};

} // namespace marine
} // namespace dect2
