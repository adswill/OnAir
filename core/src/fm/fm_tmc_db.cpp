// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RDS-TMC event lists and location tables from the user's files, messages in words and the TMC log (dect2/fm/tmc_db.h).
// The column names below are those of ISO 14819-2 / -3 event lists, the LCL lists (German BASt and others) and the TISA location table
// exchange format as they are commonly published; they are recognised loosely (case, spaces and punctuation ignored) and the content
// decides where the names say nothing.
#include "dect2/fm/tmc_db.h"
#include "dect2/common/text_charset.h"
#include "dect2/common/timecompat.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>

namespace dect2 {
namespace tmcdb {

namespace {

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// Windows-1252 0x80 - 0x9F (0 = undefined, kept as Latin-1)
const uint16_t kCp1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
                              0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool toInt(const std::string& s0, int& v) {
    const std::string s = trim(s0);
    if (s.empty() || s.size() > 10) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; i = 1; }
    if (i >= s.size()) return false;
    long long x = 0;
    for (; i < s.size(); i++) {
        if (s[i] == '.' && i + 1 < s.size() && s.find_first_not_of('0', i + 1) == std::string::npos) break;   // "123.0" from a spreadsheet
        if (!std::isdigit((unsigned char)s[i])) return false;
        x = x * 10 + (s[i] - '0');
    }
    v = (int)(neg ? -x : x);
    return true;
}

int intOr0(const std::string& s) { int v = 0; return toInt(s, v) ? v : 0; }

bool hasLetters(const std::string& s) {
    for (unsigned char c : s) if (std::isalpha(c) || c >= 0x80) return true;
    return false;
}

// The column whose normalised name is one of names (exact), else the first that starts with one of prefixes; -1 none
int findCol(const std::vector<std::string>& norm, std::initializer_list<const char*> names, std::initializer_list<const char*> prefixes = {}) {
    for (const char* n : names) for (size_t i = 0; i < norm.size(); i++) if (norm[i] == n) return (int)i;
    for (const char* p : prefixes) for (size_t i = 0; i < norm.size(); i++) if (norm[i].rfind(p, 0) == 0) return (int)i;
    return -1;
}

std::string field(const std::vector<std::string>& r, int c) { return c >= 0 && (size_t)c < r.size() ? trim(r[(size_t)c]) : std::string(); }

// The language a column name, a file name or a text says
std::string langOfName(const std::string& n) {
    const std::string s = lower(n);
    // whole tokens of the name ("text_de", "EL-DEU", "events english")
    std::vector<std::string> tok;
    std::string cur;
    for (char c : s) { if (std::isalnum((unsigned char)c)) cur += c; else { if (!cur.empty()) tok.push_back(cur); cur.clear(); } }
    if (!cur.empty()) tok.push_back(cur);
    for (const std::string& t : tok) {
        if (t == "de" || t == "deu" || t == "ger" || t == "german" || t == "deutsch" || t == "dt") return "de";
        if (t == "en" || t == "eng" || t == "english" || t == "englisch" || t == "gb" || t == "uk") return "en";
        if (t == "fr" || t == "fra" || t == "fre" || t == "french" || t == "francais") return "fr";
        if (t == "nl" || t == "nld" || t == "dut" || t == "dutch") return "nl";
        if (t == "it" || t == "ita" || t == "italian") return "it";
        if (t == "es" || t == "spa" || t == "esp" || t == "spanish") return "es";
    }
    for (const char* k : {"deutsch", "german"}) if (s.find(k) != std::string::npos) return "de";
    for (const char* k : {"english", "englisch"}) if (s.find(k) != std::string::npos) return "en";
    return "";
}

std::string langOfText(const std::vector<std::string>& samples) {
    static const char* const kDe[] = {"stau", "gesperrt", "unfall", "verkehr", "baustelle", "zähflüssig", "fahrbahn", "straße", "glätte", "nebel", " und ", " auf ", " der ", " die ", "ä", "ö", "ü", "ß"};
    static const char* const kEn[] = {"traffic", "closed", "accident", "queue", "roadworks", "slow", "road", "lane", "fog", "delays", " and ", " the ", " of ", " on "};
    int de = 0, en = 0;
    for (const std::string& s0 : samples) {
        const std::string s = " " + lower(s0) + " ";
        for (const char* k : kDe) if (s.find(k) != std::string::npos) de++;
        for (const char* k : kEn) if (s.find(k) != std::string::npos) en++;
    }
    if (de == 0 && en == 0) return "xx";
    return de > en ? "de" : "en";
}

std::string stripQ(std::string s) {   // "(Q) km of queue" without a quantifier: "km of queue" reads badly, so the placeholder and its unit word go
    for (const char* q : {"(Q) ", "(q) ", " (Q)", " (q)", "(Q)", "(q)"}) {
        size_t p;
        while ((p = s.find(q)) != std::string::npos) s.erase(p, strlen(q));
    }
    return trim(s);
}

std::string fold(const std::string& s) {   // control characters out, runs of spaces folded, trimmed
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
        o += (char)c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string fmtInt(const char* f, int v) { char b[96]; snprintf(b, sizeof b, f, v); return b; }

} // namespace

std::string normCol(const std::string& name) {
    std::string o;
    for (unsigned char c : name) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

std::string toUtf8(const std::string& raw) {
    std::string s = raw;
    if (s.size() >= 2 && (((unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE) || ((unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF))) {   // UTF-16 (a spreadsheet's "Unicode text")
        const bool le = (unsigned char)s[0] == 0xFF;
        std::string o;
        for (size_t i = 2; i + 1 < s.size(); i += 2) {
            uint32_t u = le ? ((unsigned char)s[i] | (unsigned char)s[i + 1] << 8) : ((unsigned char)s[i] << 8 | (unsigned char)s[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
                const uint32_t lo = le ? ((unsigned char)s[i + 2] | (unsigned char)s[i + 3] << 8) : ((unsigned char)s[i + 2] << 8 | (unsigned char)s[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            text::appendUtf8(o, u);
        }
        return o;
    }
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    if (validUtf8(s)) return s;
    std::string o;
    o.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) {
        if (c < 0x80) o += (char)c;
        else if (c < 0xA0 && kCp1252[c - 0x80]) text::appendUtf8(o, kCp1252[c - 0x80]);
        else text::appendUtf8(o, c);
    }
    return o;
}

Csv parseCsv(const std::string& t) {
    Csv c;
    // the lines (a quoted field may hold a line break)
    std::vector<std::string> lines;
    {
        std::string cur;
        bool q = false;
        for (size_t i = 0; i < t.size(); i++) {
            const char ch = t[i];
            if (ch == '"') q = !q;
            if (!q && (ch == '\n' || ch == '\r')) {
                if (ch == '\r' && i + 1 < t.size() && t[i + 1] == '\n') i++;
                if (!trim(cur).empty()) lines.push_back(cur);
                cur.clear();
                continue;
            }
            cur += ch;
        }
        if (!trim(cur).empty()) lines.push_back(cur);
    }
    if (lines.empty()) return c;
    // the delimiter: the one that splits the first lines into the same number of fields (more than one), most often
    const char kDelims[] = {';', '\t', ',', '|'};
    int bestScore = -1;
    for (char d : kDelims) {
        std::map<int, int> counts;
        const size_t n = std::min<size_t>(lines.size(), 40);
        for (size_t i = 0; i < n; i++) {
            int k = 0;
            bool q = false;
            for (char ch : lines[i]) { if (ch == '"') q = !q; else if (!q && ch == d) k++; }
            counts[k]++;
        }
        int mode = 0, modeN = 0;
        for (const auto& kv : counts) if (kv.second > modeN || (kv.second == modeN && kv.first > mode)) { mode = kv.first; modeN = kv.second; }
        if (mode == 0) continue;
        const int score = modeN * 100 + std::min(mode, 99);
        if (score > bestScore) { bestScore = score; c.delim = d; }
    }
    if (bestScore < 0) c.delim = '\t';
    for (const std::string& l : lines) {
        std::vector<std::string> f;
        std::string cur;
        bool q = false;
        for (size_t i = 0; i < l.size(); i++) {
            const char ch = l[i];
            if (ch == '"') {
                if (q && i + 1 < l.size() && l[i + 1] == '"') { cur += '"'; i++; }
                else q = !q;
            } else if (!q && ch == c.delim) { f.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        f.push_back(cur);
        c.rows.push_back(std::move(f));
    }
    // a header: the first line has no number at all and a later line has some
    const std::vector<std::string>& r0 = c.rows[0];
    bool anyNum = false;
    for (const std::string& f : r0) { int v; if (toInt(f, v)) anyNum = true; }
    bool laterNum = false;
    for (size_t i = 1; i < c.rows.size() && i < 10; i++) for (const std::string& f : c.rows[i]) { int v; if (toInt(f, v)) laterNum = true; }
    if (!anyNum && (laterNum || c.rows.size() == 1)) {
        c.header = true;
        for (const std::string& f : r0) c.cols.push_back(trim(f));
        c.rows.erase(c.rows.begin());
    }
    return c;
}

// ---------------------------------------------------------------------------------------------------- the database

void Database::clear() { *this = Database(); }

ImportReport Database::importFile(const std::string& path) {
    ImportReport r;
    FILE* f = u8fopen(path, "rb");
    if (!f) { r.message = "Cannot open " + path; return r; }
    std::string raw;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { raw.append(buf, n); if (raw.size() > (256u << 20)) break; }
    fclose(f);
    std::string name = path;
    const size_t sl = name.find_last_of("/\\");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    r = importText(raw, name);
    if (r.ok) sources_.push_back(name);
    return r;
}

ImportReport Database::importText(const std::string& raw, const std::string& nameHint) {
    ImportReport r;
    const Csv c = parseCsv(toUtf8(raw));
    r.delim = c.delim;
    if (c.rows.empty()) { r.message = "The file is empty."; return r; }
    std::vector<std::string> norm;
    for (const std::string& h : c.cols) norm.push_back(normCol(h));
    auto has = [&](const char* n) { return std::find(norm.begin(), norm.end(), n) != norm.end(); };
    std::string base = lower(nameHint);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    base = normCol(base);
    // TISA exchange format: by the file's name, else by its columns
    if (c.header && has("lcd")) {
        std::string kind;
        if (base.find("poffset") != std::string::npos || base.find("soffset") != std::string::npos || (has("negofflcd") && has("posofflcd"))) kind = "offsets";
        else if (base.find("segment") != std::string::npos || (has("roalcd") && has("roadnumber") && !has("junctionnumber"))) kind = "segments";
        else if (base.find("road") != std::string::npos || (has("roadnumber") && has("peslev"))) kind = "roads";
        else if (base.find("point") != std::string::npos || has("junctionnumber") || has("inpos")) kind = "points";
        else if (base.find("area") != std::string::npos || (has("nid") && has("class"))) kind = "areas";
        else if (has("roadnumber") && has("class") && has("rnid")) kind = "roads";
        if (!kind.empty() && (has("class") || kind == "offsets") && !has("firstname")) return importTisa(c, kind);
    }
    if (c.header && has("nid") && has("name") && !has("lcd")) return importTisa(c, "names");
    // a location list in one file
    const int lc = findCol(norm, {"locationcode", "lcd", "locationnumber", "loccode", "location", "lc", "lcdcode", "lokationscode", "ortscode"});
    const bool locCols = findCol(norm, {"firstname", "name1", "roadnumber", "negativeoffset", "positiveoffset", "negoffset", "posoffset", "negoff", "posoff"}) >= 0;
    if (c.header && lc >= 0 && locCols) return importLcl(c);
    return importEvents(c, nameHint);
}

ImportReport Database::importEvents(const Csv& c, const std::string& nameHint) {
    ImportReport r;
    r.delim = c.delim;
    std::vector<std::string> norm;
    for (const std::string& h : c.cols) norm.push_back(normCol(h));
    const size_t ncols = c.header ? c.cols.size() : (c.rows.empty() ? 0 : c.rows[0].size());
    // the code column: by name, else the first column of whole numbers 1 - 2047
    int code = c.header ? findCol(norm, {"code", "ecd", "eventcode", "evtcode", "eventnr", "eventnumber", "eventno", "nr", "no", "number", "nummer", "ereigniscode",
                                         "codenr", "id", "eventid", "cd", "evcode", "tmccode", "meldungscode"}, {"code", "eventcode"}) : -1;
    auto numericShare = [&](size_t col, bool tmcRange) {
        int ok = 0, all = 0;
        for (size_t i = 0; i < c.rows.size() && i < 200; i++) {
            const std::string f = field(c.rows[i], (int)col);
            if (f.empty()) continue;
            all++;
            int v;
            if (toInt(f, v) && (!tmcRange || (v >= 1 && v <= 2047))) ok++;
        }
        return all ? (double)ok / all : 0.0;
    };
    if (code < 0) for (size_t k = 0; k < ncols; k++) if (numericShare(k, true) > 0.9) { code = (int)k; break; }
    if (code < 0) { r.message = "No column of event codes (1 - 2047) was found, and no location columns either: this does not look like a TMC event list or location table."; return r; }
    // text columns: by name, else every column that holds words
    struct TC { int col; std::string lang; int prio; };
    std::vector<TC> tcs;
    static const char* const kExact[] = {"desc", "ereignis", "event", "name", "bezeichnung", "en", "de", "eng", "deu", "ger"};
    static const char* const kPart[] = {"text", "description", "beschreibung", "meldung", "wording", "phrase", "english", "englisch", "deutsch", "german"};
    const std::string fileLang = langOfName(nameHint);
    for (size_t k = 0; k < ncols; k++) {
        if ((int)k == code) continue;
        bool named = false;
        int prio = 0;
        if (c.header) {
            const std::string& n = norm[k];
            for (const char* t : kExact) if (n == t) named = true;
            for (const char* t : kPart) if (n.find(t) != std::string::npos) named = true;
            // quantifier variants ("Text (Q)", "Text Q>1", "description with quantifier"): only when nothing else is there
            if (n.find("quant") != std::string::npos || (n.size() > 1 && n.back() == 'q') || n.find("q1") != std::string::npos || n.find("plural") != std::string::npos) prio = 1;
            if (!named && !langOfName(c.cols[k]).empty() && n.size() <= 8) named = true;
        }
        if (!named) {   // no name or a name that says nothing: words in most rows, longer than a letter or two
            int words = 0, all = 0;
            size_t len = 0;
            for (size_t i = 0; i < c.rows.size() && i < 200; i++) {
                const std::string f = field(c.rows[i], (int)k);
                if (f.empty()) continue;
                all++;
                if (hasLetters(f)) { words++; len += f.size(); }
            }
            if (!(all && words > all * 0.8 && len / std::max(1, words) >= 5)) continue;
            if (c.header && findCol({norm[k]}, {"n", "q", "t", "d", "u", "c", "r", "nature", "urgency", "class", "updateclass", "reference", "type", "directionality", "duration", "durationtype", "dauer"}) >= 0) continue;
        }
        std::string lang = c.header ? langOfName(c.cols[k]) : "";
        if (lang.empty()) {
            std::vector<std::string> sample;
            for (size_t i = 0; i < c.rows.size() && sample.size() < 60; i++) { const std::string f = field(c.rows[i], (int)k); if (!f.empty()) sample.push_back(f); }
            const std::string byText = langOfText(sample);
            lang = !fileLang.empty() && (byText == "xx" || byText == fileLang) ? fileLang : byText;
        }
        tcs.push_back({(int)k, lang, prio});
    }
    // one column per language: the plain wording before the quantifier ones, the first of each
    std::map<std::string, TC> perLang;
    for (const TC& t : tcs) { auto it = perLang.find(t.lang); if (it == perLang.end() || t.prio < it->second.prio) perLang[t.lang] = t; }
    if (perLang.empty()) { r.message = "An event code column was found but no column of event texts."; return r; }
    const int durCol = c.header ? findCol(norm, {"t", "durationtype", "durtype", "dauertyp", "duration", "dauer", "durationandpersistence"}) : -1;
    int rows = 0;
    for (const auto& row : c.rows) {
        int v;
        if (!toInt(field(row, code), v) || v < 1 || v > 2047) continue;
        Event& e = events_[v];
        e.code = v;
        bool any = false;
        for (const auto& kv : perLang) {
            const std::string t = fold(field(row, kv.second.col));
            if (!t.empty()) { e.text[kv.first] = t; any = true; }
        }
        const std::string d = field(row, durCol);
        if (!d.empty()) { const char ch = (char)std::toupper((unsigned char)d[0]); if (ch == 'D' || ch == 'L') e.durType = ch; }
        if (any) rows++;
    }
    for (const auto& kv : perLang) r.languages.push_back(kv.first);
    r.ok = rows > 0;
    r.rows = rows;
    r.kind = "event list";
    std::string langs;
    for (const std::string& l : r.languages) langs += (langs.empty() ? "" : ", ") + l;
    r.message = r.ok ? "Event list: " + std::to_string(rows) + " events (" + langs + ")." : "No events were read.";
    return r;
}

ImportReport Database::importLcl(const Csv& c) {
    ImportReport r;
    r.delim = c.delim;
    std::vector<std::string> norm;
    for (const std::string& h : c.cols) norm.push_back(normCol(h));
    const int lc = findCol(norm, {"locationcode", "lcd", "locationnumber", "loccode", "location", "lc", "lcdcode", "lokationscode", "ortscode"});
    const int ty = findCol(norm, {"type", "locationtype", "typ"}), st = findCol(norm, {"subtype", "subtyp", "stcd"});
    const int cls = findCol(norm, {"class"}), tcd = findCol(norm, {"tcd"});
    const int rn = findCol(norm, {"roadnumber", "roadno", "roadnr", "road", "strassennummer", "strassennr", "strasse"});
    const int rname = findCol(norm, {"roadname", "strassenname"});
    const int n1 = findCol(norm, {"firstname", "name1", "first", "name"}), n2 = findCol(norm, {"secondname", "name2", "second"});
    const int ar = findCol(norm, {"areareference", "arearef", "area", "pollcd", "areacode"});
    const int lin = findCol(norm, {"linearreference", "linref", "linear", "seglcd", "linearcode"}), roa = findCol(norm, {"roalcd"});
    const int neg = findCol(norm, {"negativeoffset", "negoffset", "negoff", "negofflcd", "negative"});
    const int pos = findCol(norm, {"positiveoffset", "posoffset", "posoff", "posofflcd", "positive"});
    const int ex = findCol(norm, {"exitnumber", "junctionnumber", "exit", "junction", "anschlussnummer", "anschlussstellennummer"});
    const int cidc = findCol(norm, {"cid", "countryid"}), tabc = findCol(norm, {"tabcd", "tablecode", "ltn", "tablenumber"});
    int rows = 0;
    for (const auto& row : c.rows) {
        int code;
        if (!toInt(field(row, lc), code) || code <= 0) continue;
        Location& l = locs_[code];
        l.code = code;
        std::string t = field(row, ty);
        if (t.empty() && cls >= 0) t = field(row, cls) + field(row, tcd);
        const std::string s = field(row, st);
        if (!t.empty() && t.find('.') == std::string::npos && !s.empty()) t += "." + s;
        if (!t.empty()) l.type = t;
        if (rn >= 0) l.roadNumber = field(row, rn);
        if (rname >= 0) l.roadName = field(row, rname);
        if (n1 >= 0) l.firstName = field(row, n1);
        if (n2 >= 0) l.secondName = field(row, n2);
        if (ex >= 0) l.exitNumber = field(row, ex);
        l.area = intOr0(field(row, ar));
        l.linear = intOr0(field(row, lin));
        if (!l.linear) l.linear = intOr0(field(row, roa));
        l.negOff = intOr0(field(row, neg));
        l.posOff = intOr0(field(row, pos));
        if (tabc >= 0 && r.tabcd < 0) toInt(field(row, tabc), r.tabcd);
        if (cidc >= 0 && r.cid < 0) toInt(field(row, cidc), r.cid);
        rows++;
    }
    if (r.tabcd >= 0) tabcd_ = r.tabcd;
    r.ok = rows > 0;
    r.rows = rows;
    r.kind = "location table";
    r.message = r.ok ? "Location table: " + std::to_string(rows) + " locations" + (r.tabcd >= 0 ? " (table " + std::to_string(r.tabcd) + ")" : std::string()) + "." : "No locations were read.";
    return r;
}

ImportReport Database::importTisa(const Csv& c, const std::string& kind) {
    ImportReport r;
    r.delim = c.delim;
    std::vector<std::string> norm;
    for (const std::string& h : c.cols) norm.push_back(normCol(h));
    auto col = [&](const char* n) { return findCol(norm, {n}); };
    const int lcd = col("lcd"), cls = col("class"), tcd = col("tcd"), stcd = col("stcd"), tab = col("tabcd"), cid = col("cid");
    int rows = 0;
    if (kind == "names") {
        const int nid = col("nid"), name = col("name");
        for (const auto& row : c.rows) {
            int id;
            if (!toInt(field(row, nid), id)) continue;
            if (!names_.count(id)) { names_[id] = field(row, name); rows++; }   // several languages: the first is kept
        }
    } else if (kind == "offsets") {
        const int neg = col("negofflcd"), pos = col("posofflcd");
        for (const auto& row : c.rows) {
            int code;
            if (!toInt(field(row, lcd), code) || code <= 0) continue;
            Location& l = locs_[code];
            l.code = code;
            l.negOff = intOr0(field(row, neg));
            l.posOff = intOr0(field(row, pos));
            rows++;
        }
    } else {
        const int jn = col("junctionnumber"), rnid = col("rnid"), n1 = col("n1id"), n2 = col("n2id"), nid = col("nid"), rnum = col("roadnumber");
        const int pol = col("pollcd"), seg = col("seglcd"), roa = col("roalcd");
        for (const auto& row : c.rows) {
            int code;
            if (!toInt(field(row, lcd), code) || code <= 0) continue;
            Location& l = locs_[code];
            l.code = code;
            const std::string cl = field(row, cls), t = field(row, tcd), s = field(row, stcd);
            if (!cl.empty()) l.type = cl + t + (s.empty() ? "" : "." + s);
            if (rnum >= 0) l.roadNumber = field(row, rnum);
            if (jn >= 0) l.exitNumber = field(row, jn);
            l.area = intOr0(field(row, pol));
            l.linear = intOr0(field(row, seg));
            if (!l.linear) l.linear = intOr0(field(row, roa));
            NameRef& nr = nameRefs_[code];
            nr.n1 = intOr0(field(row, n1 >= 0 ? n1 : nid));
            nr.n2 = intOr0(field(row, n2));
            nr.rn = intOr0(field(row, rnid));
            if (tab >= 0 && r.tabcd < 0) toInt(field(row, tab), r.tabcd);
            if (cid >= 0 && r.cid < 0) toInt(field(row, cid), r.cid);
            rows++;
        }
    }
    if (r.tabcd >= 0) tabcd_ = r.tabcd;
    resolveNames();
    r.ok = rows > 0;
    r.rows = rows;
    r.kind = "TISA " + kind;
    r.message = r.ok ? "Location table (TISA " + kind + "): " + std::to_string(rows) + " records." : "No records were read.";
    return r;
}

void Database::resolveNames() {
    if (names_.empty()) return;
    for (const auto& kv : nameRefs_) {
        auto it = locs_.find(kv.first);
        if (it == locs_.end()) continue;
        Location& l = it->second;
        auto nm = [&](int id) { auto f = names_.find(id); return f == names_.end() ? std::string() : f->second; };
        if (kv.second.n1) l.firstName = nm(kv.second.n1);
        if (kv.second.n2) l.secondName = nm(kv.second.n2);
        if (kv.second.rn) l.roadName = nm(kv.second.rn);
    }
}

std::vector<std::string> Database::languages() const {
    std::map<std::string, int> n;
    for (const auto& kv : events_) for (const auto& t : kv.second.text) n[t.first]++;
    std::vector<std::pair<int, std::string>> v;
    for (const auto& kv : n) v.push_back({-kv.second, kv.first});
    std::sort(v.begin(), v.end());
    std::vector<std::string> o;
    for (const auto& p : v) o.push_back(p.second);
    return o;
}

const Event* Database::event(int code) const {
    auto it = events_.find(code);
    return it == events_.end() || it->second.text.empty() ? nullptr : &it->second;
}

std::string Database::eventText(int code, const std::string& lang) const {
    const Event* e = event(code);
    if (!e) return "";
    auto it = e->text.end();
    if (!lang.empty() && lang != "auto") it = e->text.find(lang);
    if (it == e->text.end()) it = e->text.find("en");
    if (it == e->text.end()) it = e->text.begin();
    return stripQ(it->second);
}

const Location* Database::location(int code) const {
    auto it = locs_.find(code);
    return it == locs_.end() ? nullptr : &it->second;
}

void Database::roadOf(const Location& l, std::string& number, std::string& name, std::string& end1, std::string& end2) const {
    number = l.roadNumber; name = l.roadName; end1.clear(); end2.clear();
    if (l.isLinear()) { end1 = l.firstName; end2 = l.secondName; }
    const Location* p = &l;
    for (int i = 0; i < 4 && p->linear && p->linear != p->code; i++) {
        const Location* q = location(p->linear);
        if (!q) break;
        if (number.empty()) number = q->roadNumber;
        if (name.empty()) name = q->roadName;
        if (end1.empty() && end2.empty()) { end1 = q->firstName; end2 = q->secondName; }
        p = q;
    }
}

int Database::follow(int code, int steps, bool positive) const {
    int c = code;
    for (int i = 0; i < steps; i++) {
        const Location* l = location(c);
        if (!l) return 0;
        const int nx = positive ? l->posOff : l->negOff;
        if (nx <= 0) return 0;
        c = nx;
    }
    return c;
}

// ---------------------------------------------------------------------------------------------------- in words

namespace {
std::string locName(const Location& l) {
    std::string s = l.firstName;
    if (l.isLinear() || (!l.isPoint() && !l.secondName.empty())) {
        if (!l.secondName.empty()) s += (s.empty() ? "" : " - ") + l.secondName;
    }
    if (s.empty()) s = "location " + std::to_string(l.code);
    if (!l.exitNumber.empty()) s += " (" + l.exitNumber + ")";
    return s;
}
}

Plain describe(const TmcMessage& m, const Database* db, const std::string& lang) {
    Plain p;
    auto evText = [&](int code, bool* known) {
        std::string t = db ? db->eventText(code, lang) : "";
        if (!t.empty()) { if (known) *known = true; return t; }
        if (TmcDecoder::eventKnown(code)) { if (known) *known = true; return TmcDecoder::eventText(code); }
        if (known) *known = false;
        return "event " + std::to_string(code) + " (?)";
    };
    p.event = evText(m.event, &p.eventKnown);
    auto locText = [&](int code) {
        const Location* l = db ? db->location(code) : nullptr;
        return l ? locName(*l) : "location " + std::to_string(code) + " (?)";
    };
    // details: from the numbers when the decoder kept them, else its English text
    if (!m.details.empty()) {
        for (const TmcMessage::Detail& d : m.details) {
            const int v = d.value;
            switch (d.label) {
            case 2: p.details.push_back(fmtInt("length of route affected %d km", v <= 10 ? v : v <= 15 ? 10 + (v - 10) * 2 : 20 + (v - 15) * 5)); break;
            case 3: p.details.push_back(fmtInt("speed limit %d km/h", v * 5)); break;
            case 9: if (v) p.details.push_back(evText(v, nullptr)); break;
            case 10: p.details.push_back("diversion via " + locText(v)); break;
            case 11: p.details.push_back("destination " + locText(v)); break;
            default: break;
            }
        }
    } else p.details = m.more;
    const Location* l = db ? db->location(m.location) : nullptr;
    p.locationKnown = l != nullptr;
    p.location = locText(m.location);
    std::string e1, e2;
    if (l) {
        std::string num, name;
        db->roadOf(*l, num, name, e1, e2);
        p.road = num;
        if (!name.empty() && name != num) p.road += (p.road.empty() ? "" : " ") + name;
    }
    const std::string& toward = m.negative ? e1 : e2;
    p.direction = !toward.empty() ? "towards " + toward : m.negative ? "negative direction" : "positive direction";
    if (m.extent > 0) {
        // the secondary location: extent steps from the primary one against the direction of travel (ISO 14819-1), so the
        // negative offsets for traffic in the positive direction
        const int sec = db ? db->follow(m.location, m.extent, m.negative) : 0;
        if (sec) p.extent = "from " + locText(sec) + " (" + std::to_string(m.extent) + (m.extent == 1 ? " location)" : " locations)");
        else p.extent = "+" + std::to_string(m.extent) + (m.extent == 1 ? " location" : " locations");
    }
    p.duration = m.durationText;
    if (db && m.durationCode) {
        const Event* e = db->event(m.event);
        if (e && e->durType) p.duration = TmcDecoder::durationText(m.durationCode, e->durType == 'L');
    }
    if (m.diversion) p.diversion = "diversion advised";
    return p;
}

std::string Plain::line() const {
    std::string s = road;
    s += (s.empty() ? "" : " ") + location;
    s += ", " + direction + ": " + event;
    for (const std::string& d : details) s += "; " + d;
    if (!extent.empty()) s += "; " + extent;
    if (!duration.empty()) s += "; " + duration;
    if (!diversion.empty()) s += "; " + diversion;
    return s;
}

// ---------------------------------------------------------------------------------------------------- the log

namespace {
std::string csvq(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
std::string keyOf(const TmcMessage& m) {
    char b[128];
    snprintf(b, sizeof b, "%d|%d|%d|%d|%d|%d|%.0f|", m.event, m.location, (int)m.negative, m.extent, (int)m.diversion, m.durationCode, m.received);
    std::string k = b;
    for (const std::string& s : m.more) k += s + "|";
    return k;
}
std::string piText(int pi) { char b[8]; snprintf(b, sizeof b, "%04X", pi & 0xFFFF); return pi ? b : ""; }
}

Log::~Log() {
    if (csv_) fclose(csv_);
    if (txt_) fclose(txt_);
}

void Log::setPath(const std::string& basePath) {
    if (basePath == base_) return;
    if (csv_) { fclose(csv_); csv_ = nullptr; }
    if (txt_) { fclose(txt_); txt_ = nullptr; }
    base_ = basePath;
    err_.clear();
}

bool Log::open() {
    if (csv_ && txt_) return true;
    std::error_code ec;
    const std::filesystem::path dir = u8file(base_).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    if (!csv_) {
        csv_ = u8fopen(base_ + ".csv", "ab");
        if (csv_) { fseek(csv_, 0, SEEK_END); if (ftell(csv_) == 0) { const std::string h = csvHeader(); fwrite(h.data(), 1, h.size(), csv_); } }
    }
    if (!txt_) txt_ = u8fopen(base_ + ".txt", "ab");
    if (!csv_ || !txt_) { err_ = "cannot write " + base_ + ".csv / .txt"; return false; }
    return true;
}

std::string Log::csvHeader() {
    return "time_utc,freq_mhz,pi,ps,ltn,sid,event_code,event,location_code,location,road,direction,extent,extent_text,duration,diversion,details\n";
}

std::string Log::csvRow(const TmcMessage& m, const Plain& p, const Station& st) {
    char f[32];
    snprintf(f, sizeof f, "%.2f", st.freqMhz);
    std::string det;
    for (const std::string& d : p.details) det += (det.empty() ? "" : " | ") + d;
    std::string r = utcIsoText((int64_t)m.received) + "," + (st.freqMhz > 0 ? f : "") + "," + piText(st.pi) + "," + csvq(st.ps) + "," +
                    (st.ltn >= 0 ? std::to_string(st.ltn) : "") + "," + (st.sid >= 0 ? std::to_string(st.sid) : "") + "," + std::to_string(m.event) + "," +
                    csvq(p.event) + "," + std::to_string(m.location) + "," + csvq(p.location) + "," + csvq(p.road) + "," + csvq(p.direction) + "," +
                    std::to_string(m.extent) + "," + csvq(p.extent) + "," + csvq(p.duration) + "," + (m.diversion ? "yes" : "no") + "," + csvq(det) + "\n";
    return r;
}

std::string Log::textLine(const TmcMessage& m, const Plain& p, const Station& st) {
    char f[48] = "";
    if (st.freqMhz > 0) snprintf(f, sizeof f, " %.2f MHz", st.freqMhz);
    std::string ps = st.ps;
    while (!ps.empty() && ps.back() == ' ') ps.pop_back();
    return utcIsoText((int64_t)m.received) + f + (ps.empty() ? "" : " " + ps) + (st.pi ? " (" + piText(st.pi) + ")" : "") + ": " + p.line() + "\n";
}

int Log::update(const std::vector<TmcMessage>& msgs, const Station& st, const Database* db, const std::string& lang) {
    if (!enabled()) return 0;
    int n = 0;
    // oldest first, so the file reads in order
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        const std::string k = keyOf(*it);
        if (seen_.count(k)) continue;
        if (!open()) return n;
        const Plain p = describe(*it, db, lang);
        const std::string row = csvRow(*it, p, st), line = textLine(*it, p, st);
        fwrite(row.data(), 1, row.size(), csv_);
        fwrite(line.data(), 1, line.size(), txt_);
        seen_.insert(k); order_.push_back(k);
        while (order_.size() > 2048) { seen_.erase(order_.front()); order_.pop_front(); }
        n++;
    }
    if (n) { fflush(csv_); fflush(txt_); }
    return n;
}

ImportReport loadFolder(Database& db, const std::string& folder) {
    ImportReport sum;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(u8file(folder), ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = lower(e.path().extension().string());
        if (ext == ".csv" || ext == ".txt" || ext == ".dat" || ext == ".tsv") files.push_back(e.path());
    }
    // names last: the TISA name ids are resolved against every location read before
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        const bool na = lower(a.filename().string()).find("name") != std::string::npos, nb = lower(b.filename().string()).find("name") != std::string::npos;
        return na != nb ? nb : a < b;
    });
    for (const auto& f : files) {
        const std::u8string u = f.u8string();
        const ImportReport r = db.importFile(std::string(u.begin(), u.end()));
        if (r.ok) { sum.ok = true; sum.rows += r.rows; }
    }
    sum.message = std::to_string(db.eventCount()) + " events, " + std::to_string(db.locationCount()) + " locations";
    return sum;
}

} // namespace tmcdb
} // namespace dect2
