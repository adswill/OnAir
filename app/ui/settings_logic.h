// The Settings window's data and rules, with no ImGui and no App, so they are unit-tested (tests/app/test_settings.cpp).
// One table of Info rows: the category, the label, the search words, the kind, the default and the limits. The window binds each
// row to the field that holds the value (settings_window.cpp); a row without a binding is not shown, so there is no dead toggle.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace settings {

enum class Kind { Bool, Int, Choice, Float, Text, Custom, Link };

struct Info {
    const char* key;        // the stored name (the prefs key); unique; never renamed
    const char* cat;        // category name
    const char* label;
    const char* tip;        // one sentence of help
    const char* words;      // extra search words
    Kind kind;
    double def = 0;         // default (Bool 0/1, Choice index, Int, Float)
    double lo = 0, hi = 0;  // Int / Float limits (hi < lo: none)
    const char* choices = nullptr;   // Choice: "A|B|C"
    const char* values = nullptr;    // Choice: "0,50,75" stored numbers when they are not the indexes
    const char* defText = "";        // Text default
    bool advanced = false;  // under the category's Advanced foldout
    bool restart = false;   // takes effect after a restart
    const char* fmt = "%.2f";
};

// the value of one setting as the window and the export see it
struct Val { double d = 0; std::string s; };

inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// "a|b|c" -> pieces
inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b <= s.size()) {
        size_t e = s.find(sep, b);
        if (e == std::string::npos) e = s.size();
        out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// Search: every word of the query must occur in the label, the help, the search words, the category or the key.
inline bool matches(const Info& i, const std::string& query) {
    if (query.empty()) return true;
    const std::string hay = lower(std::string(i.label) + " " + i.tip + " " + i.words + " " + i.cat + " " + i.key);
    for (const std::string& w : split(lower(query), ' ')) if (!w.empty() && hay.find(w) == std::string::npos) return false;
    return true;
}

// the index into the rows of the Info that match; empty query: all (of one category when cat >= 0 and no query)
inline std::vector<size_t> filter(const std::vector<Info>& t, const std::string& query, const std::string& cat) {
    std::vector<size_t> out;
    for (size_t k = 0; k < t.size(); k++) {
        if (query.empty() && !cat.empty() && cat != t[k].cat) continue;   // a search looks across every category
        if (matches(t[k], query)) out.push_back(k);
    }
    return out;
}

// the categories in the order of first use
inline std::vector<std::string> categories(const std::vector<Info>& t) {
    std::vector<std::string> out;
    for (const Info& i : t) if (std::find(out.begin(), out.end(), i.cat) == out.end()) out.push_back(i.cat);
    return out;
}

inline double clampTo(const Info& i, double v) {
    if (i.kind == Kind::Bool) return v != 0 ? 1 : 0;
    if (i.kind == Kind::Choice && i.choices && !i.values) {
        const int n = (int)split(i.choices, '|').size();
        return std::max(0.0, std::min((double)(n - 1), std::floor(v + 0.5)));
    }
    if (i.hi >= i.lo && (i.kind == Kind::Int || i.kind == Kind::Float)) v = std::max(i.lo, std::min(i.hi, v));
    if (i.kind == Kind::Int) v = std::floor(v + 0.5);
    return v;
}

inline bool isDefault(const Info& i, const Val& v) {
    if (i.kind == Kind::Text) return v.s == i.defText;
    if (i.kind == Kind::Custom || i.kind == Kind::Link) return true;
    return std::fabs(v.d - i.def) < 1e-6;
}

// choice index <-> stored number (the Choice rows with `values`)
inline int choiceIndex(const Info& i, double v) {
    if (!i.values) return (int)v;
    const std::vector<std::string> p = split(i.values, ',');
    for (size_t k = 0; k < p.size(); k++) if (std::atof(p[k].c_str()) == v) return (int)k;
    return 0;
}
inline double choiceValue(const Info& i, int idx) {
    if (!i.values) return idx;
    const std::vector<std::string> p = split(i.values, ',');
    return idx >= 0 && (size_t)idx < p.size() ? std::atof(p[(size_t)idx].c_str()) : i.def;
}

// Export / import: one "key=value" per line, text values with line breaks escaped. Unknown keys and bad numbers are ignored; a number is clamped.
inline std::string escapeText(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else o += c; }
    return o;
}
inline std::string unescapeText(const std::string& s) {
    std::string o;
    for (size_t k = 0; k < s.size(); k++) {
        if (s[k] == '\\' && k + 1 < s.size()) { const char n = s[++k]; o += n == 'n' ? '\n' : n == 'r' ? '\r' : n; }
        else o += s[k];
    }
    return o;
}
inline std::string exportText(const std::vector<Info>& t, const std::vector<Val>& v) {
    std::string o = "# OnAir settings\n";
    for (size_t k = 0; k < t.size() && k < v.size(); k++) {
        if (t[k].kind == Kind::Custom || t[k].kind == Kind::Link) continue;
        char b[64];
        o += t[k].key; o += '=';
        if (t[k].kind == Kind::Text) o += escapeText(v[k].s);
        else { snprintf(b, sizeof b, "%.9g", v[k].d); o += b; }
        o += '\n';
    }
    return o;
}
// returns how many settings were read; `v` keeps its old value for a key that is missing
inline int importText(const std::vector<Info>& t, std::vector<Val>& v, const std::string& text) {
    std::map<std::string, size_t> at;
    for (size_t k = 0; k < t.size(); k++) at[t[k].key] = k;
    int n = 0;
    for (const std::string& line : split(text, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto f = at.find(line.substr(0, eq));
        if (f == at.end() || f->second >= v.size()) continue;
        const Info& i = t[f->second];
        if (i.kind == Kind::Custom || i.kind == Kind::Link) continue;
        const std::string val = line.substr(eq + 1);
        if (i.kind == Kind::Text) { v[f->second].s = unescapeText(val); n++; continue; }
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        if (end == val.c_str()) continue;
        v[f->second].d = clampTo(i, d);
        n++;
    }
    return n;
}

inline Val defaultOf(const Info& i) { Val v; v.d = i.def; v.s = i.defText; return v; }

// how many rows of a category differ from their default (the dot beside the category)
inline int changedCount(const std::vector<Info>& t, const std::vector<Val>& v, const std::string& cat) {
    int n = 0;
    for (size_t k = 0; k < t.size() && k < v.size(); k++) if (cat == t[k].cat && !isDefault(t[k], v[k])) n++;
    return n;
}

}   // namespace settings
